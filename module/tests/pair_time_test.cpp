// SPDX-License-Identifier: GPL-3.0-or-later
//
// The common time origin and timed writes, against two fake boards with a
// thread playing the air between them.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "burst.hpp"
#include "fake_board.hpp"
#include "loopback.hpp"
#include "pair.hpp"

using namespace hackrfduplex;
using hackrfduplex::test::FakeBoard;
using hackrfduplex::test::Loopback;
using namespace std::chrono_literals;

namespace {

constexpr std::size_t kTransferSamples = 1024;

struct Rig {
    Rig() {
        rx.transfer_bytes_value = kTransferSamples * 2;
        tx.transfer_bytes_value = kTransferSamples * 2;
        // The bench: both boards on one reference.
        rx.external_clock = true;
        tx.external_clock = true;
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

    // Both streams open at one rate, CS8, four buffers unless the receiver
    // is given more.
    void open(double rate = 8e6, std::size_t rx_slots = 4) {
        pair.set_sample_rate(Direction::kRx, rate);
        pair.set_sample_rate(Direction::kTx, rate);
        pair.open_stream(Direction::kRx, Format::kCs8, rx_slots);
        pair.open_stream(Direction::kTx, Format::kCs8, 4);
    }
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
    const Rig rig;
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
    CHECK(rig.pair.tx_of_rx() == -300);
    CHECK(rig.logged("common time origin from the calibration burst"));
    // Tuned alike, so the transmitter was not retuned for the burst.
    CHECK(frequency_calls(rig.tx, tx_calls).empty());
    air.stop();
}

TEST_CASE("calibration stops listening once the burst is certain", "[pair][time]") {
    // The listen is sized for the worst case, the burst's lead and a full
    // transmit ring ahead of it; on the target that is about 35 transfers,
    // 2.4 s at 1.92 Msps, inside activateStream. Correlated as it fills, the
    // burst ends it.
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    // Transfers 10 ms apart, so the correlation keeps up with the capture
    // as it does on the boards, even in an unoptimised, instrumented build;
    // unpaced, or at 1 ms there, it fills first. 200 ms at the worst.
    air.pace_us = 10000;
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(rig.pair.tx_of_rx() == -300);
    std::string line;
    {
        const std::lock_guard<std::mutex> lock(rig.log_mutex);
        for (const std::string& m : rig.logs) {
            if (m.find("calibration: burst heard after") != std::string::npos) {
                line = m;
            }
        }
    }
    REQUIRE_FALSE(line.empty());
    CAPTURE(line);
    unsigned long heard = 0;
    unsigned long could = 0;
    REQUIRE(std::sscanf(line.c_str(), "calibration: burst heard after %lu of the %lu", &heard,
                        &could) == 2);
    CHECK(heard < could);
    air.stop();
}

TEST_CASE("the calibration burst is queued far enough ahead that a stall does not make it late",
          "[pair][time]") {
    // Queued one transfer ahead of the transmitter, the burst was late, and
    // the origin lost, whenever the activating thread stalled for longer
    // than a transfer (6.5 ms at 20 Msps) between choosing its time and
    // writing it. Eight transfers, twice what libhackrf has in flight, ride
    // out a stall eight times as long.
    constexpr std::size_t kLeadTransfers = 8;
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    air.record = true;
    rig.pair.activate(Direction::kRx);
    // Held, the transmitter pulls nothing before the burst is queued, so
    // where the burst goes out is where it was queued.
    air.paused = true;
    std::thread start([&] { rig.pair.activate(Direction::kTx); });
    std::this_thread::sleep_for(50ms);
    air.paused = false;
    start.join();
    air.stop();
    REQUIRE(rig.pair.tx_of_rx());
    const std::vector<std::int8_t>& sent = air.sent();
    const auto first = std::find_if(sent.begin(), sent.end(), [](std::int8_t v) { return v != 0; });
    REQUIRE(first != sent.end());
    const auto at = static_cast<std::size_t>(first - sent.begin()) / 2;
    CHECK(at >= kLeadTransfers * kTransferSamples);
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
    CHECK(rig.pair.tx_of_rx() == -1000);
    CHECK(frequency_calls(rig.tx, tx_calls) ==
          std::vector<std::string>{"set_frequency 892300000", "set_frequency 847300000"});
    CHECK(rig.tx.frequency_hz == 847'300'000);
    CHECK(rig.pair.frequency(Direction::kTx) == 847'300'000);
    air.stop();
}

TEST_CASE("a transmit tune during the burst is what the board keeps", "[pair][time]") {
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.pair.set_frequency(Direction::kTx, 847'300'000);
    rig.open();
    Loopback air(rig.tx, rig.rx, 1000);
    rig.pair.activate(Direction::kTx);
    // When the transmitter goes to the receive frequency for the burst, the
    // application tunes it, as soon as the pair lets it, while the burst is
    // still out: the air waits for that tune.
    std::thread tuner;
    rig.tx.on_call = [&](const std::string& call) {
        if (call == "set_frequency 892300000" && !tuner.joinable()) {
            air.paused = true;
            tuner = std::thread([&] {
                rig.pair.set_frequency(Direction::kTx, 900'000'000);
                air.paused = false;
            });
        }
    };
    rig.pair.activate(Direction::kRx);
    tuner.join();
    rig.tx.on_call = nullptr;
    CHECK(rig.pair.frequency(Direction::kTx) == 900'000'000);
    CHECK(rig.tx.frequency_hz == 900'000'000);
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

TEST_CASE("a shortfall while the burst is out leaves no origin", "[pair][time]") {
    // docs/design.md, Time: where the burst was heard says nothing once a
    // board has lost or delayed samples meanwhile.
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.pair.set_frequency(Direction::kTx, 847'300'000);
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kTx);
    // The transmitter goes to the receive frequency for the burst after the
    // pair has read the boards' counts; the receiver falls short then.
    rig.tx.on_call = [&](const std::string& call) {
        if (call == "set_frequency 892300000") {
            rig.rx.shortfall_count = 1;
        }
    };
    rig.pair.activate(Direction::kRx);
    rig.tx.on_call = nullptr;
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("while the burst was out"));
    air.stop();
}

TEST_CASE("a trigger origin cannot join a stream already running", "[pair][time]") {
    Rig rig;
    rig.pair.set_time_origin(TimeOrigin::kNone);
    rig.open();
    rig.pair.activate(Direction::kRx);
    rig.pair.set_time_origin(TimeOrigin::kTrigger);
    rig.pair.activate(Direction::kTx);
    CHECK(rig.tx.tx_streaming);
    CHECK_FALSE(rig.tx.hardware_sync);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("did not start together"));
}

