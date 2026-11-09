// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace hackrfduplex {

// Host-side sample formats, named as SoapySDR names them: complex pairs
// of int8 (the board's own), int16, float or double.
enum class Format { kCs8, kCs16, kCf32, kCf64 };

// Bytes per complex sample on the board: one int8 for I, one for Q.
inline constexpr std::size_t kNativeSampleBytes = 2;

// Bytes per complex sample of `format`.
std::size_t sample_bytes(Format format);

// From a SoapySDR format name ("CS8", "CS16", "CF32", "CF64"). Throws
// std::invalid_argument for any other name.
Format parse_format(const std::string& name);

const char* format_name(Format format);

}  // namespace hackrfduplex
