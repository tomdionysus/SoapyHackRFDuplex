// SPDX-License-Identifier: GPL-3.0-or-later
#include "soapy_device.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Logger.h>

#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <complex>
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

#if defined(HACKRFDUPLEX_DSP)
constexpr bool kDspBuild = true;
#else
constexpr bool kDspBuild = false;
#endif

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
    // The device's own sensors are item 9's: ref_locked and time_locked.
    CHECK(rig.device->listSensors() == std::vector<std::string>{"ref_locked", "time_locked"});
}

TEST_CASE("the device sensors are booleans named in the listing", "[soapy][sensors]") {
    const SoapyRig rig;
    const SoapySDR::Device& d = *rig.device;
    for (const std::string key : {"ref_locked", "time_locked"}) {
        const SoapySDR::ArgInfo info = d.getSensorInfo(key);
        CHECK(info.key == key);
        CHECK(info.type == SoapySDR::ArgInfo::BOOL);
        CHECK_FALSE(info.description.empty());
        CHECK(d.readSensor(key) == "false");
    }
    CHECK_THROWS_AS(d.readSensor("lo_locked"), std::invalid_argument);
    CHECK_THROWS_AS(d.getSensorInfo("temperature"), std::invalid_argument);
}

TEST_CASE("ref_locked says whether the boards run from one reference", "[soapy][sensors]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    // Each on its own crystal.
    CHECK(d.readSensor("ref_locked") == "false");
    // The cable from the RX board's CLKOUT to the TX board's CLKIN, CLKOUT
    // off: the TX board finds no clock there.
    d.writeSetting("clkout", "rx");
    CHECK(d.readSensor("ref_locked") == "false");
    rig.tx->external_clock = true;
    CHECK(d.readSensor("ref_locked") == "true");
    // A clock on the TX board's CLKIN that the RX board does not drive.
    d.writeSetting("clkout", "off");
    CHECK(d.readSensor("ref_locked") == "false");
    // The other way about.
    rig.tx->external_clock = false;
    rig.rx->external_clock = true;
    d.writeSetting("clkout", "tx");
    CHECK(d.readSensor("ref_locked") == "true");
    // Both fed from one outside reference.
    d.writeSetting("clkout", "off");
    rig.tx->external_clock = true;
    CHECK(d.readSensor("ref_locked") == "true");
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
        // The bench: both boards on one reference.
        rx->external_clock = true;
        tx->external_clock = true;
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

TEST_CASE("ref_locked holds while the boards stream, without asking them", "[soapy][sensors]") {
    // On the target the clock chip, CLKIN status with it, reads as zero
    // while a board streams; the reading taken at the start stands.
    TimedRig rig;
    SoapySDR::Device& d = *rig.device;
    d.writeSetting("clkout", "rx");
    REQUIRE(d.readSensor("ref_locked") == "true");
    REQUIRE(d.activateStream(rig.rx_stream) == 0);
    REQUIRE(d.activateStream(rig.tx_stream) == 0);
    rig.rx->external_clock = false;
    rig.tx->external_clock = false;
    const std::size_t rx_calls = rig.rx->calls.size();
    const std::size_t tx_calls = rig.tx->calls.size();
    CHECK(d.readSensor("ref_locked") == "true");
    CHECK(rig.rx->calls.size() == rx_calls);
    CHECK(rig.tx->calls.size() == tx_calls);
    // CLKOUT switched off under the streams is seen at once.
    d.writeSetting("clkout", "off");
    CHECK(d.readSensor("ref_locked") == "false");
}

TEST_CASE("time_locked follows the common origin", "[soapy][sensors]") {
    TimedRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK(d.readSensor("time_locked") == "false");
    REQUIRE(d.activateStream(rig.rx_stream) == 0);
    CHECK(d.readSensor("time_locked") == "false");
    REQUIRE(d.activateStream(rig.tx_stream) == 0);
    CHECK(d.readSensor("time_locked") == "true");
    CHECK(d.readSensor("time_locked") == (d.hasHardwareTime() ? "true" : "false"));
    REQUIRE(d.deactivateStream(rig.tx_stream) == 0);
    CHECK(d.readSensor("time_locked") == "false");
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

TEST_CASE("a shortfall count that goes back is a board restarted under its stream",
          "[soapy][fault]") {
    // Seen on the target with the board deauthorised mid-stream: the count
    // read 6898, then 0, and was logged as 4294960398 more shortfalls.
    RxRig rig;
    REQUIRE(rig.activated == 0);
    SoapySDR::Device& d = *rig.device;
    rig.rx->shortfall_count = 6898;
    int flags = 0;
    const auto start = std::chrono::steady_clock::now();
    while (since(start) < std::chrono::milliseconds(250)) {
        flags = 0;
        if (rig.deliver_and_read(flags) < 0 && rig.rx->shortfall_count != 0) {
            rig.rx->shortfall_count = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(rig.rx->shortfall_count == 0);
    int result = 0;
    while (since(start) < std::chrono::milliseconds(600) && result != SOAPY_SDR_STREAM_ERROR) {
        flags = 0;
        result = rig.deliver_and_read(flags);
    }
    CHECK(result == SOAPY_SDR_STREAM_ERROR);
    CHECK(rig.logged(SOAPY_SDR_ERROR, "count went back"));
    CHECK(d.deactivateStream(rig.stream) == 0);
    for (const auto& [level, message] : soapy_logs()) {
        CHECK(message.find("429496") == std::string::npos);
    }
}

TEST_CASE("steady shortfalls are logged at most once a second, every one counted",
          "[soapy][fault]") {
    // On the target at 20 Msps the boards fall short at every poll; one
    // warning per poll and direction buried everything else in the log.
    SoapyRig rig;
    rig.tx->transfer_bytes_value = 2048;
    SoapySDR::Device& d = *rig.device;
    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(d.activateStream(tx) == 0);
    const auto warnings = [] {
        std::vector<std::string> found;
        for (const auto& [level, message] : soapy_logs()) {
            if (level == SOAPY_SDR_WARNING &&
                message.find("TX board ran out") != std::string::npos) {
                found.push_back(message);
            }
        }
        return found;
    };
    const auto start = std::chrono::steady_clock::now();
    int underflows = 0;
    while (since(start) < std::chrono::milliseconds(1500)) {
        ++rig.tx->shortfall_count;
        for (const int status : statuses_for(d, tx, std::chrono::milliseconds(20))) {
            underflows += status == SOAPY_SDR_UNDERFLOW ? 1 : 0;
        }
    }
    // Each poll still reports to the stream.
    CHECK(underflows >= 8);
    // The first at once, the rest a second later, together.
    const std::vector<std::string> logged = warnings();
    REQUIRE(logged.size() == 2);
    CHECK(logged[0].find("(1 shortfall)") != std::string::npos);
    CHECK(logged[1].find("in the last") != std::string::npos);
    // What came after that is logged when the stream stops.
    REQUIRE(d.deactivateStream(tx) == 0);
    CHECK(warnings().size() == 3);
    d.closeStream(tx);
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

        // A build with the receive corrections has them on the receive
        // channel ("receive corrections in a DSP build", below).
        if (!kDspBuild || dir == SOAPY_SDR_TX) {
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
        }

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

TEST_CASE("every device setting but calibrate is also a device argument", "[soapy]") {
    // SoapySDRUtil --probe="driver=hackrfduplex,clkout=rx" must leave the
    // reference out, not drop the key it was given.
    SoapyRig rig({{"clkout", "rx"}, {"bias_tx", "true"}});
    CHECK(rig.device->readSetting("clkout") == "rx");
    CHECK(rig.rx->clock_out);
    CHECK(rig.device->readSetting("bias_tx") == "true");
    CHECK_THROWS_AS(SoapyRig({{"clkout", "both"}}), std::invalid_argument);
    CHECK_THROWS_AS(SoapyRig({{"bias_tx", "on"}}), std::invalid_argument);
    {
        const SoapyRig quiet({{"calibrate", "1"}});
        CHECK(quiet.logged(SOAPY_SDR_WARNING, "calibrate"));
    }
}

TEST_CASE("an unknown device argument is logged and otherwise ignored", "[soapy]") {
    const SoapyRig rig({{"driver", "hackrfduplex"},
                        {"label", "HackRF Duplex 1 + 2"},
                        {"rx_serial", "1"},
                        {"tx_serial", "2"},
                        {"biastee", "1"}});
    CHECK(rig.logged(SOAPY_SDR_WARNING, "biastee"));
    for (const char* known : {"driver", "label", "rx_serial", "tx_serial"}) {
        CAPTURE(known);
        CHECK_FALSE(rig.logged(SOAPY_SDR_WARNING, std::string("argument ") + known));
    }
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

namespace {

// A receive stream at 8 Msps in CS8 over 1024-sample transfers, set up but
// not active, whose board sends each sample's position in it: I the low
// byte, Q the next, so a read shows which samples it holds.
struct WindowRig : SoapyRig {
    WindowRig() {
        rx->transfer_bytes_value = 2048;
        SoapySDR::Device& d = *device;
        d.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
        stream = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    }

    void deliver(int transfers) {
        for (int t = 0; t < transfers; ++t) {
            std::vector<std::uint8_t> bytes(2048);
            for (std::size_t i = 0; i < 1024; ++i) {
                bytes[2 * i] = static_cast<std::uint8_t>(sent & 0xff);
                bytes[2 * i + 1] = static_cast<std::uint8_t>((sent >> 8) & 0xff);
                ++sent;
            }
            rx->deliver_rx(bytes);
        }
    }

    struct Read {
        int result = 0;
        int flags = 0;
        long long time_ns = 0;
        // The position the first sample carries, from its bytes.
        std::uint64_t first = 0;
    };
    Read read(std::size_t max = 1024, long timeout_us = 0) {
        std::vector<std::uint8_t> buffer(2 * max);
        void* buffs[] = {buffer.data()};
        Read r;
        SoapySDR::Device& d = *device;
        r.result = d.readStream(stream, buffs, max, r.flags, r.time_ns, timeout_us);
        if (r.result > 0) {
            r.first = buffer[0] | (static_cast<std::uint64_t>(buffer[1]) << 8);
        }
        return r;
    }

    // Samples delivered until a read flags the end of the burst or none come.
    std::size_t read_burst(bool& ended) {
        std::size_t total = 0;
        ended = false;
        while (true) {
            const Read r = read();
            if (r.result <= 0) {
                return total;
            }
            total += static_cast<std::size_t>(r.result);
            if ((r.flags & SOAPY_SDR_END_BURST) != 0) {
                ended = true;
                return total;
            }
        }
    }

    static long long ns(long long ticks) { return SoapySDR::ticksToTimeNs(ticks, 8e6); }

    SoapySDR::Stream* stream = nullptr;
    // Samples the board has sent; positions run modulo 65536 in the bytes.
    std::uint64_t sent = 0;
};

}  // namespace

TEST_CASE("a timed receive start delivers from that time, stamped exactly", "[soapy][timed-rx]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    d.setHardwareTime(WindowRig::ns(10'000));
    // Receive time 11'500 is stream position 1500.
    REQUIRE(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME, WindowRig::ns(11'500)) == 0);
    rig.deliver(4);
    const WindowRig::Read first = rig.read();
    REQUIRE(first.result > 0);
    CHECK(first.time_ns == WindowRig::ns(11'500));
    CHECK(first.first == 1500);
    // Then on without a break.
    const WindowRig::Read next = rig.read();
    CHECK(next.time_ns == first.time_ns + WindowRig::ns(first.result));
    CHECK(next.first == 1500 + static_cast<std::uint64_t>(first.result));
}

TEST_CASE("a timed receive start waits for its time to come", "[soapy][timed-rx]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    REQUIRE(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME, WindowRig::ns(5000)) == 0);
    rig.deliver(4);  // positions 0 to 4095: all before the time
    CHECK(rig.read().result == SOAPY_SDR_TIMEOUT);
    rig.deliver(2);
    const WindowRig::Read r = rig.read();
    CHECK(r.result > 0);
    CHECK(r.first == 5000);
}

TEST_CASE("a finite receive burst ends with END_BURST and then delivers nothing",
          "[soapy][timed-rx]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    SECTION("at a time") {
        REQUIRE(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST,
                                 WindowRig::ns(1500), 2500) == 0);
        rig.deliver(6);
        const WindowRig::Read first = rig.read();
        CHECK(first.first == 1500);
        CHECK(first.time_ns == WindowRig::ns(1500));
        bool ended = false;
        CHECK(static_cast<std::size_t>(first.result) + rig.read_burst(ended) == 2500);
        CHECK(ended);
    }
    SECTION("at once") {
        REQUIRE(d.activateStream(rig.stream, SOAPY_SDR_END_BURST, 0, 100) == 0);
        rig.deliver(2);
        bool ended = false;
        CHECK(rig.read_burst(ended) == 100);
        CHECK(ended);
    }
    // After the burst, more from the board is not delivered.
    rig.deliver(4);
    CHECK(rig.read(1024, 20000).result == SOAPY_SDR_TIMEOUT);
}

TEST_CASE("a timed window can be set on a running receive stream", "[soapy][timed-rx]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    REQUIRE(d.activateStream(rig.stream) == 0);
    rig.deliver(1);
    CHECK(rig.read().result == 1024);
    // The streams keep running, so the origin holds; the window starts later.
    REQUIRE(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST,
                             WindowRig::ns(3000), 512) == 0);
    rig.deliver(4);
    const WindowRig::Read r = rig.read();
    CHECK(r.first == 3000);
    CHECK(r.time_ns == WindowRig::ns(3000));
    // A read ends with its transfer, so the burst may take several.
    REQUIRE(r.result > 0);
    bool ended = false;
    CHECK(static_cast<std::size_t>(r.result) + rig.read_burst(ended) == 512);
    CHECK(ended);
    // A plain activation goes back to streaming without end.
    REQUIRE(d.activateStream(rig.stream) == 0);
    rig.deliver(1);
    const WindowRig::Read after = rig.read();
    CHECK(after.result > 0);
    CHECK((after.flags & SOAPY_SDR_END_BURST) == 0);
}

TEST_CASE("a receive time already passed is a time error", "[soapy][timed-rx]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    SECTION("before the stream starts") {
        d.setHardwareTime(WindowRig::ns(10'000));
        CHECK(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME, WindowRig::ns(9'000)) ==
              SOAPY_SDR_TIME_ERROR);
        CHECK_FALSE(rig.rx->rx_streaming);
    }
    SECTION("while it runs") {
        REQUIRE(d.activateStream(rig.stream) == 0);
        rig.deliver(2);  // the time is now 2048
        CHECK(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME, WindowRig::ns(1000)) ==
              SOAPY_SDR_TIME_ERROR);
        // The stream runs on as it was.
        CHECK(rig.read().first == 0);
    }
}

TEST_CASE("receive burst flags must come with their count", "[soapy][timed-rx]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK(d.activateStream(rig.stream, SOAPY_SDR_END_BURST, 0, 0) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(d.activateStream(rig.stream, 0, 0, 100) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME, 0, 100) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK_FALSE(rig.rx->rx_streaming);
}

TEST_CASE("clock sources are the clkout setting by another name", "[soapy][sources]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK(d.listClockSources() == std::vector<std::string>{"internal", "rx", "tx"});
    CHECK(d.getClockSource() == "internal");
    d.setClockSource("rx");
    CHECK(rig.rx->clock_out);
    CHECK_FALSE(rig.tx->clock_out);
    CHECK(d.getClockSource() == "rx");
    CHECK(d.readSetting("clkout") == "rx");
    d.setClockSource("tx");
    CHECK_FALSE(rig.rx->clock_out);
    CHECK(rig.tx->clock_out);
    CHECK(d.getClockSource() == "tx");
    d.setClockSource("internal");
    CHECK_FALSE(rig.rx->clock_out);
    CHECK_FALSE(rig.tx->clock_out);
    CHECK(d.readSetting("clkout") == "off");
    CHECK_THROWS_AS(d.setClockSource("external"), std::invalid_argument);
    CHECK_THROWS_AS(d.setClockSource("off"), std::invalid_argument);
}

TEST_CASE("time sources are the time_origin setting by another name", "[soapy][sources]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK(d.listTimeSources() == std::vector<std::string>{"burst", "trigger", "none"});
    CHECK(d.getTimeSource() == "burst");
    d.setTimeSource("trigger");
    CHECK(d.getTimeSource() == "trigger");
    CHECK(d.readSetting("time_origin") == "trigger");
    d.writeSetting("time_origin", "none");
    CHECK(d.getTimeSource() == "none");
    CHECK_THROWS_AS(d.setTimeSource("pps"), std::invalid_argument);
    CHECK(d.getTimeSource() == "none");
}

namespace {

// One direct-access read on a WindowRig: the result, its handle and flags,
// and the position its first sample carries.
struct Acquired {
    int result = 0;
    std::size_t handle = 0;
    int flags = 0;
    long long time_ns = 0;
    const std::uint8_t* data = nullptr;
    std::uint64_t first = 0;
};

Acquired acquire(SoapySDR::Device& d, SoapySDR::Stream* stream, long timeout_us = 0) {
    Acquired a;
    const void* buffs[1] = {nullptr};
    a.result = d.acquireReadBuffer(stream, a.handle, buffs, a.flags, a.time_ns, timeout_us);
    if (a.result > 0) {
        a.data = static_cast<const std::uint8_t*>(buffs[0]);
        a.first = a.data[0] | (static_cast<std::uint64_t>(a.data[1]) << 8);
    }
    return a;
}

}  // namespace

TEST_CASE("a CS8 receive stream offers its ring for direct access", "[soapy][direct]") {
    SoapyRig rig;
    rig.rx->transfer_bytes_value = 2048;
    rig.tx->transfer_bytes_value = 2048;
    SoapySDR::Device& d = *rig.device;
    SoapySDR::Stream* rx = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, {}, {{"buffers", "6"}});
    CHECK(d.getNumDirectAccessBuffers(rx) == 6);
    for (std::size_t handle = 0; handle < 6; ++handle) {
        void* buffs[1] = {nullptr};
        CHECK(d.getDirectAccessBufferAddrs(rx, handle, buffs) == 0);
        CHECK(buffs[0] != nullptr);
    }
    void* beyond[1] = {nullptr};
    CHECK(d.getDirectAccessBufferAddrs(rx, 6, beyond) == SOAPY_SDR_NOT_SUPPORTED);
    d.closeStream(rx);

    // Any other format converts, so there is nothing to hand over; nor on
    // the transmit side.
    rx = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    CHECK(d.getNumDirectAccessBuffers(rx) == 0);
    REQUIRE(d.activateStream(rx) == 0);
    CHECK(acquire(d, rx).result == SOAPY_SDR_NOT_SUPPORTED);
    d.closeStream(rx);
    SoapySDR::Stream* tx = d.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    CHECK(d.getNumDirectAccessBuffers(tx) == 0);
    std::size_t handle = 0;
    void* buffs[1] = {nullptr};
    CHECK(d.acquireWriteBuffer(tx, handle, buffs, 0) == SOAPY_SDR_NOT_SUPPORTED);
    d.closeStream(tx);
}

TEST_CASE("direct access hands over the transfers in place, stamped as reads are",
          "[soapy][direct]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    REQUIRE(d.activateStream(rig.stream) == 0);
    CHECK(acquire(d, rig.stream).result == SOAPY_SDR_TIMEOUT);
    rig.deliver(3);
    for (std::uint64_t transfer = 0; transfer < 3; ++transfer) {
        const Acquired a = acquire(d, rig.stream);
        REQUIRE(a.result == 1024);
        CHECK(a.first == transfer * 1024);
        CHECK((a.flags & SOAPY_SDR_HAS_TIME) != 0);
        CHECK(a.time_ns == WindowRig::ns(static_cast<long long>(transfer * 1024)));
        // In the ring itself, not a copy.
        void* buffs[1] = {nullptr};
        REQUIRE(d.getDirectAccessBufferAddrs(rig.stream, a.handle, buffs) == 0);
        CHECK(static_cast<const void*>(a.data) == buffs[0]);
        // One at a time: a second before the release is refused, as is a
        // read meanwhile.
        CHECK(acquire(d, rig.stream).result == SOAPY_SDR_NOT_SUPPORTED);
        CHECK(rig.read().result == SOAPY_SDR_NOT_SUPPORTED);
        d.releaseReadBuffer(rig.stream, a.handle);
    }
    CHECK_THROWS_AS(d.releaseReadBuffer(rig.stream, 0), std::invalid_argument);
    // Reads and direct access mix, each taking up where the other left off.
    rig.deliver(2);
    const WindowRig::Read part = rig.read(100);
    CHECK(part.first == 3072);
    const Acquired rest = acquire(d, rig.stream);
    CHECK(rest.result == 924);
    CHECK(rest.first == 3172);
    d.releaseReadBuffer(rig.stream, rest.handle);
    CHECK(acquire(d, rig.stream).first == 4096);
}

TEST_CASE("direct access reports gaps and keeps to a timed window", "[soapy][direct]") {
    WindowRig rig;
    SoapySDR::Device& d = *rig.device;
    // Stream position 1500 on, 1000 samples.
    REQUIRE(d.activateStream(rig.stream, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST,
                             WindowRig::ns(1500), 1000) == 0);
    rig.deliver(4);
    const Acquired first = acquire(d, rig.stream);
    REQUIRE(first.result == 548);
    CHECK(first.first == 1500);
    CHECK(first.time_ns == WindowRig::ns(1500));
    CHECK((first.flags & SOAPY_SDR_END_BURST) == 0);
    d.releaseReadBuffer(rig.stream, first.handle);
    const Acquired last = acquire(d, rig.stream);
    REQUIRE(last.result == 452);
    CHECK(last.first == 2048);
    CHECK((last.flags & SOAPY_SDR_END_BURST) != 0);
    d.releaseReadBuffer(rig.stream, last.handle);
    CHECK(acquire(d, rig.stream).result == SOAPY_SDR_TIMEOUT);
}

TEST_CASE("direct access reports an overflow and a failed board as reads do", "[soapy][direct]") {
    RxRig rig;
    REQUIRE(rig.activated == 0);
    SoapySDR::Device& d = *rig.device;
    // The ring holds 15; twenty drop five whole transfers, which show as a
    // gap once the next one comes.
    for (int i = 0; i < 20; ++i) {
        rig.rx->deliver_rx(std::vector<std::uint8_t>(2048, 0));
    }
    int overflows = 0;
    for (int i = 0; i < 18; ++i) {
        if (i == 15) {
            rig.rx->deliver_rx(std::vector<std::uint8_t>(2048, 0));
        }
        const Acquired a = acquire(d, rig.stream);
        if (a.result == SOAPY_SDR_OVERFLOW) {
            ++overflows;
            CHECK((a.flags & SOAPY_SDR_END_ABRUPT) != 0);
        } else if (a.result > 0) {
            d.releaseReadBuffer(rig.stream, a.handle);
        }
    }
    CHECK(overflows == 1);
    rig.rx->stopped_itself = true;
    int result = 0;
    const auto start = std::chrono::steady_clock::now();
    while (result != SOAPY_SDR_STREAM_ERROR && since(start) < std::chrono::milliseconds(500)) {
        result = acquire(d, rig.stream, 50000).result;
    }
    CHECK(result == SOAPY_SDR_STREAM_ERROR);
}

TEST_CASE("the device names itself and both its boards", "[soapy]") {
    SoapyRig rig;
    rig.rx->external_clock = true;
    rig.tx->clock_out = true;
    CHECK(rig.device->getDriverKey() == "hackrfduplex");
    CHECK(rig.device->getHardwareKey() == "HackRF Duplex");
    const SoapySDR::Kwargs info = rig.device->getHardwareInfo();
    CHECK(info.at("rx_serial") == "00000000000000000000000000000001");
    CHECK(info.at("tx_serial") == "00000000000000000000000000000002");
    CHECK(info.at("rx_board") == "HackRF One");
    CHECK(info.at("rx_revision") == "r9");
    CHECK(info.at("tx_firmware") == "2026.01.3");
    CHECK(info.at("tx_usb_api") == "1.10");
    CHECK(info.at("rx_clock") == "external");
    CHECK(info.at("tx_clock") == "internal");
    CHECK(info.at("rx_clkout") == "off");
    CHECK(info.at("tx_clkout") == "on");
    CHECK_FALSE(info.at("module_version").empty());
    CHECK_FALSE(info.at("libhackrf").empty());
}

TEST_CASE("each direction has one channel, and other directions none", "[soapy]") {
    SoapyRig rig;
    CHECK(rig.device->getNumChannels(SOAPY_SDR_RX) == 1);
    CHECK(rig.device->getNumChannels(SOAPY_SDR_TX) == 1);
    CHECK_THROWS_AS(rig.device->getNumChannels(7), std::invalid_argument);
    CHECK_THROWS_AS(rig.device->getGain(SOAPY_SDR_RX, 1), std::invalid_argument);
}

TEST_CASE("receive gains by stage reach the receiving board and read back", "[soapy]") {
    SoapyRig rig;
    rig.device->setGain(SOAPY_SDR_RX, 0, "LNA", 24);
    rig.device->setGain(SOAPY_SDR_RX, 0, "VGA", 30);
    rig.device->setGain(SOAPY_SDR_RX, 0, "AMP", 14);
    CHECK(rig.rx->lna_db == 24);
    CHECK(rig.rx->vga_db == 30);
    CHECK(rig.rx->amp);
    CHECK_FALSE(rig.tx->amp);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0, "LNA") == 24);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0, "VGA") == 30);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0, "AMP") == 14);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0) == 24 + 30 + 14);
    // Between steps, a stage rounds down to its step (gain.hpp).
    rig.device->setGain(SOAPY_SDR_RX, 0, "LNA", 31);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0, "LNA") == 24);
    rig.device->setGain(SOAPY_SDR_RX, 0, "AMP", 0);
    CHECK_FALSE(rig.rx->amp);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0, "AMP") == 0);
    CHECK_THROWS_AS(rig.device->setGain(SOAPY_SDR_RX, 0, "IF", 10), std::invalid_argument);
    CHECK_THROWS_AS(rig.device->getGain(SOAPY_SDR_RX, 0, "IF"), std::invalid_argument);
}

