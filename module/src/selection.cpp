// SPDX-License-Identifier: GPL-3.0-or-later
#include "selection.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>

namespace hackrfduplex {

namespace {

std::optional<std::string> match(const Args& args, const char* key,
                                 const std::vector<ListedBoard>& boards) {
    const auto it = args.find(key);
    if (it == args.end() || it->second.empty()) {
        return std::nullopt;
    }
    const std::string& wanted = it->second;
    std::vector<std::string> found;
    for (const ListedBoard& board : boards) {
        if (board.serial.ends_with(wanted)) {
            found.push_back(board.serial);
        }
    }
    if (found.empty()) {
        throw std::invalid_argument(std::string("no HackRF matches ") + key + "=" + wanted);
    }
    if (found.size() > 1) {
        throw std::invalid_argument(std::string(key) + "=" + wanted + " matches " +
                                    std::to_string(found.size()) + " boards");
    }
    return found.front();
}

std::string the_other(const std::string& chosen, const char* chosen_key,
                      const std::vector<ListedBoard>& boards) {
    std::vector<std::string> others;
    for (const ListedBoard& board : boards) {
        if (board.serial != chosen) {
            others.push_back(board.serial);
        }
    }
    if (others.size() != 1) {
        throw std::invalid_argument(std::string("with only ") + chosen_key +
                                    " given, exactly one other HackRF must be attached, not " +
                                    std::to_string(others.size()));
    }
    return others.front();
}

}  // namespace

std::string short_serial(const std::string& serial) {
    const std::size_t first = serial.find_first_not_of('0');
    if (first == std::string::npos) {
        return serial.empty() ? serial : serial.substr(serial.size() - 1);
    }
    return serial.substr(first);
}

std::string pair_label(const Selection& selection) {
    return "HackRF Duplex RX " + short_serial(selection.rx) + " TX " + short_serial(selection.tx);
}

Selection select_pair(const Args& args, const std::vector<ListedBoard>& boards) {
    const std::optional<std::string> rx = match(args, kRxSerialKey, boards);
    const std::optional<std::string> tx = match(args, kTxSerialKey, boards);
    if (rx && tx) {
        if (*rx == *tx) {
            throw std::invalid_argument("rx_serial and tx_serial name the same board " + *rx);
        }
        return Selection{*rx, *tx};
    }
    if (rx) {
        return Selection{*rx, the_other(*rx, kRxSerialKey, boards)};
    }
    if (tx) {
        return Selection{the_other(*tx, kTxSerialKey, boards), *tx};
    }
    if (boards.size() != 2) {
        throw std::invalid_argument(
            "give rx_serial and tx_serial: " + std::to_string(boards.size()) +
            " HackRF boards attached, and a pair needs exactly two to choose by itself");
    }
    std::vector<std::string> serials = {boards[0].serial, boards[1].serial};
    std::sort(serials.begin(), serials.end());
    return Selection{serials[0], serials[1]};
}

std::vector<Selection> enumerate_pairs(const Args& args, const std::vector<ListedBoard>& boards) {
    std::vector<Selection> pairs;
    if (args.count(kRxSerialKey) != 0 || args.count(kTxSerialKey) != 0) {
        try {
            pairs.push_back(select_pair(args, boards));
        } catch (const std::invalid_argument&) {
        }
        return pairs;
    }
    for (const ListedBoard& rx : boards) {
        for (const ListedBoard& tx : boards) {
            if (rx.serial != tx.serial) {
                pairs.push_back(Selection{rx.serial, tx.serial});
            }
        }
    }
    return pairs;
}

}  // namespace hackrfduplex
