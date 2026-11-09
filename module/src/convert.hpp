// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

#include "format.hpp"

namespace hackrfduplex {

// Full scale of the board's int8 samples, as SoapySDR reports it for CS8.
inline constexpr float kFullScale = 128.0f;

// Converts `samples` complex samples at `src`, in `format`, into the
// board's int8 pairs at `dst`. Floats are scaled by kFullScale, rounded
// to nearest and clamped; int16 keeps its top byte.
void to_native(const void* src, Format format, std::int8_t* dst, std::size_t samples);

// The reverse: `samples` int8 pairs at `src` into `format` at `dst`.
void from_native(const std::int8_t* src, Format format, void* dst, std::size_t samples);

}  // namespace hackrfduplex
