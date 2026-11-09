// SPDX-License-Identifier: GPL-3.0-or-later
#include "format.hpp"

#include <stdexcept>

namespace hackrfduplex {

std::size_t sample_bytes(Format format) {
    switch (format) {
        case Format::kCs8:
            return 2;
        case Format::kCs16:
            return 4;
        case Format::kCf32:
            return 8;
        case Format::kCf64:
            return 16;
    }
    return 0;
}

Format parse_format(const std::string& name) {
    if (name == "CS8") {
        return Format::kCs8;
    }
    if (name == "CS16") {
        return Format::kCs16;
    }
    if (name == "CF32") {
        return Format::kCf32;
    }
    if (name == "CF64") {
        return Format::kCf64;
    }
    throw std::invalid_argument("unsupported stream format " + name);
}

const char* format_name(Format format) {
    switch (format) {
        case Format::kCs8:
            return "CS8";
        case Format::kCs16:
            return "CS16";
        case Format::kCf32:
            return "CF32";
        case Format::kCf64:
            return "CF64";
    }
    return "?";
}

}  // namespace hackrfduplex
