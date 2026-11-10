// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_board.hpp"

using namespace hackrfduplex;
using hackrfduplex::test::FakeBoard;
using namespace std::chrono_literals;

namespace {

struct Rig {
    std::unique_ptr<FakeBoard> rx_owner =
        std::make_unique<FakeBoard>("00000000000000000000000000000001");
    std::unique_ptr<FakeBoard> tx_owner =
        std::make_unique<FakeBoard>("00000000000000000000000000000002");
    FakeBoard& rx = *rx_owner;
    FakeBoard& tx = *tx_owner;
    std::vector<std::string> logs;
    Pair pair{std::move(rx_owner), std::move(tx_owner),
              [this](LogLevel, const std::string& message) { logs.push_back(message); }};
};

}  // namespace

TEST_CASE("a pair needs two different boards", "[pair]") {
    CHECK_THROWS_AS(Pair(nullptr, std::make_unique<FakeBoard>(), nullptr), std::invalid_argument);
    CHECK_THROWS_AS(
        Pair(std::make_unique<FakeBoard>("a"), std::make_unique<FakeBoard>("a"), nullptr),
        std::invalid_argument);
}

TEST_CASE("opening a pair puts both boards in a known gain state", "[pair]") {
    Rig rig;
    CHECK(rig.rx.lna_db == 16);
    CHECK(rig.rx.vga_db == 16);
    CHECK_FALSE(rig.rx.amp);
    CHECK(rig.tx.tx_vga_db == 0);
    CHECK_FALSE(rig.tx.amp);
    REQUIRE_FALSE(rig.logs.empty());
    CHECK(rig.logs.front().find("pair opened") != std::string::npos);
}