TEST_CASE("transmit gains by stage reach the transmitting board and read back", "[soapy]") {
    SoapyRig rig;
    rig.device->setGain(SOAPY_SDR_TX, 0, "VGA", 20);
    rig.device->setGain(SOAPY_SDR_TX, 0, "AMP", 14);
    CHECK(rig.tx->tx_vga_db == 20);
    CHECK(rig.tx->amp);
    CHECK_FALSE(rig.rx->amp);
    CHECK(rig.device->getGain(SOAPY_SDR_TX, 0, "VGA") == 20);
    CHECK(rig.device->getGain(SOAPY_SDR_TX, 0, "AMP") == 14);
    CHECK(rig.device->getGain(SOAPY_SDR_TX, 0) == 34);
    // The receive stages are not the transmitter's.
    CHECK_THROWS_AS(rig.device->setGain(SOAPY_SDR_TX, 0, "LNA", 8), std::invalid_argument);
    CHECK_THROWS_AS(rig.device->getGain(SOAPY_SDR_TX, 0, "LNA"), std::invalid_argument);
}

TEST_CASE("an overall gain is spread over the stages and reads back whole", "[soapy]") {
    SoapyRig rig;
    rig.device->setGain(SOAPY_SDR_RX, 0, 40.0);
    CHECK(rig.device->getGain(SOAPY_SDR_RX, 0) == 40);
    CHECK(rig.rx->lna_db + rig.rx->vga_db + (rig.rx->amp ? 14U : 0U) == 40);
    rig.device->setGain(SOAPY_SDR_TX, 0, 30.0);
    CHECK(rig.device->getGain(SOAPY_SDR_TX, 0) == 30);
    CHECK(rig.tx->tx_vga_db + (rig.tx->amp ? 14U : 0U) == 30);
}