TEST_CASE("a burst that cannot be written leaves no origin", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kTx);
    // The transmitter takes nothing, and the writer has filled its ring, so
    // the burst finds no room within its two seconds.
    air.paused = true;
    const std::vector<std::int8_t> samples(8 * kTransferSamples, 0);
    std::size_t written = 0;
    while (written < 4 * kTransferSamples) {
        const std::size_t n = rig.pair.write(samples.data() + 2 * written,
                                             4 * kTransferSamples - written, false, 100ms);
        REQUIRE(n > 0);
        written += n;
    }
    rig.pair.activate(Direction::kRx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("the calibration burst could not be written"));
    air.paused = false;
    air.stop();
}

TEST_CASE("a receiver that delivers nothing while the burst is out leaves no origin",
          "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kTx);
    // Nothing moves either way, so the listen runs to its deadline.
    air.paused = true;
    rig.pair.activate(Direction::kRx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("the receive stream delivered less than expected"));
    CHECK(rig.logged("calibration burst was not heard"));
    air.paused = false;
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
    // The air runs far faster than real boards, so a host busy elsewhere can
    // fall behind it: the receiver gets room to spare, and a pulse made late
    // by a stall, or lost to an overflow, which says nothing about where it
    // was sent, is sent again. The first that is neither must land exactly.
    Rig rig;
    rig.open(8e6, 32);
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    rig.pair.set_time_ticks(rig.pair.time_ticks() + 1'000'000);

    std::vector<std::int8_t> pulse(128, 0);
    for (std::size_t k = 0; k < 64; ++k) {
        pulse[2 * k] = 100;
    }
    std::array<std::int8_t, 2 * kTransferSamples> buffer{};
    bool clean = false;
    for (int attempt = 0; attempt < 5 && !clean; ++attempt) {
        const std::int64_t when =
            rig.pair.time_ticks() + 20 * static_cast<std::int64_t>(kTransferSamples);
        const TimedStatus status = rig.pair.write_at(pulse.data(), 64, true, when, 1s).status;
        if (status == TimedStatus::kLate) {
            UNSCOPED_INFO("attempt " << attempt << ": late");
            CHECK(rig.pair.take_time_error());
            continue;
        }
        REQUIRE(status == TimedStatus::kOk);
        // Time `when` is receive position when - offset.
        const std::int64_t expected = when - rig.pair.time_offset();
        // Older samples hold the calibration burst, or an earlier pulse.
        const auto from = static_cast<std::int64_t>(rig.pair.rx_received());
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        // Listen for it: the first strong sample in the receive stream.
        std::int64_t onset = -1;
        std::int64_t reached = 0;
        bool overflow = false;
        while (onset < 0 && reached < expected + 4096 &&
               std::chrono::steady_clock::now() < deadline) {
            const RxRead r = rig.pair.read(buffer.data(), kTransferSamples, 100ms);
            overflow = overflow || r.overflow;
            for (std::size_t k = 0; k < r.samples && onset < 0; ++k) {
                if (static_cast<std::int64_t>(r.position + k) >= from && buffer[2 * k] > 15) {
                    onset = static_cast<std::int64_t>(r.position + k);
                }
            }
            reached = static_cast<std::int64_t>(r.position + r.samples);
        }
        // Taken in time but sent late, the board having passed its time
        // before the block reached it.
        const bool late = rig.pair.take_time_error();
        UNSCOPED_INFO("attempt " << attempt << ": onset " << onset << ", expected " << expected
                                 << (overflow ? ", after an overflow" : "")
                                 << (late ? ", sent late" : ""));
        if (!overflow && !late) {
            clean = true;
            CHECK(onset == expected);
        }
    }
    air.stop();
    CHECK(clean);
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

TEST_CASE("a timed write behind what is already queued is late", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    const std::int64_t when =
        rig.pair.time_ticks() + 40 * static_cast<std::int64_t>(kTransferSamples);
    const std::array<std::int8_t, 8> samples{};
    CHECK(rig.pair.write_at(samples.data(), 4, true, when, 1s).status == TimedStatus::kOk);
    CHECK(rig.pair.write_at(samples.data(), 4, true, when, 1s).status == TimedStatus::kLate);
    CHECK(rig.pair.take_time_error());
    air.stop();
}

TEST_CASE("a timed write that finds the ring full times out", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    // The board stops taking transfers; each timed write takes a block of
    // the four, and the fifth finds none.
    air.paused = true;
    const std::int64_t when =
        rig.pair.time_ticks() + 400 * static_cast<std::int64_t>(kTransferSamples);
    const std::array<std::int8_t, 8> samples{};
    for (std::int64_t k = 0; k < 4; ++k) {
        CHECK(rig.pair.write_at(samples.data(), 4, true, when + k * 4096, 100ms).status ==
              TimedStatus::kOk);
    }
    const auto start = std::chrono::steady_clock::now();
    const TimedWrite w =
        rig.pair.write_at(samples.data(), 4, true, when + std::int64_t{5} * 4096, 50ms);
    CHECK(w.status == TimedStatus::kTimeout);
    CHECK(w.taken == 0);
    CHECK(std::chrono::steady_clock::now() - start >= 50ms);
    air.paused = false;
    air.stop();
}

TEST_CASE("a timed write to a transmitter that has stopped is refused", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    air.stop();
    rig.tx.tx_streaming = false;
    // Past one poll interval, so the write looks at the boards first.
    std::this_thread::sleep_for(150ms);
    const std::array<std::int8_t, 8> samples{};
    const TimedWrite w =
        rig.pair.write_at(samples.data(), 4, true, rig.pair.time_ticks() + 1'000'000, 100ms);
    CHECK(w.status == TimedStatus::kTimeout);
    CHECK(rig.pair.failed(Direction::kTx));
}

TEST_CASE("a transmitter that stopped streams again, with a new origin, once both streams restart",
          "[pair][time][fault]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    air.stop();
    rig.tx.tx_streaming = false;
    // Past one poll interval, so the write looks at the boards first.
    std::this_thread::sleep_for(150ms);
    const std::array<std::int8_t, 8> samples{};
    CHECK(rig.pair.write_at(samples.data(), 4, true, rig.pair.time_ticks() + 1'000'000, 100ms)
              .status == TimedStatus::kTimeout);
    CHECK(rig.pair.failed(Direction::kTx));
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("TX board stopped streaming"));
    // Calibrating alone cannot bring it back: the burst has no transmitter
    // to go out through (it is not written, or written and never heard),
    // and the log says why.
    rig.pair.calibrate();
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("no common time origin"));

    // What hackrfduplex-check --recover does: both down, then both up.
    rig.pair.deactivate(Direction::kRx);
    rig.pair.deactivate(Direction::kTx);
    Loopback again(rig.tx, rig.rx, 500);
    rig.pair.activate(Direction::kTx);
    rig.pair.activate(Direction::kRx);
    CHECK_FALSE(rig.pair.failed(Direction::kTx));
    CHECK_FALSE(rig.pair.failed(Direction::kRx));
    REQUIRE(rig.pair.tx_of_rx());
    again.stop();
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
    CHECK(rig.pair.tx_of_rx() == -300);
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
    CHECK(rig.pair.tx_of_rx() == -300);
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
    CHECK(rig.pair.tx_of_rx() == -(received + 500));
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
    CHECK(rig.pair.tx_of_rx() == 0);

    // The transmitter was put in sync mode before it started, then the receiver started.
    const auto& calls = rig.tx.calls;
    const auto sync = std::find(calls.begin(), calls.end(), "set_hardware_sync on");
    const auto start = std::find(calls.begin(), calls.end(), "start_tx ");
    REQUIRE(sync != calls.end());
    CHECK(sync < start);
    CHECK(rig.rx.starts == 1);

    // Sync mode goes when the transmitter stops, so a later start does not wait.
    const std::size_t before_stop = rig.tx.calls.size();
    rig.pair.deactivate(Direction::kTx);
    CHECK(std::vector<std::string>(rig.tx.calls.begin() + static_cast<long>(before_stop),
                                   rig.tx.calls.end()) ==
          std::vector<std::string>{"stop ", "set_hardware_sync off", "set_tx_underrun_limit 0"});
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

