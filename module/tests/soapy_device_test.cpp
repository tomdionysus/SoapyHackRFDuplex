// SPDX-License-Identifier: GPL-3.0-or-later
#include "soapy_device.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Logger.h>

#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "fake_board.hpp"
#include "loopback.hpp"

using namespace hackrfduplex;
using hackrfduplex::test::FakeBoard;

namespace {

std::vector<std::pair<SoapySDRLogLevel, std::string>>& soapy_logs() {
    static std::vector<std::pair<SoapySDRLogLevel, std::string>> logs;
    return logs;
}

void capture(const SoapySDRLogLevel level, const char* message) {
    soapy_logs().emplace_back(level, message);
}

// A SoapyDevice over two fake boards, with SoapySDR's log captured for
// the life of the rig.
struct SoapyRig {
    explicit SoapyRig(const SoapySDR::Kwargs& args = {}) {
        soapy_logs().clear();
        SoapySDR::registerLogHandler(&capture);
        auto rx_owner = std::make_unique<FakeBoard>("00000000000000000000000000000001");
        auto tx_owner = std::make_unique<FakeBoard>("00000000000000000000000000000002");
        rx = rx_owner.get();
        tx = tx_owner.get();
        // The pair's log goes to SoapySDR's, as the device opened for real does.
        const Log forward = [](LogLevel level, const std::string& message) {
            const SoapySDRLogLevel soapy = level == LogLevel::kError     ? SOAPY_SDR_ERROR
                                           : level == LogLevel::kWarning ? SOAPY_SDR_WARNING
                                           : level == LogLevel::kInfo    ? SOAPY_SDR_INFO
                                                                         : SOAPY_SDR_DEBUG;
            SoapySDR::log(soapy, message);
        };
        device = std::make_unique<SoapyDevice>(
            std::make_unique<Pair>(std::move(rx_owner), std::move(tx_owner), forward), args);
    }
    ~SoapyRig() {
        device.reset();
        SoapySDR::registerLogHandler(nullptr);
    }
    SoapyRig(const SoapyRig&) = delete;
    SoapyRig& operator=(const SoapyRig&) = delete;

    bool logged(SoapySDRLogLevel level, const std::string& text) const {
        for (const auto& [l, m] : soapy_logs()) {
            if (l == level && m.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    FakeBoard* rx = nullptr;
    FakeBoard* tx = nullptr;
    std::unique_ptr<SoapyDevice> device;
};

}  // namespace

TEST_CASE("an unknown setting written is logged and otherwise ignored", "[soapy]") {
    // Applications pass whole device argument strings around and write
    // vendor settings this device does not have.
    SoapyRig rig;
    CHECK_NOTHROW(rig.device->writeSetting("biastee", "true"));
    CHECK(rig.logged(SOAPY_SDR_WARNING, "biastee"));
    CHECK_FALSE(rig.tx->bias);
    CHECK(rig.device->readSetting("bias_tx") == "false");
}

TEST_CASE("an unknown setting reads as an empty string", "[soapy]") {
    SoapyRig rig;
    CHECK(rig.device->readSetting("no_such_setting").empty());
}

TEST_CASE("each direction has an lo_locked sensor", "[soapy]") {
    SoapyRig rig;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX}) {
        CHECK(rig.device->listSensors(direction, 0) == std::vector<std::string>{"lo_locked"});
        const SoapySDR::ArgInfo info = rig.device->getSensorInfo(direction, 0, "lo_locked");
        CHECK(info.key == "lo_locked");
        CHECK(info.type == SoapySDR::ArgInfo::BOOL);
        CHECK(rig.device->readSensor(direction, 0, "lo_locked") == "true");
        CHECK_THROWS_AS(rig.device->readSensor(direction, 0, "temperature"), std::invalid_argument);
        CHECK_THROWS_AS(rig.device->readSensor(direction, 1, "lo_locked"), std::invalid_argument);
    }
    CHECK(rig.device->listSensors().empty());
}

TEST_CASE("lo_locked reads false while a reference switch is handled", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    device.setFrequency(SOAPY_SDR_TX, 0, 868.3e6);
    rig.tx->reference_switch_pending = true;
    SoapySDR::Stream* tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    std::vector<std::string> during;
    rig.tx->on_call = [&](const std::string& call) {
        if (call.rfind("set_frequency", 0) == 0) {
            during.push_back(device.readSensor(SOAPY_SDR_TX, 0, "lo_locked"));
            during.push_back(device.readSensor(SOAPY_SDR_RX, 0, "lo_locked"));
        }
    };
    CHECK(device.activateStream(tx) == 0);
    rig.tx->on_call = nullptr;
    CHECK(during == std::vector<std::string>{"false", "true", "false", "true"});
    CHECK(device.readSensor(SOAPY_SDR_TX, 0, "lo_locked") == "true");
    device.closeStream(tx);
}

TEST_CASE("each direction advertises the sample rates measured to work", "[soapy]") {
    // Measured on two boards: receive is clean down to 200 ksps, the
    // transmitter fails between 1.05 and 1.1 Msps.
    SoapyRig rig;
    const SoapySDR::RangeList rx = rig.device->getSampleRateRange(SOAPY_SDR_RX, 0);
    const SoapySDR::RangeList tx = rig.device->getSampleRateRange(SOAPY_SDR_TX, 0);
    REQUIRE(rx.size() == 1);
    REQUIRE(tx.size() == 1);
    CHECK(rx[0].minimum() == 200e3);
    CHECK(rx[0].maximum() == 20e6);
    CHECK(tx[0].minimum() == 1.2e6);
    CHECK(tx[0].maximum() == 20e6);
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX}) {
        const SoapySDR::Range range = rig.device->getSampleRateRange(direction, 0)[0];
        const std::vector<double> rates = rig.device->listSampleRates(direction, 0);
        CHECK(std::find(rates.begin(), rates.end(), 1.92e6) != rates.end());
        for (const double rate : rates) {
            CHECK(rate >= range.minimum());
            CHECK(rate <= range.maximum());
        }
    }
}

