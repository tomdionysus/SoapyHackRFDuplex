// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <string>
#include <vector>

#include "board.hpp"

namespace hackrfduplex {

// Which board receives and which transmits, by full serial.
struct Selection {
    std::string rx;
    std::string tx;
};

using Args = std::map<std::string, std::string>;

// The keys a caller names boards with. A value matches a board whose
// serial ends with it, so the zero padding may be left off.
inline constexpr const char* kRxSerialKey = "rx_serial";
inline constexpr const char* kTxSerialKey = "tx_serial";

// The pair `args` asks for, from the boards on the bus: both serials
// named; one named and exactly one other board to take the other role;
// or neither named and exactly two boards, the lower serial receiving.
// Throws std::invalid_argument saying what is missing or ambiguous.
Selection select_pair(const Args& args, const std::vector<ListedBoard>& boards);

// What discovery offers: the one pair `args` names (nothing if it names
// one that cannot be made), or with no serial named, every ordered pair
// of distinct boards.
std::vector<Selection> enumerate_pairs(const Args& args, const std::vector<ListedBoard>& boards);

// A label for a pair, with the serials' zero padding trimmed.
std::string pair_label(const Selection& selection);

// Trims the leading zeros libhackrf pads a serial with.
std::string short_serial(const std::string& serial);

}  // namespace hackrfduplex