TEST_CASE("the transmitter sends what was written, the burst and zeros, nothing else",
          "[pair][time][safety]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    air.record = true;
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);  // the calibration burst
    REQUIRE(rig.pair.tx_of_rx());

    // Data with no zero in it, I and Q, so the zeros the board sends
    // between bursts can be told from it.
    std::vector<std::int8_t> expected = burst::samples();
    std::uint32_t seed = 7;
    const auto make = [&](std::size_t samples) {
        std::vector<std::int8_t> data(2 * samples);
        for (std::int8_t& v : data) {
            seed = seed * 1103515245U + 12345U;
            v = static_cast<std::int8_t>(1 + (seed >> 16) % 100);
        }
        return data;
    };
    const auto write = [&](const std::vector<std::int8_t>& data, bool end) {
        std::size_t done = 0;
        while (done < data.size() / 2) {
            done += rig.pair.write(data.data() + 2 * done, data.size() / 2 - done, end, 1s);
        }
        expected.insert(expected.end(), data.begin(), data.end());
    };

    write(make(3000), true);            // a burst
    std::this_thread::sleep_for(20ms);  // a gap
    write(make(500), false);            // a stream, then its end
    write(make(700), true);
    const std::vector<std::int8_t> timed = make(2000);  // a timed burst
    REQUIRE(
        rig.pair.write_at(timed.data(), 1024, false, rig.pair.time_ticks() + 20'000, 1s).status ==
        TimedStatus::kOk);
    REQUIRE(rig.pair.write(timed.data() + 2048, 976, true, 1s) == 976);
    expected.insert(expected.end(), timed.begin(), timed.end());
    const std::vector<std::int8_t> late = make(100);  // too late: never sent
    CHECK(rig.pair.write_at(late.data(), 100, true, 0, 1s).status == TimedStatus::kLate);
    std::this_thread::sleep_for(20ms);
    rig.pair.calibrate();  // the burst again
    REQUIRE(rig.pair.tx_of_rx());
    const std::vector<std::int8_t> again = burst::samples();
    expected.insert(expected.end(), again.begin(), again.end());
    const std::vector<std::int8_t> cut = make(6000);  // stopped part way through
    std::size_t queued = 0;
    while (queued < 6000) {
        queued += rig.pair.write(cut.data() + 2 * queued, 6000 - queued, false, 1s);
    }
    expected.insert(expected.end(), cut.begin(), cut.end());
    air.stop();
    rig.pair.deactivate(Direction::kTx);

    std::vector<std::int8_t> heard;
    for (const std::int8_t v : air.sent()) {
        if (v != 0) {
            heard.push_back(v);
        }
    }
    // Everything up to the cut went out whole and in order; of the cut, a
    // beginning; nothing more.
    const auto [h, e] = std::mismatch(heard.begin(), heard.end(), expected.begin(), expected.end());
    INFO("heard " << heard.size() << " bytes, expected " << expected.size()
                  << ", first difference at " << (h - heard.begin()));
    REQUIRE(heard.size() <= expected.size());
    CHECK(heard.size() >= expected.size() - cut.size());
    CHECK(h == heard.end());
}

