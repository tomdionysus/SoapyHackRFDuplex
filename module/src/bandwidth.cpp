// SPDX-License-Identifier: GPL-3.0-or-later
#include "bandwidth.hpp"

#include <array>

namespace hackrfduplex {

namespace {

constexpr std::array<std::uint32_t, 16> kBandwidths = {
    1'750'000, 2'500'000,  3'500'000,  5'000'000,  5'500'000,  6'000'000,  7'000'000,  8'000'000,
    9'000'000, 10'000'000, 12'000'000, 14'000'000, 15'000'000, 20'000'000, 24'000'000, 28'000'000,
};

}  // namespace

std::span<const std::uint32_t> filter_bandwidths() { return kBandwidths; }

std::uint32_t round_down_bandwidth(double hz) {
    std::uint32_t chosen = kBandwidths.front();
    for (const std::uint32_t candidate : kBandwidths) {
        if (static_cast<double>(candidate) <= hz) {
            chosen = candidate;
        }
    }
    return chosen;
}

std::uint32_t auto_bandwidth(double sample_rate_hz) {
    if (!(sample_rate_hz > 0)) {
        return 0;
    }
    return round_down_bandwidth(0.75 * sample_rate_hz);
}

}  // namespace hackrfduplex
