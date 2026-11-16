// SPDX-License-Identifier: GPL-3.0-or-later
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <exception>
#include <memory>

#include "hackrf_board.hpp"
#include "selection.hpp"
#include "soapy_device.hpp"

namespace {

// Discovery never opens a board, so a pair already in use in this
// process is still listed.
SoapySDR::KwargsList find_pairs(const SoapySDR::Kwargs& args) {
    SoapySDR::KwargsList results;
    try {
        const std::vector<hackrfduplex::ListedBoard> boards = hackrfduplex::list_boards();
        for (const hackrfduplex::Selection& pair : hackrfduplex::enumerate_pairs(args, boards)) {
            SoapySDR::Kwargs result;
            result[hackrfduplex::kRxSerialKey] = pair.rx;
            result[hackrfduplex::kTxSerialKey] = pair.tx;
            result["label"] = hackrfduplex::pair_label(pair);
            results.push_back(result);
        }
    } catch (const std::exception& e) {
        SoapySDR::log(SOAPY_SDR_WARNING,
                      std::string("hackrfduplex: discovery failed: ") + e.what());
    }
    return results;
}

// Opens the boards `args` select (selection.hpp); throws when they cannot
// be chosen or opened.
SoapySDR::Device* make_pair(const SoapySDR::Kwargs& args) {
    using namespace hackrfduplex;
    const Selection selection = select_pair(args, list_boards());
    return new SoapyDevice(
        std::make_unique<Pair>(std::make_unique<HackrfBoard>(selection.rx),
                               std::make_unique<HackrfBoard>(selection.tx), &soapy_log),
        args);
}

SoapySDR::Registry registration("hackrfduplex", &find_pairs, &make_pair, SOAPY_SDR_ABI_VERSION);

}  // namespace
