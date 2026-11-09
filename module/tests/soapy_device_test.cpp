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
        device = std::make_unique<SoapyDevice>(
            std::make_unique<Pair>(std::move(rx_owner), std::move(tx_owner), nullptr), args);
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