TEST_CASE("each direction tunes its own board", "[pair]") {
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 1'842'500'000);
    rig.pair.set_frequency(Direction::kTx, 1'747'500'000);
    CHECK(rig.rx.frequency_hz == 1'842'500'000);
    CHECK(rig.tx.frequency_hz == 1'747'500'000);
    CHECK(rig.pair.frequency(Direction::kRx) == 1'842'500'000);
    CHECK(rig.pair.frequency(Direction::kTx) == 1'747'500'000);
}

TEST_CASE("the bandwidth follows the rate until set, then survives rate changes", "[pair]") {
    Rig rig;
    rig.pair.set_sample_rate(Direction::kRx, 8e6);
    CHECK(rig.rx.sample_rate_hz == 8e6);
    CHECK(rig.pair.bandwidth_is_auto(Direction::kRx));
    CHECK(rig.pair.bandwidth(Direction::kRx) == 6'000'000);
    CHECK(rig.rx.bandwidth_hz == 6'000'000);

    rig.pair.set_bandwidth(Direction::kRx, 3'000'000);
    CHECK_FALSE(rig.pair.bandwidth_is_auto(Direction::kRx));
    CHECK(rig.pair.bandwidth(Direction::kRx) == 2'500'000);
    CHECK(rig.rx.bandwidth_hz == 2'500'000);

    rig.pair.set_sample_rate(Direction::kRx, 10e6);
    CHECK(rig.rx.bandwidth_hz == 2'500'000);
    CHECK(rig.rx.calls.back() == "set_bandwidth 2500000");

    rig.pair.set_bandwidth(Direction::kRx, 0);
    CHECK(rig.pair.bandwidth_is_auto(Direction::kRx));
    CHECK(rig.rx.bandwidth_hz == 7'000'000);
    CHECK(rig.pair.bandwidth(Direction::kRx) == 7'000'000);

    CHECK(rig.tx.sample_rate_hz == 0);
    CHECK_THROWS_AS(rig.pair.set_sample_rate(Direction::kTx, 0), std::invalid_argument);
}

TEST_CASE("gains are quantised and an overall gain is split over the stages", "[pair]") {
    Rig rig;
    rig.pair.set_rx_gains(RxGains{20, 33, true});
    CHECK(rig.rx.lna_db == 16);
    CHECK(rig.rx.vga_db == 32);
    CHECK(rig.rx.amp);
    CHECK(rig.pair.overall_gain(Direction::kRx) == 16 + 32 + 14);

    rig.pair.set_overall_gain(Direction::kRx, 50);
    CHECK(rig.rx.lna_db == 16);
    CHECK(rig.rx.vga_db == 34);
    CHECK_FALSE(rig.rx.amp);
    CHECK(rig.pair.overall_gain(Direction::kRx) == 50);

    rig.pair.set_tx_gains(TxGains{50, false});
    CHECK(rig.tx.tx_vga_db == 47);
    rig.pair.set_overall_gain(Direction::kTx, 55);
    CHECK(rig.tx.tx_vga_db == 41);
    CHECK(rig.tx.amp);
    CHECK(rig.pair.overall_gain(Direction::kTx) == 55);
}

TEST_CASE("the bias tee is set on the transmitter and restored when it starts", "[pair]") {
    Rig rig;
    rig.pair.set_bias_tee(true);
    CHECK(rig.tx.bias);
    CHECK_FALSE(rig.rx.bias);
    rig.pair.open_stream(Direction::kTx, Format::kCs8, 2);
    rig.pair.activate(Direction::kTx);
    rig.pair.deactivate(Direction::kTx);
    CHECK_FALSE(rig.tx.bias);  // the firmware drops it when idle
    rig.pair.activate(Direction::kTx);
    CHECK(rig.tx.bias);
    const auto& calls = rig.tx.calls;
    const auto bias_at = std::find(calls.rbegin(), calls.rend(), "set_bias_tee on");
    const auto start_at = std::find(calls.rbegin(), calls.rend(), "start_tx ");
    REQUIRE(bias_at != calls.rend());
    REQUIRE(start_at != calls.rend());
    CHECK(bias_at > start_at);  // reverse iterators: the bias went first
}

TEST_CASE("a stream is opened, activated, read and closed", "[pair]") {
    Rig rig;
    CHECK_FALSE(rig.pair.stream_open(Direction::kRx));
    CHECK_THROWS_AS(rig.pair.activate(Direction::kRx), std::logic_error);
    std::array<std::uint8_t, 32> dst{};
    CHECK_THROWS_AS(rig.pair.read(dst.data(), 8, 0us), std::logic_error);

    rig.pair.open_stream(Direction::kRx, Format::kCs8, 2);
    CHECK(rig.pair.stream_open(Direction::kRx));
    CHECK(rig.pair.mtu(Direction::kRx) == 8);
    CHECK_THROWS_AS(rig.pair.open_stream(Direction::kRx, Format::kCs8, 2), std::logic_error);
    CHECK_THROWS_AS(rig.pair.open_stream(Direction::kTx, Format::kCs8, 0), std::invalid_argument);
    CHECK(rig.pair.read(dst.data(), 8, 0us).samples == 0);
    CHECK_FALSE(rig.pair.active(Direction::kRx));

    rig.pair.activate(Direction::kRx);
    CHECK(rig.pair.active(Direction::kRx));
    CHECK(rig.rx.rx_streaming);
    rig.pair.activate(Direction::kRx);  // a second activation is a no-op
    CHECK(rig.rx.starts == 1);

    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::uint8_t>(i);
    }
    rig.rx.deliver_rx(bytes);
    const RxRead read = rig.pair.read(dst.data(), 8, 0us);
    CHECK(read.samples == 8);
    CHECK(read.position == 0);
    CHECK(dst[15] == 15);
    CHECK(rig.pair.rx_received() == 8);

    rig.pair.deactivate(Direction::kRx);
    CHECK(rig.rx.stops == 1);
    CHECK_FALSE(rig.rx.rx_streaming);
    rig.pair.close_stream(Direction::kRx);
    CHECK_FALSE(rig.pair.stream_open(Direction::kRx));
    CHECK_THROWS_AS(rig.pair.read(dst.data(), 8, 0us), std::logic_error);
    CHECK(rig.pair.rx_received() == 0);
}

TEST_CASE("the transmit stream is written through to the board", "[pair]") {
    Rig rig;
    rig.pair.open_stream(Direction::kTx, Format::kCf32, 2);
    rig.pair.activate(Direction::kTx);
    CHECK(rig.tx.tx_streaming);
    const std::array<float, 16> in = {1.0f, -1.0f, 0.5f, 0.0f};
    CHECK(rig.pair.write(in.data(), 8, false, 0us) == 8);
    const auto out = rig.tx.pull_tx(16);
    CHECK(static_cast<std::int8_t>(out[0]) == 127);
    CHECK(static_cast<std::int8_t>(out[1]) == -128);
    CHECK(static_cast<std::int8_t>(out[2]) == 64);
    CHECK(static_cast<std::int8_t>(out[3]) == 0);
    CHECK(rig.pair.tx_sent() == 8);
    CHECK_FALSE(rig.pair.take_tx_underflow());
    rig.tx.pull_tx(16);
    CHECK(rig.pair.take_tx_underflow());
    CHECK(rig.pair.tx_underflows() == 1);
}

TEST_CASE("a board failure reaches the caller", "[pair]") {
    Rig rig;
    rig.rx.fail_on = "set_frequency";
    CHECK_THROWS_AS(rig.pair.set_frequency(Direction::kRx, 100e6), BoardError);
    rig.tx.fail_on = "start_tx";
    rig.pair.open_stream(Direction::kTx, Format::kCs8, 2);
    CHECK_THROWS_AS(rig.pair.activate(Direction::kTx), BoardError);
    CHECK_FALSE(rig.pair.active(Direction::kTx));
}

namespace {

// The calls a board saw from its first start onwards.
std::vector<std::string> calls_from_start(const FakeBoard& board) {
    const auto start = std::find_if(board.calls.begin(), board.calls.end(),
                                    [](const std::string& c) { return c.rfind("start_", 0) == 0; });
    return {start, board.calls.end()};
}

}  // namespace

TEST_CASE("a start that switches the reference retunes and restarts the board", "[pair]") {
    // The firmware picks its clock source at stream start, under synthesisers
    // tuned on the old reference, which then do not relock.
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.pair.set_frequency(Direction::kTx, 868'300'000);
    rig.rx.reference_switch_pending = true;
    rig.tx.reference_switch_pending = true;
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 2);
    rig.pair.open_stream(Direction::kTx, Format::kCs8, 2);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);

    // A step away first, because the firmware skips a tune to the frequency
    // it last applied; then back, a stop, and a second start.
    const std::string rx_start = "start_rx ";
    CHECK(calls_from_start(rig.rx) == std::vector<std::string>{rx_start, "set_frequency 893300000",
                                                               "set_frequency 892300000", "stop ",
                                                               rx_start});
    // The transmitter's underrun limit goes on before each of its starts.
    const std::string tx_start = "start_tx ";
    CHECK(calls_from_start(rig.tx) ==
          std::vector<std::string>{tx_start, "set_frequency 869300000", "set_frequency 868300000",
                                   "stop ", "set_tx_underrun_limit 8", tx_start});
    CHECK(rig.pair.active(Direction::kRx));
    CHECK(rig.pair.active(Direction::kTx));
    CHECK(rig.rx.rx_streaming);
    CHECK(rig.tx.tx_streaming);
    CHECK(rig.rx.frequency_hz == 892'300'000);

    // The restarted stream still delivers.
    std::array<std::uint8_t, 16> bytes{};
    rig.rx.deliver_rx(bytes);
    std::array<std::uint8_t, 16> dst{};
    CHECK(rig.pair.read(dst.data(), 8, 0us).samples == 8);
}

TEST_CASE("a start on the same reference neither retunes nor restarts", "[pair]") {
    // Retuning when nothing switched left a real board's receiver 1.7 kHz off.
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 2);
    rig.pair.activate(Direction::kRx);
    CHECK(calls_from_start(rig.rx) == std::vector<std::string>{"start_rx "});
    CHECK(rig.rx.starts == 1);
}

TEST_CASE("a switch on a board never tuned restarts without tuning", "[pair]") {
    Rig rig;
    rig.rx.reference_switch_pending = true;
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 2);
    rig.pair.activate(Direction::kRx);
    CHECK(calls_from_start(rig.rx) == std::vector<std::string>{"start_rx ", "stop ", "start_rx "});
}

