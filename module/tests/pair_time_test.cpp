// SPDX-License-Identifier: GPL-3.0-or-later
//
// The common time origin and timed writes, against two fake boards with a
// thread playing the air between them.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "burst.hpp"
#include "fake_board.hpp"
#include "pair.hpp"

using namespace hackrfduplex;
using hackrfduplex::test::FakeBoard;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kTransferSamples = 1024;

struct Rig {
    Rig() {
        rx.transfer_bytes_value = kTransferSamples * 2;
        tx.transfer_bytes_value = kTransferSamples * 2;
    }
    std::unique_ptr<FakeBoard> rx_owner =
        std::make_unique<FakeBoard>("00000000000000000000000000000001");
    std::unique_ptr<FakeBoard> tx_owner =
        std::make_unique<FakeBoard>("00000000000000000000000000000002");
    FakeBoard& rx = *rx_owner;
    FakeBoard& tx = *tx_owner;
    std::vector<std::string> logs;
    std::mutex log_mutex;
    Pair pair{std::move(rx_owner), std::move(tx_owner),
              [this](LogLevel, const std::string& message) {
                  const std::lock_guard<std::mutex> lock(log_mutex);
                  logs.push_back(message);
              }};

    bool logged(const std::string& text) {
        const std::lock_guard<std::mutex> lock(log_mutex);
        return std::any_of(logs.begin(), logs.end(),
                           [&](const std::string& m) { return m.find(text) != std::string::npos; });
    }

    // Both streams open at one rate, CS8, four buffers.
    void open(double rate = 8e6) {
        pair.set_sample_rate(Direction::kRx, rate);
        pair.set_sample_rate(Direction::kTx, rate);
        pair.open_stream(Direction::kRx, Format::kCs8, 4);
        pair.open_stream(Direction::kTx, Format::kCs8, 4);
    }
};

// The air: once both boards stream, each transmitted transfer is pulled
// and delivered to the receiver `delay` samples later, both stream
// positions moving in step. Uncoupled, the receiver hears only a little
// noise. Stop it before stopping either board.
class Loopback {
public:
    Loopback(FakeBoard& tx, FakeBoard& rx, std::size_t delay)
        : tx_(tx), rx_(rx), air_(delay * 2, 0), thread_([this] { run(); }) {}
    ~Loopback() { stop(); }
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    void stop() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    std::atomic<bool> coupled{true};

private:
    void run() {
        std::uint32_t noise = 12345;
        while (!stop_) {
            if (!tx_.tx_streaming || !rx_.rx_streaming) {
                std::this_thread::sleep_for(20us);
                continue;
            }
            const std::vector<std::uint8_t> sent = tx_.pull_tx(kTransferSamples * 2);
            for (const std::uint8_t b : sent) {
                air_.push_back(coupled ? static_cast<std::int8_t>(b) : 0);
            }
            std::vector<std::uint8_t> heard(kTransferSamples * 2);
            for (std::uint8_t& b : heard) {
                noise = noise * 1103515245U + 12345U;
                const auto jitter =
                    static_cast<std::int8_t>(static_cast<int>((noise >> 16) % 7) - 3);
                b = static_cast<std::uint8_t>(static_cast<std::int8_t>(air_.front() / 4 + jitter));
                air_.pop_front();
            }
            rx_.deliver_rx(heard);
            std::this_thread::sleep_for(20us);
        }
    }

    FakeBoard& tx_;
    FakeBoard& rx_;
    std::deque<std::int8_t> air_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::vector<std::string> frequency_calls(const FakeBoard& board, std::size_t from) {
    std::vector<std::string> out;
    for (std::size_t i = from; i < board.calls.size(); ++i) {
        if (board.calls[i].rfind("set_frequency", 0) == 0) {
            out.push_back(board.calls[i]);
        }
    }
    return out;
}

}  // namespace

