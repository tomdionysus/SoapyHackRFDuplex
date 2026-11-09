// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The calibration burst that gives the two streams a common time origin
// (docs/design.md, Time): the transmitter sends it at a known transmit
// position, the receiver's samples are searched for it, and where it
// lands in the receive stream ties the two sample counts together.
//
// It is BPSK on I, one chip per sample, so it needs no knowledge of the
// sample rate: 4096 chips of the 13-bit maximal-length sequence
// x^13 + x^4 + x^3 + x + 1, from the point in its period where the
// aperiodic autocorrelation's sidelobes are lowest (32 dB under the peak).
namespace hackrfduplex::burst {

inline constexpr std::size_t kChips = 4096;
// 0.7 of int8 full scale.
inline constexpr std::int8_t kAmplitude = 89;
// A correlation peak this far over the median correlation is the burst.
inline constexpr double kMinPeakOverMedianDb = 20.0;

// The chips, +1 or -1.
std::vector<std::int8_t> chips();
// The burst as native interleaved int8 I/Q.
std::vector<std::int8_t> samples();

struct Found {
    bool found = false;
    // Sample index in the searched capture of the burst's first chip.
    std::size_t offset = 0;
    // The correlation peak over the median correlation magnitude.
    double peak_over_median_db = 0;
};

// Searches interleaved int8 I/Q for the burst at any phase: the strongest
// correlation, accepted when it stands kMinPeakOverMedianDb over the
// median. Fast correlation by overlap-save, so a capture of millions of
// samples takes a fraction of a second.
Found find(std::span<const std::int8_t> iq);

}  // namespace hackrfduplex::burst
