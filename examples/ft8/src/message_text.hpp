// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
#pragma once

#include <optional>
#include <string>

#include "ft8/constants.h"

namespace ft8 {

// A payload as standard FT8 text, through ft8_lib's message code; nothing
// for a payload that is not a standard message.
std::optional<std::string> message_text(const Payload& payload);

// Standard FT8 text as a payload, or nothing if it does not encode.
std::optional<Payload> message_payload(const std::string& text);

}  // namespace ft8