TEST_CASE("time origins are named burst, trigger and none", "[pair][time]") {
    CHECK(parse_time_origin("burst") == TimeOrigin::kBurst);
    CHECK(parse_time_origin("trigger") == TimeOrigin::kTrigger);
    CHECK(parse_time_origin("none") == TimeOrigin::kNone);
    CHECK_THROWS_AS(parse_time_origin("gps"), std::invalid_argument);
    CHECK(std::string(time_origin_name(TimeOrigin::kTrigger)) == "trigger");
    Rig rig;
    CHECK(rig.pair.time_origin() == TimeOrigin::kBurst);  // the default
}

TEST_CASE("the burst gives the origin: the loop's delay", "[pair][time]") {
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 868'300'000);
    rig.pair.set_frequency(Direction::kTx, 868'300'000);
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    CHECK_FALSE(rig.pair.tx_of_rx());  // one stream: nothing to tie yet
    const std::size_t tx_calls = rig.tx.calls.size();
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    // A transmit sample at position t is heard at receive position t + 300.
    CHECK(*rig.pair.tx_of_rx() == -300);
    CHECK(rig.logged("common time origin from the calibration burst"));
    // Tuned alike, so the transmitter was not retuned for the burst.
    CHECK(frequency_calls(rig.tx, tx_calls).empty());
    air.stop();
}

TEST_CASE("the transmitter is tuned to the receiver for the burst and back", "[pair][time]") {
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.pair.set_frequency(Direction::kTx, 847'300'000);
    rig.open();
    Loopback air(rig.tx, rig.rx, 1000);
    rig.pair.activate(Direction::kTx);
    const std::size_t tx_calls = rig.tx.calls.size();
    rig.pair.activate(Direction::kRx);
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(*rig.pair.tx_of_rx() == -1000);
    CHECK(frequency_calls(rig.tx, tx_calls) ==
          std::vector<std::string>{"set_frequency 892300000", "set_frequency 847300000"});
    CHECK(rig.tx.frequency_hz == 847'300'000);
    CHECK(rig.pair.frequency(Direction::kTx) == 847'300'000);
    air.stop();
}

TEST_CASE("an unheard burst leaves no origin and logs how near it came", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    air.coupled = false;
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("calibration burst was not heard (best correlation"));
    const std::array<std::int8_t, 8> samples{};
    CHECK(rig.pair.write_at(samples.data(), 4, true, 100'000, 0us).status == TimedStatus::kNoTime);
    air.stop();
}

TEST_CASE("the two directions at different rates share no origin", "[pair][time]") {
    Rig rig;
    rig.open();
    rig.pair.set_sample_rate(Direction::kTx, 4e6);
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("one sample rate"));
    air.stop();
}