TEST_CASE("the LO reads unlocked only while a reference switch is being handled", "[pair]") {
    // libhackrf cannot report synthesiser lock; the pair knows when its
    // synthesisers are tuned on a reference that has gone.
    Rig rig;
    CHECK(rig.pair.lo_locked(Direction::kRx));
    CHECK(rig.pair.lo_locked(Direction::kTx));
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.rx.reference_switch_pending = true;
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 2);

    std::vector<std::string> seen;
    rig.rx.on_call = [&](const std::string& call) {
        seen.push_back(call + (rig.pair.lo_locked(Direction::kRx) ? " locked" : " unlocked"));
    };
    rig.pair.activate(Direction::kRx);
    rig.rx.on_call = nullptr;

    // The board's reference is read while idle, before anything is known to
    // have switched; from the first start until the second, the switch is
    // being handled.
    CHECK(seen == std::vector<std::string>{
                      "info  locked", "start_rx  unlocked", "set_frequency 893300000 unlocked",
                      "set_frequency 892300000 unlocked", "stop  unlocked", "start_rx  unlocked"});
    CHECK(rig.pair.lo_locked(Direction::kRx));
    CHECK(rig.pair.lo_locked(Direction::kTx));
}

TEST_CASE("the LO reads locked through a start that switches nothing", "[pair]") {
    Rig rig;
    rig.pair.open_stream(Direction::kTx, Format::kCs8, 2);
    bool always_locked = true;
    rig.tx.on_call = [&](const std::string&) {
        always_locked = always_locked && rig.pair.lo_locked(Direction::kTx);
    };
    rig.pair.activate(Direction::kTx);
    rig.tx.on_call = nullptr;
    CHECK(always_locked);
    CHECK(rig.pair.lo_locked(Direction::kTx));
    rig.pair.deactivate(Direction::kTx);
    CHECK(rig.pair.lo_locked(Direction::kTx));
}

