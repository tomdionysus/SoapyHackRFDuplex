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
        out[2 * k + 1] = static_cast<std::int8_t>(c[k] * kAmplitude);
    }
    return out;
}

Found find(std::span<const std::int8_t> iq) {
    Finder finder;
    finder.finish(iq);
    return finder.result();
}

Finder::Finder() : reference_(kFftSize), block_(kFftSize) {
    const std::vector<std::int8_t> c = chips();
    for (std::size_t k = 0; k < kChips; ++k) {
        reference_[k] = static_cast<float>(c[k]);
    }
    fft(reference_, false);
    for (std::complex<float>& r : reference_) {
        r = std::conj(r);
    }
}

void Finder::advance(std::span<const std::int8_t> iq) { correlate(iq); }

void Finder::finish(std::span<const std::int8_t> iq) { correlate(iq); }

void Finder::correlate(std::span<const std::int8_t> iq) {
    const std::size_t count = iq.size() / 2;
    for (; summed_ < count; ++summed_) {
        sum_i_ += iq[2 * summed_];
        sum_q_ += iq[2 * summed_ + 1];
    }
    if (count < kChips) {
        return;
    }
    const std::complex<float> dc(static_cast<float>(sum_i_ / static_cast<double>(count)),
                                 static_cast<float>(sum_q_ / static_cast<double>(count)));
    // Overlap-save: each FFT block yields kFftSize - kChips + 1 lags whose
    // windows lie wholly inside it. A block the capture does not yet fill
    // is padded, and only the lags whose windows the capture covers are
    // kept, so they are what a full block would give; the next call takes
    // up from the first lag not kept.
    const std::size_t lags = count - kChips + 1;
    const std::size_t step = kFftSize - kChips + 1;
    for (std::size_t start = magnitude_.size(); start < lags; start = magnitude_.size()) {
        for (std::size_t i = 0; i < kFftSize; ++i) {
            const std::size_t n = start + i;
            block_[i] = n < count ? std::complex<float>(iq[2 * n], iq[2 * n + 1]) - dc
                                  : std::complex<float>(0, 0);
        }
        fft(block_, false);
        for (std::size_t i = 0; i < kFftSize; ++i) {
            block_[i] *= reference_[i];
        }
        fft(block_, true);
        for (std::size_t k = 0; k < step && start + k < lags; ++k) {
            magnitude_.push_back(std::abs(block_[k]));
        }
    }
}

Found Finder::result() const {
    Found found;
    if (magnitude_.empty()) {
        return found;
    }
    const auto peak = std::max_element(magnitude_.begin(), magnitude_.end());
    found.offset = static_cast<std::size_t>(peak - magnitude_.begin());
    const float peak_value = *peak;
    std::vector<float> sorted = magnitude_;
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

bool Finder::settled() const {
    const Found found = result();
    return found.found && magnitude_.size() > found.offset + kChips;
}

}  // namespace hackrfduplex::burst