TEST_CASE("with no time origin chosen the streams share none", "[pair][time]") {
    Rig rig;
    rig.pair.set_time_origin(TimeOrigin::kNone);
    rig.open();
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.tx.calls.back() == "start_tx ");  // no burst was sent
    const std::array<std::int8_t, 8> samples{};
    CHECK(rig.pair.write_at(samples.data(), 4, true, 100'000, 1s).status == TimedStatus::kNoTime);
}

TEST_CASE("a timed write waits for the origin to be found", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    std::vector<std::int8_t> samples(64, 5);
    TimedWrite result;
    std::thread writer(
        [&] { result = rig.pair.write_at(samples.data(), 32, true, 1'000'000, 5s); });
    std::this_thread::sleep_for(20ms);
    rig.pair.activate(Direction::kTx);
    writer.join();
    CHECK(result.status == TimedStatus::kOk);
    CHECK(result.taken == 32);
    air.stop();
}

TEST_CASE("a timed write with no origin coming times out", "[pair][time]") {
    Rig rig;
    rig.open();
    rig.pair.activate(Direction::kTx);  // the receiver never starts
    const std::array<std::int8_t, 8> samples{};
    const auto start = std::chrono::steady_clock::now();
    const TimedWrite w = rig.pair.write_at(samples.data(), 4, true, 1000, 30ms);
    CHECK(w.status == TimedStatus::kTimeout);
    CHECK(w.taken == 0);
    CHECK(std::chrono::steady_clock::now() - start >= 30ms);
}

TEST_CASE("a timed write lands at its time in the receive stream", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    rig.pair.set_time_ticks(rig.pair.time_ticks() + 1'000'000);
    const std::int64_t when =
        rig.pair.time_ticks() + 20 * static_cast<std::int64_t>(kTransferSamples);

    // Listen for it: the first strong sample in the receive stream.
    std::vector<std::int8_t> pulse(2 * 64, 0);
    for (std::size_t k = 0; k < 64; ++k) {
        pulse[2 * k] = 100;
    }
    REQUIRE(rig.pair.write_at(pulse.data(), 64, true, when, 1s).status == TimedStatus::kOk);
    // Time `when` is receive position when - offset.
    const std::int64_t expected = when - rig.pair.time_offset();
    // Older samples hold the calibration burst.
    const auto from = static_cast<std::int64_t>(rig.pair.rx_received());
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::array<std::int8_t, 2 * kTransferSamples> buffer{};
    std::int64_t onset = -1;
    std::int64_t reached = 0;
    while (onset < 0 && reached < expected + 4096 && std::chrono::steady_clock::now() < deadline) {
        const RxRead r = rig.pair.read(buffer.data(), kTransferSamples, 100ms);
        for (std::size_t k = 0; k < r.samples && onset < 0; ++k) {
            if (static_cast<std::int64_t>(r.position + k) >= from && buffer[2 * k] > 15) {
                onset = static_cast<std::int64_t>(r.position + k);
            }
        }
        reached = static_cast<std::int64_t>(r.position + r.samples);
    }
    air.stop();
    CHECK(onset == expected);
    CHECK_FALSE(rig.pair.take_time_error());
}

TEST_CASE("a write for a time already past is late and reported once", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    const std::array<std::int8_t, 8> samples{};
    CHECK(rig.pair.write_at(samples.data(), 4, true, 0, 0us).status == TimedStatus::kLate);
    CHECK(rig.pair.take_time_error());
    CHECK_FALSE(rig.pair.take_time_error());
    air.stop();
}

TEST_CASE("a board's shortfall loses the origin, reported once", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    CHECK_FALSE(rig.pair.take_time_error());
    rig.rx.shortfall_count = 2;
    std::this_thread::sleep_for(120ms);  // the boards are read at most every 100 ms
    CHECK(rig.pair.take_time_error());
    CHECK_FALSE(rig.pair.take_time_error());
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("common time origin lost"));
    const std::array<std::int8_t, 8> samples{};
    CHECK(rig.pair.write_at(samples.data(), 4, true, rig.pair.time_ticks() + 100'000, 0us).status ==
          TimedStatus::kNoTime);

    // calibrate finds it again, counting shortfalls from there.
    rig.pair.calibrate();
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(*rig.pair.tx_of_rx() == -300);
    std::this_thread::sleep_for(120ms);
    CHECK_FALSE(rig.pair.take_time_error());
    air.stop();
}

TEST_CASE("calibrate runs the burst again on demand", "[pair][time]") {
    Rig rig;
    rig.open();
    CHECK_THROWS_AS(rig.pair.calibrate(), std::logic_error);
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    CHECK_THROWS_AS(rig.pair.calibrate(), std::logic_error);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    rig.pair.calibrate();
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(*rig.pair.tx_of_rx() == -300);
    air.coupled = false;
    rig.pair.calibrate();
    CHECK_FALSE(rig.pair.tx_of_rx());
    air.stop();
}

TEST_CASE("a sample rate change while streaming loses the origin", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    rig.pair.set_sample_rate(Direction::kRx, 8e6);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.pair.take_time_error());
    air.stop();
}