TEST_CASE("the reference clock out goes to one board at a time and is read back", "[pair]") {
    Rig rig;
    CHECK(rig.pair.clock_out() == ClockOut::kOff);

    rig.pair.set_clock_out(ClockOut::kRx);
    CHECK(rig.rx.clock_out);
    CHECK_FALSE(rig.tx.clock_out);
    CHECK(rig.pair.clock_out() == ClockOut::kRx);

    rig.pair.set_clock_out(ClockOut::kTx);
    CHECK_FALSE(rig.rx.clock_out);
    CHECK(rig.tx.clock_out);
    CHECK(rig.pair.clock_out() == ClockOut::kTx);
    // The old source went off before the new one came on.
    const auto& calls = rig.rx.calls;
    CHECK(std::find(calls.begin(), calls.end(), "set_clock_out off") != calls.end());

    rig.pair.set_clock_out(ClockOut::kOff);
    CHECK_FALSE(rig.rx.clock_out);
    CHECK_FALSE(rig.tx.clock_out);
    CHECK(rig.pair.clock_out() == ClockOut::kOff);

    // A board switched on behind the pair's back is still read back.
    rig.tx.clock_out = true;
    CHECK(rig.pair.clock_out() == ClockOut::kTx);

    CHECK(parse_clock_out("rx") == ClockOut::kRx);
    CHECK(parse_clock_out("tx") == ClockOut::kTx);
    CHECK(parse_clock_out("off") == ClockOut::kOff);
    CHECK_THROWS_AS(parse_clock_out("both"), std::invalid_argument);
    CHECK(std::string(clock_out_name(ClockOut::kRx)) == "rx");
}

