// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <complex>
#include <vector>

namespace hackrfduplex {

// In-place radix-2 FFT, written here rather than taken from a library for
// the one correlation the pair does. The inverse is scaled by 1/N, so the
// two round trip. Throws std::invalid_argument unless the size is a power
// of two.
void fft(std::vector<std::complex<float>>& data, bool inverse);

}  // namespace hackrfduplex
