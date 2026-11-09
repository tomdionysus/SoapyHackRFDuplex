// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>

namespace hackrfduplex {

// The MAX2837 baseband filter's selectable widths, in Hz, ascending.
std::span<const std::uint32_t> filter_bandwidths();

// The widest filter not wider than `hz`, and the narrowest for anything
// below it. The anti-aliasing choice: never wider than asked.
std::uint32_t round_down_bandwidth(double hz);

// What the firmware chooses when a sample rate is set: the widest filter
// not wider than three quarters of the rate. 0 for a rate of 0.
std::uint32_t auto_bandwidth(double sample_rate_hz);

}  // namespace hackrfduplex