TEST_CASE("destroying a pair stops what was running", "[pair]") {
    int stops = 0;
    {
        Pair pair(std::make_unique<FakeBoard>("1", &stops),
                  std::make_unique<FakeBoard>("2", &stops), nullptr);
        pair.open_stream(Direction::kRx, Format::kCs8, 2);
        pair.open_stream(Direction::kTx, Format::kCs8, 2);
        pair.activate(Direction::kRx);
        pair.activate(Direction::kTx);
    }
    CHECK(stops == 2);
}

TEST_CASE("a stream's reads and writes refuse it until it is open", "[pair]") {
    Rig rig;
    std::array<std::int8_t, 8> buffer{};
    CHECK_THROWS_AS(rig.pair.read(buffer.data(), 4, 0us), std::logic_error);
    CHECK_THROWS_AS(rig.pair.acquire_read(4, 0us), std::logic_error);
    CHECK_THROWS_AS(rig.pair.release_read(0), std::logic_error);
    CHECK_THROWS_AS(rig.pair.set_rx_window(std::nullopt, std::nullopt), std::logic_error);
    CHECK_THROWS_AS(rig.pair.write(buffer.data(), 4, false, 0us), std::logic_error);
    CHECK_THROWS_AS(rig.pair.write_at(buffer.data(), 4, false, 0, 0us), std::logic_error);
    CHECK(rig.pair.rx_slots() == 0);
    CHECK_THROWS_AS(rig.pair.rx_slot_bytes(0), std::invalid_argument);
}

TEST_CASE("a stream closed while another thread finds its board failed is left alone", "[pair]") {
    // Any reader or writer polls both boards. One that finds the receiving
    // board stopped marks it failed; meanwhile the application closes the
    // receive stream. The mark must be done before the close can free it.
    auto rx_owner = std::make_unique<FakeBoard>("00000000000000000000000000000001");
    auto tx_owner = std::make_unique<FakeBoard>("00000000000000000000000000000002");
    FakeBoard& rx = *rx_owner;
    std::mutex marking_mutex;
    std::condition_variable marking_changed;
    bool marking = false;
    Pair pair(std::move(rx_owner), std::move(tx_owner), [&](LogLevel, const std::string& message) {
        if (message.find("stopped streaming") == std::string::npos) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(marking_mutex);
            marking = true;
        }
        marking_changed.notify_all();
        // Hold the marking thread inside the mark for a while.
        std::this_thread::sleep_for(100ms);
    });
    pair.set_time_origin(TimeOrigin::kNone);
    pair.set_sample_rate(Direction::kRx, 8e6);
    pair.open_stream(Direction::kRx, Format::kCs8, 4);
    pair.activate(Direction::kRx);
    rx.rx_streaming = false;
    // The transmit side's status call polls both boards.
    std::thread poller([&] { pair.take_tx_underflow(); });
    {
        std::unique_lock<std::mutex> lock(marking_mutex);
        REQUIRE(marking_changed.wait_for(lock, 2s, [&] { return marking; }));
    }
    pair.close_stream(Direction::kRx);
    poller.join();
    CHECK_FALSE(pair.stream_open(Direction::kRx));
}

TEST_CASE("a board whose reference switch cannot be handled is left stopped", "[pair]") {
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 868'300'000);
    rig.pair.set_sample_rate(Direction::kRx, 8e6);
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 4);
    rig.rx.reference_switch_pending = true;
    // The retune on the new reference fails.
    rig.rx.fail_on = "set_frequency";
    CHECK_THROWS_AS(rig.pair.activate(Direction::kRx), BoardError);
    CHECK_FALSE(rig.rx.rx_streaming);
    CHECK_FALSE(rig.pair.active(Direction::kRx));
    // And it can be tried again.
    CHECK_NOTHROW(rig.pair.activate(Direction::kRx));
    CHECK(rig.rx.rx_streaming);
    CHECK(rig.pair.active(Direction::kRx));
}