TEST_CASE("each gain stage gives its own range", "[soapy]") {
    SoapyRig rig;
    const SoapySDR::Range lna = rig.device->getGainRange(SOAPY_SDR_RX, 0, "LNA");
    CHECK(lna.minimum() == 0);
    CHECK(lna.maximum() == 40);
    CHECK(lna.step() == 8);
    const SoapySDR::Range vga = rig.device->getGainRange(SOAPY_SDR_RX, 0, "VGA");
    CHECK(vga.maximum() == 62);
    CHECK(vga.step() == 2);
    const SoapySDR::Range tx_vga = rig.device->getGainRange(SOAPY_SDR_TX, 0, "VGA");
    CHECK(tx_vga.maximum() == 47);
    CHECK(tx_vga.step() == 1);
    const SoapySDR::Range amp = rig.device->getGainRange(SOAPY_SDR_TX, 0, "AMP");
    CHECK(amp.maximum() == 14);
    CHECK(amp.step() == 14);
    CHECK_THROWS_AS(rig.device->getGainRange(SOAPY_SDR_TX, 0, "LNA"), std::invalid_argument);
}

TEST_CASE("frequency, sample rate and bandwidth read back what was set", "[soapy]") {
    SoapyRig rig;
    // As an application calls it, through the base class.
    SoapySDR::Device& device = *rig.device;
    device.setFrequency(SOAPY_SDR_RX, 0, 915e6);
    device.setFrequency(SOAPY_SDR_TX, 0, "RF", 433.92e6);
    CHECK(rig.device->getFrequency(SOAPY_SDR_RX, 0) == 915e6);
    CHECK(rig.device->getFrequency(SOAPY_SDR_TX, 0, "RF") == 433.92e6);
    CHECK(rig.rx->frequency_hz == 915'000'000);
    CHECK(rig.tx->frequency_hz == 433'920'000);
    CHECK_THROWS_AS(rig.device->getFrequency(SOAPY_SDR_RX, 0, "BB"), std::invalid_argument);

    rig.device->setSampleRate(SOAPY_SDR_TX, 0, 10e6);
    CHECK(rig.device->getSampleRate(SOAPY_SDR_TX, 0) == 10e6);
    CHECK(rig.tx->sample_rate_hz == 10e6);

    rig.device->setSampleRate(SOAPY_SDR_RX, 0, 8e6);
    rig.device->setBandwidth(SOAPY_SDR_RX, 0, 5e6);
    CHECK(rig.device->getBandwidth(SOAPY_SDR_RX, 0) == 5e6);
    CHECK(rig.rx->bandwidth_hz == 5'000'000);
    // 0: back to the filter that follows the rate.
    rig.device->setBandwidth(SOAPY_SDR_RX, 0, 0);
    CHECK(rig.device->getBandwidth(SOAPY_SDR_RX, 0) == 6e6);
}