TEST_CASE("the burst is not sent unless the boards share a reference", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    SECTION("each on its own crystal") {
        rig.rx.external_clock = false;
        rig.tx.external_clock = false;
    }
    SECTION("the TX board fed, but nothing drives its CLKIN's source") {
        rig.rx.external_clock = false;
    }
    rig.pair.activate(Direction::kRx);
    const std::size_t before = rig.tx.calls.size();
    rig.pair.activate(Direction::kTx);
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("do not share a reference"));
    CHECK(rig.logged("clkout"));
    // Nothing timed was sent: no retune, and the transmitter only started.
    CHECK(frequency_calls(rig.tx, before).empty());
    air.stop();
}

TEST_CASE("a board fed from the other's CLKOUT shares its reference", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    SECTION("RX drives the TX board") {
        rig.rx.external_clock = false;
        rig.pair.set_clock_out(ClockOut::kRx);
    }
    SECTION("TX drives the RX board") {
        rig.tx.external_clock = false;
        rig.pair.set_clock_out(ClockOut::kTx);
    }
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    CHECK(rig.pair.tx_of_rx() == -300);
    air.stop();
}

TEST_CASE("CLKOUT switched off under running streams leaves no reference to share",
          "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.rx.external_clock = false;
    rig.pair.set_clock_out(ClockOut::kRx);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    rig.pair.set_clock_out(ClockOut::kOff);
    rig.pair.calibrate();
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("do not share a reference"));
    air.stop();
}

