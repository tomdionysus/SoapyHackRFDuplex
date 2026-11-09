// SPDX-License-Identifier: GPL-3.0-or-later
#include "soapy_device.hpp"

#include <SoapySDR/Constants.h>
#include <SoapySDR/Logger.h>

#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
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