TEST_CASE("no automatic DC or I/Q correction is claimed", "[soapy]") {
    SoapyRig rig;
    for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX}) {
        CHECK_FALSE(rig.device->getDCOffsetMode(direction, 0));
        CHECK_FALSE(rig.device->getIQBalanceMode(direction, 0));
    }
}

TEST_CASE("a stream's MTU is one transfer", "[soapy]") {
    SoapyRig rig;
    rig.rx->transfer_bytes_value = 262144;
    SoapySDR::Device& device = *rig.device;
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    CHECK(rig.device->getStreamMTU(rx) == 131072);
    rig.device->closeStream(rx);
}

TEST_CASE("calls the device cannot honour are refused by name", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    CHECK_THROWS_AS(device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, {1}), std::invalid_argument);
    CHECK_THROWS_AS(device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, {0, 0}), std::invalid_argument);
    CHECK_THROWS_AS(device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8, {}, {{"buffers", "0"}}),
                    std::invalid_argument);
    CHECK_THROWS_AS(device.setAntenna(SOAPY_SDR_RX, 0, "ANT2"), std::invalid_argument);
    CHECK_NOTHROW(device.setAntenna(SOAPY_SDR_TX, 0, "TX/RX"));
    CHECK_THROWS_AS(device.setFrequency(SOAPY_SDR_RX, 0, "BB", 1e6), std::invalid_argument);
    CHECK_THROWS_AS(device.setFrequency(SOAPY_SDR_RX, 0, -1.0), std::invalid_argument);
    CHECK_THROWS_AS(device.getFrequencyRange(SOAPY_SDR_RX, 0, "BB"), std::invalid_argument);
    CHECK_THROWS_AS(device.getHardwareTime("PPS"), std::invalid_argument);
    CHECK_THROWS_AS(device.setHardwareTime(0, "PPS"), std::invalid_argument);
    CHECK_THROWS_AS(device.getSensorInfo("temperature"), std::invalid_argument);
    // A handle this device did not give out.
    int token = 0;
    CHECK_THROWS_AS(device.getStreamMTU(reinterpret_cast<SoapySDR::Stream*>(&token)),
                    std::invalid_argument);
}