TEST_CASE("stopping either stream leaves the origin to be found again", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    air.stop();
    rig.pair.deactivate(Direction::kTx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK_FALSE(rig.pair.take_time_error());  // stopping is not an error
    // The receiver runs on from where it was; the transmitter starts at 0.
    const auto received = static_cast<std::int64_t>(rig.pair.rx_received());
    Loopback again(rig.tx, rig.rx, 500);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(*rig.pair.tx_of_rx() == -(received + 500));
    again.stop();
}

TEST_CASE("time ticks count received samples plus a settable offset", "[pair][time]") {
    Rig rig;
    rig.pair.open_stream(Direction::kRx, Format::kCs8, 4);
    rig.pair.activate(Direction::kRx);
    CHECK(rig.pair.time_ticks() == 0);
    const std::vector<std::uint8_t> transfer(kTransferSamples * 2, 0);
    rig.rx.deliver_rx(transfer);
    CHECK(rig.pair.time_ticks() == static_cast<std::int64_t>(kTransferSamples));
    rig.pair.set_time_ticks(5'000'000);
    CHECK(rig.pair.time_ticks() == 5'000'000);
    CHECK(rig.pair.time_offset() == 5'000'000 - static_cast<std::int64_t>(kTransferSamples));
    rig.rx.deliver_rx(transfer);
    CHECK(rig.pair.time_ticks() == 5'000'000 + static_cast<std::int64_t>(kTransferSamples));
    rig.pair.set_time_ticks(0);
    CHECK(rig.pair.time_ticks() == 0);
}

TEST_CASE("a trigger origin arms the first stream and starts both on the second", "[pair][time]") {
    Rig rig;
    rig.pair.set_time_origin(TimeOrigin::kTrigger);
    rig.open();
    rig.pair.activate(Direction::kRx);
    CHECK(rig.pair.active(Direction::kRx));
    CHECK(rig.rx.starts == 0);  // armed, not started
    CHECK_FALSE(rig.pair.tx_of_rx());

    // The transmitter moves only once the receiver's start sends the edge.
    std::atomic<bool> stop{false};
    std::thread edge([&] {
        while (!stop) {
            if (rig.rx.rx_streaming && rig.tx.tx_streaming) {
                rig.tx.pull_tx(kTransferSamples * 2);
            }
            std::this_thread::sleep_for(100us);
        }
    });
    rig.pair.activate(Direction::kTx);
    stop = true;
    edge.join();
    CHECK(rig.rx.rx_streaming);
    CHECK(rig.tx.tx_streaming);
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(*rig.pair.tx_of_rx() == 0);

    // The transmitter was put in sync mode before it started, then the receiver started.
    const auto& calls = rig.tx.calls;
    const auto sync = std::find(calls.begin(), calls.end(), "set_hardware_sync on");
    const auto start = std::find(calls.begin(), calls.end(), "start_tx ");
    REQUIRE(sync != calls.end());
    CHECK(sync < start);
    CHECK(rig.rx.starts == 1);

    // Sync mode goes when the transmitter stops, so a later start does not wait.
    rig.pair.deactivate(Direction::kTx);
    CHECK(rig.tx.calls.back() == "set_hardware_sync off");
    CHECK_FALSE(rig.tx.hardware_sync);
}

TEST_CASE("a trigger start with no edge stops both and names the jumper", "[pair][time]") {
    Rig rig;
    rig.pair.set_time_origin(TimeOrigin::kTrigger);
    rig.open();
    rig.pair.activate(Direction::kTx);
    try {
        rig.pair.activate(Direction::kRx);
        FAIL("no edge should have been an error");
    } catch (const BoardError& e) {
        CHECK(std::string(e.what()).find("P28 pin 15") != std::string::npos);
    }
    CHECK_FALSE(rig.pair.active(Direction::kRx));
    CHECK_FALSE(rig.pair.active(Direction::kTx));
    CHECK_FALSE(rig.rx.rx_streaming);
    CHECK_FALSE(rig.tx.tx_streaming);
    CHECK_FALSE(rig.tx.hardware_sync);
    CHECK_FALSE(rig.pair.tx_of_rx());
}
