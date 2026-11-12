// SPDX-License-Identifier: GPL-3.0-or-later
#include "fft.hpp"

#include <numbers>
#include <stdexcept>
#include <utility>

namespace hackrfduplex {

void fft(std::vector<std::complex<float>>& data, bool inverse) {
    const std::size_t n = data.size();
    if (n == 0 || (n & (n - 1)) != 0) {
        throw std::invalid_argument("fft: size must be a power of two");
    }
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    const double sign = inverse ? 1.0 : -1.0;
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len / 2;
        for (std::size_t k = 0; k < half; ++k) {
            // Twiddles in double, each from its own angle, so the error does
            // not build up along a butterfly row.
            const std::complex<float> w(std::polar(
                1.0,
                sign * 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(len)));
            for (std::size_t i = 0; i < n; i += len) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + half] * w;
                data[i + k] = u + v;
                data[i + k + half] = u - v;
            }
        }
    }
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(n);
        for (std::complex<float>& x : data) {
            x *= scale;
        }
    }
}

}  // namespace hackrfduplex
