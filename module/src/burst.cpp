// SPDX-License-Identifier: GPL-3.0-or-later
#include "burst.hpp"

#include <algorithm>
#include <cmath>
#include <complex>

#include "fft.hpp"

namespace hackrfduplex::burst {

namespace {

constexpr unsigned kRegisterBits = 13;
constexpr unsigned kTaps[] = {13, 4, 3, 1};
// Where in the 8191-chip period the burst starts.
constexpr std::size_t kSkip = 7168;
constexpr std::size_t kFftSize = 16384;

}  // namespace

std::vector<std::int8_t> chips() {
    std::uint32_t state = 1;
    std::vector<std::int8_t> out;
    out.reserve(kChips);
    for (std::size_t i = 0; i < kSkip + kChips; ++i) {
        if (i >= kSkip) {
            out.push_back((state & 1U) != 0 ? 1 : -1);
        }
        std::uint32_t feedback = 0;
        for (const unsigned tap : kTaps) {
            feedback ^= (state >> (kRegisterBits - tap)) & 1U;
        }
        state = (state >> 1) | (feedback << (kRegisterBits - 1));
    }
    return out;
}

std::vector<std::int8_t> samples() {
    const std::vector<std::int8_t> c = chips();
    std::vector<std::int8_t> out(2 * kChips, 0);
    for (std::size_t k = 0; k < kChips; ++k) {
        out[2 * k] = static_cast<std::int8_t>(c[k] * kAmplitude);
    }
    return out;
}

Found find(std::span<const std::int8_t> iq) {
    const std::size_t count = iq.size() / 2;
    if (count < kChips) {
        return {};
    }
    // Overlap-save: each FFT block yields kFftSize - kChips + 1 lags whose
    // windows lie wholly inside it.
    const std::size_t lags = count - kChips + 1;
    const std::size_t step = kFftSize - kChips + 1;

    std::vector<std::complex<float>> reference(kFftSize);
    const std::vector<std::int8_t> c = chips();
    for (std::size_t k = 0; k < kChips; ++k) {
        reference[k] = static_cast<float>(c[k]);
    }
    fft(reference, false);
    for (std::complex<float>& r : reference) {
        r = std::conj(r);
    }

    std::vector<float> magnitude(lags);
    std::vector<std::complex<float>> block(kFftSize);
    for (std::size_t start = 0; start < lags; start += step) {
        for (std::size_t i = 0; i < kFftSize; ++i) {
            const std::size_t n = start + i;
            block[i] = n < count ? std::complex<float>(iq[2 * n], iq[2 * n + 1])
                                 : std::complex<float>(0, 0);
        }
        fft(block, false);
        for (std::size_t i = 0; i < kFftSize; ++i) {
            block[i] *= reference[i];
        }
        fft(block, true);
        for (std::size_t k = 0; k < step && start + k < lags; ++k) {
            magnitude[start + k] = std::abs(block[k]);
        }
    }

    const auto peak = std::max_element(magnitude.begin(), magnitude.end());
    Found found;
    found.offset = static_cast<std::size_t>(peak - magnitude.begin());
    const float peak_value = *peak;
    std::vector<float> sorted = magnitude;
    const auto middle = sorted.begin() + static_cast<long>(sorted.size() / 2);
    std::nth_element(sorted.begin(), middle, sorted.end());
    const double median = *middle;
    if (peak_value <= 0) {
        return found;
    }
    found.peak_over_median_db = median > 0 ? 20.0 * std::log10(peak_value / median) : 200.0;
    found.found = found.peak_over_median_db >= kMinPeakOverMedianDb;
    return found;
}

}  // namespace hackrfduplex::burst
