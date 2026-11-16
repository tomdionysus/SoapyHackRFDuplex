// SPDX-License-Identifier: GPL-3.0-or-later
//
// Choosing the pair from device arguments and the boards on the bus, as
// discovery and opening do with whatever an application passes. A pair
// chosen is two different boards from the bus; anything else is refused
// with std::invalid_argument, and discovery never throws.

#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "selection.hpp"

using namespace hackrfduplex;

namespace {

bool listed(const std::vector<ListedBoard>& boards, const std::string& serial) {
    return std::any_of(boards.begin(), boards.end(),
                       [&](const ListedBoard& b) { return b.serial == serial; });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    FuzzedDataProvider in(data, size);
    // Any serials, shared ones included, as clones can report.
    std::vector<ListedBoard> boards(in.ConsumeIntegralInRange<std::size_t>(0, 4));
    for (ListedBoard& board : boards) {
        board.serial = in.ConsumeRandomLengthString(40);
        board.name = "HackRF One";
    }
    Args args;
    const std::size_t keys = in.ConsumeIntegralInRange<std::size_t>(0, 3);
    for (std::size_t k = 0; k < keys; ++k) {
        const int which = in.ConsumeIntegralInRange<int>(0, 2);
        const std::string key = which == 0   ? kRxSerialKey
                                : which == 1 ? kTxSerialKey
                                             : in.ConsumeRandomLengthString(16);
        args[key] = in.ConsumeRandomLengthString(40);
    }

    bool refused = false;
    Selection chosen;
    try {
        chosen = select_pair(args, boards);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    if (!refused) {
        if (chosen.rx == chosen.tx || !listed(boards, chosen.rx) || !listed(boards, chosen.tx)) {
            std::abort();
        }
        (void)pair_label(chosen);
    }
    for (const Selection& pair : enumerate_pairs(args, boards)) {
        if (pair.rx == pair.tx || !listed(boards, pair.rx) || !listed(boards, pair.tx)) {
            std::abort();
        }
    }
    for (const ListedBoard& board : boards) {
        const std::string shorter = short_serial(board.serial);
        if (shorter.size() > board.serial.size() || !board.serial.ends_with(shorter)) {
            std::abort();
        }
    }
    return 0;
}
