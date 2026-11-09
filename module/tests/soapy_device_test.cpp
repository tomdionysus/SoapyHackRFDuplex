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
