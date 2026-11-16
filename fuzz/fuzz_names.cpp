// SPDX-License-Identifier: GPL-3.0-or-later
//
// The names an application passes as text: stream formats, clock outputs
// and time origins. Any text is either a name, which reads back as itself,
// or refused with std::invalid_argument; nothing else may happen.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "format.hpp"
#include "pair.hpp"

using namespace hackrfduplex;

namespace {

// `parse` then `name` gives back `text`, or `parse` refuses it.
template <typename Parse, typename Name>
void round_trips(const std::string& text, Parse parse, Name name) {
    bool refused = false;
    std::string back;
    try {
        back = name(parse(text));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    if (!refused && back != text) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text(reinterpret_cast<const char*>(data), size);
    round_trips(text, parse_format, format_name);
    round_trips(text, parse_clock_out, clock_out_name);
    round_trips(text, parse_time_origin, time_origin_name);
    return 0;
}