TEST_CASE("the receive gains are listed in the order gr-osmosdr expects", "[soapy]") {
    SoapyRig rig;
    CHECK(rig.device->listGains(SOAPY_SDR_RX, 0) == std::vector<std::string>{"LNA", "AMP", "VGA"});
    CHECK(rig.device->listGains(SOAPY_SDR_TX, 0) == std::vector<std::string>{"VGA", "AMP"});
    const SoapySDR::Range rx = rig.device->getGainRange(SOAPY_SDR_RX, 0);
    CHECK(rx.minimum() == 0);
    CHECK(rx.maximum() == 40 + 62 + 14);
    const SoapySDR::Range tx = rig.device->getGainRange(SOAPY_SDR_TX, 0);
    CHECK(tx.maximum() == 47 + 14);
}

TEST_CASE("the filter widths are listed, and offered as single points", "[soapy]") {
    SoapyRig rig;
    const std::vector<double> widths = rig.device->listBandwidths(SOAPY_SDR_RX, 0);
    REQUIRE(widths.size() == 16);
    CHECK(widths.front() == 1.75e6);
    CHECK(widths.back() == 28e6);
    const SoapySDR::RangeList ranges = rig.device->getBandwidthRange(SOAPY_SDR_TX, 0);
    REQUIRE(ranges.size() == widths.size());
    CHECK(ranges[3].minimum() == widths[3]);
    CHECK(ranges[3].maximum() == widths[3]);
}