namespace {

// Both streams set up at 8 Msps in CS8 over 1024-sample transfers, the
// air between the boards played by a loopback 300 samples long.
struct TimedRig : SoapyRig {
    explicit TimedRig(const SoapySDR::Kwargs& args = {}) : SoapyRig(args) {
        rx->transfer_bytes_value = 2048;
        tx->transfer_bytes_value = 2048;
        SoapySDR::Device& d = *device;
        d.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
        d.setSampleRate(SOAPY_SDR_TX, 0, 8e6);
        rx_stream = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
        tx_stream = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    }
    ~TimedRig() { air.stop(); }

    SoapySDR::Stream* rx_stream = nullptr;
    SoapySDR::Stream* tx_stream = nullptr;
    hackrfduplex::test::Loopback air{*tx, *rx, 300};
};

int write_timed(SoapySDR::Device& d, SoapySDR::Stream* stream, std::size_t samples, int flags,
                long long time_ns) {
    std::vector<std::int8_t> buffer(2 * samples, 3);
    const void* buffs[] = {buffer.data()};
    flags |= SOAPY_SDR_HAS_TIME;
    return d.writeStream(stream, buffs, samples, flags, time_ns, 100000);
}

}  // namespace

TEST_CASE("the time origin is a device argument and a setting", "[soapy][time]") {
    {
        SoapyRig rig;
        CHECK(rig.device->readSetting("time_origin") == "burst");
    }
    {
        SoapyRig rig({{"time_origin", "none"}});
        CHECK(rig.device->readSetting("time_origin") == "none");
        rig.device->writeSetting("time_origin", "trigger");
        CHECK(rig.device->readSetting("time_origin") == "trigger");
        CHECK_THROWS_AS(rig.device->writeSetting("time_origin", "gps"), std::invalid_argument);
        bool listed_origin = false;
        bool listed_calibrate = false;
        for (const SoapySDR::ArgInfo& info : rig.device->getSettingInfo()) {
            if (info.key == "time_origin") {
                listed_origin = true;
                CHECK(info.value == "burst");
                CHECK(info.options == std::vector<std::string>{"burst", "trigger", "none"});
            }
            listed_calibrate = listed_calibrate || info.key == "calibrate";
        }
        CHECK(listed_origin);
        CHECK(listed_calibrate);
    }
    CHECK_THROWS_AS(SoapyRig({{"time_origin", "gps"}}), std::invalid_argument);
}