TEST_CASE("a board error while calibrating leaves the streams running without an origin",
          "[pair][time]") {
    Rig rig;
    rig.pair.set_frequency(Direction::kRx, 892'300'000);
    rig.pair.set_frequency(Direction::kTx, 847'300'000);
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kTx);
    // Taking the transmitter to the receive frequency for the burst fails.
    rig.tx.fail_on = "set_frequency";
    CHECK_NOTHROW(rig.pair.activate(Direction::kRx));
    CHECK(rig.pair.active(Direction::kRx));
    CHECK(rig.pair.active(Direction::kTx));
    CHECK_FALSE(rig.pair.tx_of_rx());
    CHECK(rig.logged("no common time origin"));
    // Refused at once rather than left waiting for an origin to come.
    const std::array<std::int8_t, 8> samples{};
    const auto start = std::chrono::steady_clock::now();
    CHECK(rig.pair.write_at(samples.data(), 4, true, 1'000'000, 1s).status == TimedStatus::kNoTime);
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
    // The transmitter is back where the application put it.
    CHECK(rig.tx.frequency_hz == 847'300'000);
    air.stop();
}

TEST_CASE("stopping a stream ends the origin, whatever origin is chosen next", "[pair][time]") {
    Rig rig;
    rig.open();
    Loopback air(rig.tx, rig.rx, 300);
    rig.pair.activate(Direction::kRx);
    rig.pair.activate(Direction::kTx);
    REQUIRE(rig.pair.tx_of_rx());
    rig.pair.set_time_origin(TimeOrigin::kNone);
    air.stop();
    rig.pair.deactivate(Direction::kRx);
    CHECK_FALSE(rig.pair.tx_of_rx());
}