TEST_CASE("the hardware time needs the receive rate", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    // No rate, no time: it reads 0 and cannot be set.
    CHECK(device.getHardwareTime() == 0);
    CHECK_THROWS_AS(device.setHardwareTime(1'000'000'000), std::logic_error);
    device.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
    device.setHardwareTime(1'000'000'000);
    CHECK(device.getHardwareTime() == 1'000'000'000);
}

TEST_CASE("each stream call refuses the other direction's stream", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    SoapySDR::Stream* tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    std::vector<std::complex<float>> buffer(16);
    void* out[] = {buffer.data()};
    const void* in[] = {buffer.data()};
    int flags = 0;
    long long time_ns = 0;
    size_t mask = 0;
    CHECK(device.readStream(tx, out, 16, flags, time_ns, 0) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(device.writeStream(rx, in, 16, flags, 0, 0) == SOAPY_SDR_NOT_SUPPORTED);
    CHECK(device.readStreamStatus(rx, mask, flags, time_ns, 0) == SOAPY_SDR_NOT_SUPPORTED);
    // Direct access is for a CS8 receive stream only.
    CHECK_THROWS_AS(device.releaseReadBuffer(rx, 0), std::invalid_argument);
    device.closeStream(rx);
    device.closeStream(tx);
}

TEST_CASE("a timed write without a receive rate or an origin is a time error", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    rig.device->writeSetting("time_origin", "none");
    SoapySDR::Stream* tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(device.activateStream(tx) == 0);
    std::vector<std::int8_t> samples(32, 0);
    const void* in[] = {samples.data()};
    int flags = SOAPY_SDR_HAS_TIME;
    // No receive rate: no time to place it at.
    CHECK(device.writeStream(tx, in, 16, flags, 1'000'000, 1000) == SOAPY_SDR_TIME_ERROR);
    // A rate, but no origin to tie the streams.
    rig.device->setSampleRate(SOAPY_SDR_RX, 0, 8e6);
    flags = SOAPY_SDR_HAS_TIME;
    CHECK(device.writeStream(tx, in, 16, flags, 1'000'000, 1000) == SOAPY_SDR_TIME_ERROR);
    device.deactivateStream(tx);
    device.closeStream(tx);
}

TEST_CASE("a timed receive start needs the receive rate", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    CHECK(device.activateStream(rx, SOAPY_SDR_HAS_TIME, 1'000'000) == SOAPY_SDR_TIME_ERROR);
    CHECK(rig.logged(SOAPY_SDR_ERROR, "set the receive sample rate first"));
    device.closeStream(rx);
}

TEST_CASE("direct access on a stream not running is a stream error", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    size_t handle = 0;
    const void* buffs[] = {nullptr};
    int flags = 0;
    long long time_ns = 0;
    CHECK(device.acquireReadBuffer(rx, handle, buffs, flags, time_ns, 0) == SOAPY_SDR_STREAM_ERROR);
    device.closeStream(rx);
}

TEST_CASE("a board that fails to start is a stream error", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    rig.rx->fail_on = "start_rx";
    CHECK(device.activateStream(rx) == SOAPY_SDR_STREAM_ERROR);
    CHECK(rig.logged(SOAPY_SDR_ERROR, "start_rx"));
    device.closeStream(rx);
}

TEST_CASE("a read or write of no samples returns at once", "[soapy]") {
    SoapyRig rig;
    SoapySDR::Device& device = *rig.device;
    rig.device->writeSetting("time_origin", "none");
    device.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
    device.setSampleRate(SOAPY_SDR_TX, 0, 8e6);
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8);
    SoapySDR::Stream* tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8);
    REQUIRE(device.activateStream(rx) == 0);
    REQUIRE(device.activateStream(tx) == 0);
    std::vector<std::int8_t> buffer(32, 0);
    const void* in[] = {buffer.data()};
    void* out[] = {buffer.data()};
    const auto start = std::chrono::steady_clock::now();
    // An empty write that ends a burst is how a writer closes one.
    int flags = SOAPY_SDR_END_BURST;
    CHECK(device.writeStream(tx, in, 0, flags, 0, 1'000'000) == 0);
    flags = 0;
    long long time_ns = 0;
    CHECK(device.readStream(rx, out, 0, flags, time_ns, 1'000'000) == 0);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
    device.deactivateStream(tx);
    device.deactivateStream(rx);
    device.closeStream(tx);
    device.closeStream(rx);
}

#if defined(HACKRFDUPLEX_DSP)
TEST_CASE("receive corrections in a DSP build: offered, kept and read back", "[soapy][dsp]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    CHECK(d.hasDCOffsetMode(SOAPY_SDR_RX, 0));
    CHECK(d.hasDCOffset(SOAPY_SDR_RX, 0));
    CHECK(d.hasIQBalance(SOAPY_SDR_RX, 0));
    CHECK(d.hasIQBalanceMode(SOAPY_SDR_RX, 0));
    // All off until set.
    CHECK_FALSE(d.getDCOffsetMode(SOAPY_SDR_RX, 0));
    CHECK_FALSE(d.getIQBalanceMode(SOAPY_SDR_RX, 0));
    CHECK(d.getDCOffset(SOAPY_SDR_RX, 0) == std::complex<double>{});
    CHECK(d.getIQBalance(SOAPY_SDR_RX, 0) == std::complex<double>{});
    d.setDCOffsetMode(SOAPY_SDR_RX, 0, true);
    CHECK(d.getDCOffsetMode(SOAPY_SDR_RX, 0));
    d.setDCOffset(SOAPY_SDR_RX, 0, {0.1, -0.2});
    CHECK(d.getDCOffset(SOAPY_SDR_RX, 0) == std::complex<double>{0.1, -0.2});
    d.setIQBalance(SOAPY_SDR_RX, 0, {0.01, 0.02});
    CHECK(d.getIQBalance(SOAPY_SDR_RX, 0) == std::complex<double>{0.01, 0.02});
    d.setIQBalanceMode(SOAPY_SDR_RX, 0, true);
    CHECK(d.getIQBalanceMode(SOAPY_SDR_RX, 0));
    // The transmitter still has none.
    CHECK_FALSE(d.hasDCOffsetMode(SOAPY_SDR_TX, 0));
    CHECK_THROWS_AS(d.setDCOffset(SOAPY_SDR_TX, 0, {0.1, 0.0}), std::invalid_argument);
    CHECK_THROWS_AS(d.setIQBalanceMode(SOAPY_SDR_TX, 0, true), std::invalid_argument);
}

TEST_CASE("receive corrections in a DSP build: a set offset comes off what is read",
          "[soapy][dsp]") {
    SoapyRig rig;
    SoapySDR::Device& d = *rig.device;
    d.writeSetting("time_origin", "none");
    d.setSampleRate(SOAPY_SDR_RX, 0, 8e6);
    d.setDCOffset(SOAPY_SDR_RX, 0, {0.25, -0.25});
    SoapySDR::Stream* rx = d.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    REQUIRE(d.activateStream(rx) == 0);
    // Eight samples at (0.5, -0.5) as the board delivers them.
    std::vector<std::uint8_t> transfer(rig.rx->transfer_bytes_value);
    for (std::size_t i = 0; i < transfer.size(); i += 2) {
        transfer[i] = static_cast<std::uint8_t>(64);
        transfer[i + 1] = static_cast<std::uint8_t>(-64);
    }
    rig.rx->deliver_rx(transfer);
    std::vector<std::complex<float>> out(8);
    void* buffs[] = {out.data()};
    int flags = 0;
    long long time_ns = 0;
    REQUIRE(d.readStream(rx, buffs, out.size(), flags, time_ns, 100'000) == 8);
    for (const auto& sample : out) {
        CHECK(std::abs(sample - std::complex<float>(0.25F, -0.25F)) < 1e-6F);
    }
    d.deactivateStream(rx);
    d.closeStream(rx);
}
#endif