TEST_CASE("hardware time is the receive count plus the time set", "[soapy][time]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.rx->transfer_bytes_value = 2048;
    d.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
    CHECK_FALSE(d.hasHardwareTime());
    CHECK_FALSE(d.hasHardwareTime("PPS"));
    CHECK(d.getHardwareTime() == 0);
    d.setHardwareTime(1'000'000'000);
    CHECK(d.getHardwareTime() == 1'000'000'000);

    SoapySDR::Stream* rx = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    REQUIRE(d.activateStream(rx) == 0);
    d.setHardwareTime(2'000'000'000);
    const std::vector<std::uint8_t> transfer(2048, 0);
    rig.rx->deliver_rx(transfer);
    // 1024 samples at 8 Msps.
    CHECK(d.getHardwareTime() == 2'000'128'000);

    // Receive timestamps carry the same offset.
    std::vector<std::int8_t> buffer(2048);
    void* buffs[] = {buffer.data()};
    int flags = 0;
    long long time_ns = 0;
    CHECK(d.readStream(rx, buffs, 1024, flags, time_ns, 100000) == 1024);
    CHECK((flags & SOAPY_SDR_HAS_TIME) != 0);
    CHECK(time_ns == 2'000'000'000);
    d.closeStream(rx);
}

TEST_CASE("hardware time is shared with the transmitter once calibrated", "[soapy][time]") {
    TimedRig rig;
    SoapySDR::Device& d = *rig.device;
    REQUIRE(d.activateStream(rig.rx_stream) == 0);
    CHECK_FALSE(d.hasHardwareTime());
    REQUIRE(d.activateStream(rig.tx_stream) == 0);
    CHECK(d.hasHardwareTime());

    // Writing calibrate runs the burst again; unheard, it leaves no time.
    rig.air.coupled = false;
    d.writeSetting("calibrate", "");
    CHECK_FALSE(d.hasHardwareTime());
    rig.air.coupled = true;
    d.writeSetting("calibrate", "1");
    CHECK(d.hasHardwareTime());
}

TEST_CASE("timed writes: placed, late, and without a time", "[soapy][time]") {
    TimedRig rig;
    SoapySDR::Device& d = *rig.device;
    REQUIRE(d.activateStream(rig.rx_stream) == 0);
    REQUIRE(d.activateStream(rig.tx_stream) == 0);
    REQUIRE(d.hasHardwareTime());
    const long long now = d.getHardwareTime();

    CHECK(write_timed(d, rig.tx_stream, 64, SOAPY_SDR_END_BURST, now + 10'000'000) == 64);
    // ONE_PACKET ends the burst as END_BURST does.
    CHECK(write_timed(d, rig.tx_stream, 64, SOAPY_SDR_ONE_PACKET, now + 20'000'000) == 64);

    std::size_t mask = 0;
    int flags = 0;
    long long time_ns = 0;
    CHECK(d.readStreamStatus(rig.tx_stream, mask, flags, time_ns, 0) == SOAPY_SDR_TIMEOUT);
    CHECK(write_timed(d, rig.tx_stream, 64, SOAPY_SDR_END_BURST, 0) == SOAPY_SDR_TIME_ERROR);
    CHECK(d.readStreamStatus(rig.tx_stream, mask, flags, time_ns, 0) == SOAPY_SDR_TIME_ERROR);
    CHECK(d.readStreamStatus(rig.tx_stream, mask, flags, time_ns, 0) == SOAPY_SDR_TIMEOUT);

    // With the origin gone, timed writes are refused.
    rig.air.coupled = false;
    d.writeSetting("calibrate", "");
    CHECK(write_timed(d, rig.tx_stream, 64, SOAPY_SDR_END_BURST, now + 30'000'000) ==
          SOAPY_SDR_TIME_ERROR);
}

TEST_CASE("an untimed write ignores the time it is given", "[soapy][time]") {
    TimedRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.air.stop();
    REQUIRE(d.activateStream(rig.tx_stream) == 0);
    std::vector<std::int8_t> buffer(128, 3);
    const void* buffs[] = {buffer.data()};
    int flags = SOAPY_SDR_END_BURST;
    CHECK(d.writeStream(rig.tx_stream, buffs, 64, flags, 123, 100000) == 64);
}

TEST_CASE("the known settings still work through the device", "[soapy]") {
    SoapyRig rig;
    rig.device->writeSetting("bias_tx", "true");
    CHECK(rig.tx->bias);
    CHECK(rig.device->readSetting("bias_tx") == "true");
    rig.device->writeSetting("clkout", "rx");
    CHECK(rig.rx->clock_out);
    CHECK(rig.device->readSetting("clkout") == "rx");
    CHECK_THROWS_AS(rig.device->writeSetting("clkout", "both"), std::invalid_argument);
}

namespace {

// An active receive stream at 8 Msps in CS8 over 1024-sample transfers.
struct RxRig : SoapyRig {
    RxRig() {
        rx->transfer_bytes_value = 2048;
        SoapySDR::Device& d = *device;
        d.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
        stream = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
        activated = d.activateStream(stream);
    }

    // The board delivers one transfer, if it still streams; then one read.
    int deliver_and_read(int& flags, long timeout_us = 100000) {
        if (rx->streaming()) {
            rx->deliver_rx(std::vector<std::uint8_t>(2048, 0));
        }
        std::vector<std::int8_t> buffer(2048);
        void* buffs[] = {buffer.data()};
        long long time_ns = 0;
        return device->readStream(stream, buffs, 1024, flags, time_ns, timeout_us);
    }

    SoapySDR::Stream* stream = nullptr;
    int activated = -1;
};

// Every status readStreamStatus gives on the transmit stream for `span`.
std::vector<int> statuses_for(SoapySDR::Device& d, SoapySDR::Stream* tx,
                              std::chrono::milliseconds span) {
    std::vector<int> found;
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {
        std::size_t mask = 0;
        int flags = 0;
        long long time_ns = 0;
        const int r = d.readStreamStatus(tx, mask, flags, time_ns, 10000);
        if (r != SOAPY_SDR_TIMEOUT) {
            found.push_back(r);
        }
    }
    return found;
}

std::chrono::milliseconds since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start);
}

}  // namespace

TEST_CASE("a receive shortfall is reported once as an overflow, with no origin", "[soapy][fault]") {
    RxRig rig;
    REQUIRE(rig.activated == 0);
    int flags = 0;
    CHECK(rig.deliver_and_read(flags) == 1024);
    rig.rx->shortfall_count = 1;
    // The boards are read at most every 100 ms; 300 ms of reads must see it once.
    int overflows = 0;
    int overflow_flags = 0;
    const auto start = std::chrono::steady_clock::now();
    while (since(start) < std::chrono::milliseconds(300)) {
        flags = 0;
        const int n = rig.deliver_and_read(flags);
        if (n == SOAPY_SDR_OVERFLOW) {
            ++overflows;
            overflow_flags = flags;
        } else {
            CHECK(n == 1024);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(overflows == 1);
    CHECK((overflow_flags & SOAPY_SDR_END_ABRUPT) != 0);
    CHECK(rig.logged(SOAPY_SDR_WARNING, "shifted by an unknown amount"));
}

TEST_CASE("a receive shortfall with an origin is an overflow and a lost origin", "[soapy][fault]") {
    TimedRig rig;
    SoapySDR::Device& d = *rig.device;
    REQUIRE(d.activateStream(rig.rx_stream) == 0);
    REQUIRE(d.activateStream(rig.tx_stream) == 0);
    REQUIRE(d.hasHardwareTime());
    rig.rx->shortfall_count = 3;
    // The loopback outruns the reader, so the ring overflows of its own
    // accord too; the shortfall shows as its own warning, once.
    int overflows = 0;
    std::vector<std::int8_t> buffer(2048);
    void* buffs[] = {buffer.data()};
    const auto start = std::chrono::steady_clock::now();
    while (since(start) < std::chrono::milliseconds(300)) {
        int flags = 0;
        long long time_ns = 0;
        if (d.readStream(rig.rx_stream, buffs, 1024, flags, time_ns, 100000) ==
            SOAPY_SDR_OVERFLOW) {
            ++overflows;
        }
    }
    CHECK(overflows >= 1);
    CHECK(std::count_if(soapy_logs().begin(), soapy_logs().end(), [](const auto& entry) {
              return entry.second.find("shifted by an unknown amount") != std::string::npos;
          }) == 1);
    CHECK_FALSE(d.hasHardwareTime());
    CHECK(statuses_for(d, rig.tx_stream, std::chrono::milliseconds(50)) ==
          std::vector<int>{SOAPY_SDR_TIME_ERROR});
}

TEST_CASE("a transmit shortfall is reported once as an underflow", "[soapy][fault]") {
    SECTION("with no origin") {
        SoapyRig rig;
        rig.tx->transfer_bytes_value = 2048;
        SoapySDR::Device& d = *rig.device;
        SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
        REQUIRE(d.activateStream(tx) == 0);
        rig.tx->shortfall_count = 1;
        CHECK(statuses_for(d, tx, std::chrono::milliseconds(300)) ==
              std::vector<int>{SOAPY_SDR_UNDERFLOW});
    }
    SECTION("with an origin, which it loses") {
        TimedRig rig;
        SoapySDR::Device& d = *rig.device;
        REQUIRE(d.activateStream(rig.rx_stream) == 0);
        REQUIRE(d.activateStream(rig.tx_stream) == 0);
        REQUIRE(d.hasHardwareTime());
        rig.tx->shortfall_count = 1;
        std::vector<int> found = statuses_for(d, rig.tx_stream, std::chrono::milliseconds(300));
        std::sort(found.begin(), found.end());
        std::vector<int> expected{SOAPY_SDR_UNDERFLOW, SOAPY_SDR_TIME_ERROR};
        std::sort(expected.begin(), expected.end());
        CHECK(found == expected);
        CHECK_FALSE(d.hasHardwareTime());
    }
}

TEST_CASE("a receiving board that stops ends its stream with an error", "[soapy][fault]") {
    RxRig rig;
    REQUIRE(rig.activated == 0);
    SoapySDR::Device& d = *rig.device;
    int flags = 0;
    CHECK(rig.deliver_and_read(flags) == 1024);

    SECTION("its transfer thread dies") {
        rig.rx->rx_streaming = false;
        const auto start = std::chrono::steady_clock::now();
        CHECK(rig.deliver_and_read(flags, 2000000) == SOAPY_SDR_STREAM_ERROR);
        CHECK(since(start) < std::chrono::milliseconds(500));
        CHECK(rig.deliver_and_read(flags, 0) == SOAPY_SDR_STREAM_ERROR);
        CHECK(rig.logged(SOAPY_SDR_ERROR, "RX board stopped streaming"));
        // Deactivating and activating again starts afresh.
        CHECK(d.deactivateStream(rig.stream) == 0);
        REQUIRE(d.activateStream(rig.stream) == 0);
        CHECK(rig.deliver_and_read(flags) == 1024);
    }
    SECTION("it is unplugged") {
        rig.rx->unplugged = true;
        const auto start = std::chrono::steady_clock::now();
        CHECK(rig.deliver_and_read(flags, 2000000) == SOAPY_SDR_STREAM_ERROR);
        CHECK(since(start) < std::chrono::milliseconds(500));
        CHECK(rig.logged(SOAPY_SDR_ERROR, "HACKRF_ERROR_LIBUSB"));
        // Stopping a board that has gone fails, but the stream is no longer active.
        CHECK(d.deactivateStream(rig.stream) == SOAPY_SDR_STREAM_ERROR);
        CHECK(rig.deliver_and_read(flags, 0) == SOAPY_SDR_STREAM_ERROR);
        CHECK_NOTHROW(d.closeStream(rig.stream));
    }
}

TEST_CASE("a transmitting board that stops ends its stream with an error", "[soapy][fault]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.tx->transfer_bytes_value = 2048;
    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(d.activateStream(tx) == 0);
    SECTION("its transfer thread dies") { rig.tx->tx_streaming = false; }
    SECTION("it is unplugged") { rig.tx->unplugged = true; }
    std::vector<std::int8_t> buffer(2048, 3);
    const void* buffs[] = {buffer.data()};
    const auto start = std::chrono::steady_clock::now();
    int result = 0;
    for (int i = 0; i < 64 && result >= 0; ++i) {
        int flags = 0;
        result = d.writeStream(tx, buffs, 1024, flags, 0, 2000000);
    }
    CHECK(result == SOAPY_SDR_STREAM_ERROR);
    CHECK(since(start) < std::chrono::milliseconds(500));
    std::size_t mask = 0;
    int flags = 0;
    long long time_ns = 0;
    CHECK(d.readStreamStatus(tx, mask, flags, time_ns, 0) == SOAPY_SDR_STREAM_ERROR);
    CHECK(rig.logged(SOAPY_SDR_ERROR, "TX board"));
}

TEST_CASE("stream flags the device cannot honour are refused", "[soapy][refuse]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.rx->transfer_bytes_value = 2048;
    rig.tx->transfer_bytes_value = 2048;
    SoapySDR::Stream* rx = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);

    SECTION("activateStream on the transmitter takes no flags and no count") {
        CHECK(d.activateStream(tx, SOAPY_SDR_HAS_TIME, 1'000'000) == SOAPY_SDR_NOT_SUPPORTED);
        CHECK(d.activateStream(tx, SOAPY_SDR_END_BURST, 0, 100) == SOAPY_SDR_NOT_SUPPORTED);
        CHECK(d.activateStream(tx, 0, 0, 100) == SOAPY_SDR_NOT_SUPPORTED);
        CHECK_FALSE(rig.tx->tx_streaming);
        CHECK(d.activateStream(tx) == 0);
    }
    SECTION("activateStream on the receiver refuses flags it does not know") {
        CHECK(d.activateStream(rx, SOAPY_SDR_WAIT_TRIGGER) == SOAPY_SDR_NOT_SUPPORTED);
        CHECK_FALSE(rig.rx->rx_streaming);
    }
    SECTION("deactivateStream takes no flags") {
        REQUIRE(d.activateStream(rx) == 0);
        CHECK(d.deactivateStream(rx, SOAPY_SDR_HAS_TIME, 1'000'000) == SOAPY_SDR_NOT_SUPPORTED);
        CHECK(rig.rx->rx_streaming);
        CHECK(d.deactivateStream(rx, 0, 1'000'000) == 0);
        CHECK_FALSE(rig.rx->rx_streaming);
    }
    SECTION("writeStream refuses flags it does not know") {
        REQUIRE(d.activateStream(tx) == 0);
        std::vector<std::int8_t> buffer(128, 3);
        const void* buffs[] = {buffer.data()};
        for (const int flag : {SOAPY_SDR_WAIT_TRIGGER, SOAPY_SDR_MORE_FRAGMENTS}) {
            int flags = flag;
            CHECK(d.writeStream(tx, buffs, 64, flags, 0, 100000) == SOAPY_SDR_NOT_SUPPORTED);
        }
    }
    SECTION("an unknown stream argument is logged") {
        d.closeStream(rx);
        rx = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, {}, {{"bufflen", "4096"}});
        CHECK(rig.logged(SOAPY_SDR_WARNING, "bufflen"));
    }
    d.closeStream(rx);
    d.closeStream(tx);
}

TEST_CASE("corrections and modes the device lacks are refused unless neutral", "[soapy][refuse]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    for (const int dir : {SOAPY_SDR_RX, SOAPY_SDR_TX}) {
        CAPTURE(dir);
        CHECK_FALSE(d.hasGainMode(dir, 0));
        CHECK_NOTHROW(d.setGainMode(dir, 0, false));
        CHECK_THROWS_AS(d.setGainMode(dir, 0, true), std::invalid_argument);
        CHECK_FALSE(d.getGainMode(dir, 0));

        CHECK_FALSE(d.hasDCOffsetMode(dir, 0));
        CHECK_NOTHROW(d.setDCOffsetMode(dir, 0, false));
        CHECK_THROWS_AS(d.setDCOffsetMode(dir, 0, true), std::invalid_argument);
        CHECK_FALSE(d.hasDCOffset(dir, 0));
        CHECK_THROWS_AS(d.setDCOffset(dir, 0, {0.1, 0.0}), std::invalid_argument);

        CHECK_FALSE(d.hasIQBalance(dir, 0));
        CHECK_THROWS_AS(d.setIQBalance(dir, 0, {1.0, 0.0}), std::invalid_argument);
        CHECK_FALSE(d.hasIQBalanceMode(dir, 0));
        CHECK_NOTHROW(d.setIQBalanceMode(dir, 0, false));
        CHECK_THROWS_AS(d.setIQBalanceMode(dir, 0, true), std::invalid_argument);

        CHECK_FALSE(d.hasFrequencyCorrection(dir, 0));
        CHECK_NOTHROW(d.setFrequencyCorrection(dir, 0, 0.0));
        CHECK_THROWS_AS(d.setFrequencyCorrection(dir, 0, 1.5), std::invalid_argument);
        CHECK(d.getFrequencyCorrection(dir, 0) == 0.0);
    }
}

TEST_CASE("tuning arguments are refused, not ignored", "[soapy][refuse]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK_THROWS_AS(d.setFrequency(SOAPY_SDR_RX, 0, 868.3e6, {{"OFFSET", "1e6"}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(d.setFrequency(SOAPY_SDR_TX, 0, "RF", 868.3e6, {{"OFFSET", "1e6"}}),
                    std::invalid_argument);
    CHECK(rig.rx->frequency_hz == 0);
    CHECK(rig.tx->frequency_hz == 0);
    d.setFrequency(SOAPY_SDR_RX, 0, 868.3e6);
    CHECK(rig.rx->frequency_hz == 868'300'000);
}

TEST_CASE("device-level controls the HackRF lacks are refused", "[soapy][refuse]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK_NOTHROW(d.setFrontendMapping(SOAPY_SDR_RX, ""));
    CHECK_THROWS_AS(d.setFrontendMapping(SOAPY_SDR_RX, "A:0"), std::invalid_argument);
    CHECK_THROWS_AS(d.setMasterClockRate(40e6), std::invalid_argument);
    CHECK_THROWS_AS(d.setReferenceClockRate(10e6), std::invalid_argument);
    CHECK_THROWS_AS(d.setCommandTime(1'000'000'000), std::invalid_argument);
    CHECK(d.listRegisterInterfaces().empty());
    CHECK_THROWS_AS(d.writeRegister(0x10, 1), std::invalid_argument);
    CHECK_THROWS_AS(d.writeRegister("RFFC", 0x10, 1), std::invalid_argument);
    CHECK_THROWS_AS(d.writeRegisters("RFFC", 0x10, {1, 2}), std::invalid_argument);
    CHECK(d.listGPIOBanks().empty());
    CHECK_THROWS_AS(d.writeGPIO("P28", 1), std::invalid_argument);
    CHECK_THROWS_AS(d.writeGPIO("P28", 1, 1), std::invalid_argument);
    CHECK_THROWS_AS(d.writeGPIODir("P28", 1), std::invalid_argument);
    CHECK_THROWS_AS(d.writeGPIODir("P28", 1, 1), std::invalid_argument);
    CHECK_THROWS_AS(d.writeI2C(0x60, "\x01"), std::invalid_argument);
    CHECK_THROWS_AS(d.transactSPI(0, 1, 8), std::invalid_argument);
    CHECK(d.listUARTs().empty());
    CHECK_THROWS_AS(d.writeUART("0", "x"), std::invalid_argument);
}

TEST_CASE("settings take only the values they name", "[soapy][refuse]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    for (const char* on : {"true", "1"}) {
        d.writeSetting("bias_tx", "false");
        d.writeSetting("bias_tx", on);
        CHECK(rig.tx->bias);
    }
    d.writeSetting("bias_tx", "0");
    CHECK_FALSE(rig.tx->bias);
    CHECK_THROWS_AS(d.writeSetting("bias_tx", "yes"), std::invalid_argument);
    // Per-channel settings: there are none, and one written is logged.
    CHECK(d.getSettingInfo(SOAPY_SDR_RX, 0).empty());
    CHECK_NOTHROW(d.writeSetting(SOAPY_SDR_RX, 0, "lna_boost", "1"));
    CHECK(rig.logged(SOAPY_SDR_WARNING, "lna_boost"));
    CHECK(d.readSetting(SOAPY_SDR_RX, 0, "lna_boost").empty());
}

TEST_CASE("every per-channel call checks its direction and channel", "[soapy][refuse]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    for (const auto& [direction, channel] :
         std::vector<std::pair<int, std::size_t>>{{SOAPY_SDR_RX, 1}, {SOAPY_SDR_TX, 1}, {7, 0}}) {
        CAPTURE(direction, channel);
        CHECK_THROWS_AS(d.getFullDuplex(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.getStreamFormats(direction, channel), std::invalid_argument);
        double full_scale = 0;
        CHECK_THROWS_AS(d.getNativeStreamFormat(direction, channel, full_scale),
                        std::invalid_argument);
        CHECK_THROWS_AS(d.getStreamArgsInfo(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.listAntennas(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.setAntenna(direction, channel, "TX/RX"), std::invalid_argument);
        CHECK_THROWS_AS(d.getAntenna(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.listGains(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.hasGainMode(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.getGainRange(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.getGainRange(direction, channel, "VGA"), std::invalid_argument);
        CHECK_THROWS_AS(d.listFrequencies(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.getFrequencyRange(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.listSampleRates(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.getSampleRateRange(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.listBandwidths(direction, channel), std::invalid_argument);
        CHECK_THROWS_AS(d.getBandwidthRange(direction, channel), std::invalid_argument);
    }
}

namespace {

// Index of the first call named `call` at or after `from`, or calls.size().
std::size_t find_call(const FakeBoard& board, const std::string& call, std::size_t from = 0) {
    for (std::size_t i = from; i < board.calls.size(); ++i) {
        if (board.calls[i] == call) {
            return i;
        }
    }
    return board.calls.size();
}

}  // namespace

TEST_CASE("the transmit underrun limit is set before every transmit start", "[soapy][safety]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.tx->transfer_bytes_value = 2048;
    // One transfer's worth by default.
    CHECK(d.readSetting("tx_underrun_limit") == "1024");
    bool listed = false;
    for (const SoapySDR::ArgInfo& info : d.getSettingInfo()) {
        if (info.key == "tx_underrun_limit") {
            listed = true;
            CHECK(info.type == SoapySDR::ArgInfo::INT);
            CHECK(info.units == "samples");
        }
    }
    CHECK(listed);

    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(d.activateStream(tx) == 0);
    const std::size_t limit = find_call(*rig.tx, "set_tx_underrun_limit 1024");
    CHECK(limit < find_call(*rig.tx, "start_tx "));
    // Stopped cleanly, the board is left with no limit, as it was found.
    REQUIRE(d.deactivateStream(tx) == 0);
    const std::size_t stop = find_call(*rig.tx, "stop ", limit);
    CHECK(find_call(*rig.tx, "set_tx_underrun_limit 0", stop) < rig.tx->calls.size());

    d.writeSetting("tx_underrun_limit", "0");
    CHECK(d.readSetting("tx_underrun_limit") == "0");
    const std::size_t from = rig.tx->calls.size();
    REQUIRE(d.activateStream(tx) == 0);
    CHECK(find_call(*rig.tx, "set_tx_underrun_limit 0", from) <
          find_call(*rig.tx, "start_tx ", from));
    d.writeSetting("tx_underrun_limit", "4096");
    CHECK(d.readSetting("tx_underrun_limit") == "4096");
    for (const char* bad : {"-1", "abc", "", "5000000000"}) {
        CAPTURE(bad);
        CHECK_THROWS_AS(d.writeSetting("tx_underrun_limit", bad), std::invalid_argument);
    }
    CHECK(d.readSetting("tx_underrun_limit") == "4096");
    d.closeStream(tx);
}

TEST_CASE("the transmit underrun limit is also a device argument", "[soapy][safety]") {
    SoapyRig rig({{"tx_underrun_limit", "500"}});
    CHECK(rig.device->readSetting("tx_underrun_limit") == "500");
    CHECK_THROWS_AS(SoapyRig({{"tx_underrun_limit", "lots"}}), std::invalid_argument);
}

TEST_CASE("a transmitter that stops itself ends its stream with an error", "[soapy][safety]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.tx->transfer_bytes_value = 2048;
    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(d.activateStream(tx) == 0);
    rig.tx->stopped_itself = true;
    std::vector<std::int8_t> buffer(2048, 3);
    const void* buffs[] = {buffer.data()};
    const auto start = std::chrono::steady_clock::now();
    int result = 0;
    for (int i = 0; i < 64 && result >= 0; ++i) {
        int flags = 0;
        result = d.writeStream(tx, buffs, 1024, flags, 0, 2000000);
    }
    CHECK(result == SOAPY_SDR_STREAM_ERROR);
    CHECK(since(start) < std::chrono::milliseconds(500));
    CHECK(rig.logged(SOAPY_SDR_ERROR, "underrun limit"));
}

TEST_CASE("firmware that takes no underrun limit is named in the log", "[soapy][safety]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    rig.tx->takes_underrun_limit = false;
    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(d.activateStream(tx) == 0);
    CHECK(rig.logged(SOAPY_SDR_WARNING, "takes no underrun limit"));
    d.closeStream(tx);
}
