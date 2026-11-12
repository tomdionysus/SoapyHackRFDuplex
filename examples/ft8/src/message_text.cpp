// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
#include "message_text.hpp"

#include <algorithm>

extern "C" {
#include "ft8_lib/message.h"
}

namespace ft8 {

std::optional<std::string> message_text(const Payload& payload) {
    ftx_message_t message{};
    std::copy(payload.begin(), payload.end(), message.payload);
    char text[FTX_MAX_MESSAGE_LENGTH] = {};
    ftx_message_offsets_t offsets{};
    if (ftx_message_decode(&message, nullptr, text, &offsets) != FTX_MESSAGE_RC_OK) {
        return std::nullopt;
    }
    return std::string(text);
}

std::optional<Payload> message_payload(const std::string& text) {
    ftx_message_t message{};
    ftx_message_init(&message);
    if (ftx_message_encode(&message, nullptr, text.c_str()) != FTX_MESSAGE_RC_OK) {
        return std::nullopt;
    }
    Payload payload{};
    std::copy(message.payload, message.payload + payload.size(), payload.begin());
    return payload;
}

}  // namespace ft8