TEST_CASE("the hardware time while receive is stopped is where the next start begins", "[pair]") {
    // Receive positions start again at 0 with each activation, so a time set
    // while stopped is the time of the first sample after the next start.
    Rig rig;
    rig.pair.set_time_origin(TimeOrigin::kNone);
    rig.pair.set_sample_rate(Direction::kRx, 8e6);
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 4);
    rig.pair.activate(Direction::kRx);
    const std::vector<std::uint8_t> transfer(rig.rx.transfer_bytes_value, 0);
    for (int k = 0; k < 3; ++k) {
        rig.rx.deliver_rx(transfer);
    }
    CHECK(rig.pair.time_ticks() == 24);
    rig.pair.deactivate(Direction::kRx);
    rig.pair.set_time_ticks(1000);
    CHECK(rig.pair.time_ticks() == 1000);
    rig.pair.activate(Direction::kRx);
    CHECK(rig.pair.time_ticks() == 1000);
    rig.rx.deliver_rx(transfer);
    CHECK(rig.pair.time_ticks() == 1008);
}

TEST_CASE("the automatic receive depth holds half a second at the rate, within bounds",
          "[pair][ring]") {
    // libhackrf's transfer: 131072 samples.
    CHECK(rx_auto_slots(0, 131072) == 15);
    CHECK(rx_auto_slots(2e6, 131072) == 15);
    CHECK(rx_auto_slots(8e6, 131072) == 31);
    CHECK(rx_auto_slots(16e6, 131072) == 62);
    CHECK(rx_auto_slots(20e6, 131072) == 77);
    // Tiny transfers stop at the ceiling.
    CHECK(rx_auto_slots(20e6, 8) == 256);
}

namespace {

// Receive transfers taken by reads before the ring runs dry.
std::size_t transfers_kept(Pair& pair, std::size_t samples) {
    std::vector<std::uint8_t> dst(samples * 2);
    std::size_t kept = 0;
    while (pair.read(dst.data(), samples, 0us).samples == samples) {
        ++kept;
    }
    return kept;
}

}  // namespace

TEST_CASE("a receive stream left to the module holds as much as its rate needs at each start",
          "[pair][ring]") {
    Rig rig;
    rig.rx.transfer_bytes_value = 262144;
    rig.pair.set_time_origin(TimeOrigin::kNone);
    rig.pair.set_sample_rate(Direction::kRx, 2e6);
    rig.pair.open_stream(Direction::kRx, Format::kCs8, std::nullopt);
    // Allocated once for the fastest rate, so its buffers never move.
    CHECK(rig.pair.rx_slots() == 77);
    const std::vector<std::uint8_t> transfer(262144, 0);

    rig.pair.activate(Direction::kRx);
    for (int k = 0; k < 80; ++k) {
        rig.rx.deliver_rx(transfer);
    }
    CHECK(transfers_kept(rig.pair, 131072) == 15);
    rig.pair.deactivate(Direction::kRx);

    // The rate is set after the stream was opened, as GNU Radio does.
    rig.pair.set_sample_rate(Direction::kRx, 16e6);
    rig.pair.activate(Direction::kRx);
    for (int k = 0; k < 80; ++k) {
        rig.rx.deliver_rx(transfer);
    }
    CHECK(transfers_kept(rig.pair, 131072) == 62);
    CHECK(rig.pair.rx_slots() == 77);
}

TEST_CASE("a receive stream given a depth keeps it at any rate", "[pair][ring]") {
    Rig rig;
    rig.rx.transfer_bytes_value = 262144;
    rig.pair.set_time_origin(TimeOrigin::kNone);
    rig.pair.set_sample_rate(Direction::kRx, 16e6);
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 4);
    CHECK(rig.pair.rx_slots() == 4);
    rig.pair.activate(Direction::kRx);
    const std::vector<std::uint8_t> transfer(262144, 0);
    for (int k = 0; k < 10; ++k) {
        rig.rx.deliver_rx(transfer);
    }
    CHECK(transfers_kept(rig.pair, 131072) == 4);
}
