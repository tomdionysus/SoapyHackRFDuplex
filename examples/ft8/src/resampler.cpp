// SPDX-License-Identifier: GPL-3.0-or-later
#include "resampler.hpp"

#include <array>

namespace ft8 {
namespace {

constexpr unsigned kTaps = kResampleFactor * 10 + 1;
constexpr float kCutoffHz = 20000.0F;
constexpr float kStopbandDb = 70.0F;

// Normalised to sum to `gain`.
std::array<float, kTaps> design(float gain) {
    std::array<float, kTaps> taps{};
    liquid_firdes_kaiser(kTaps, kCutoffHz / kPairRate, kStopbandDb, 0.0F, taps.data());
    float sum = 0;
    for (const float t : taps) {
        sum += t;
    }
    for (float& t : taps) {
        t *= gain / sum;
    }
    return taps;
}

}  // namespace

DownTo48k::DownTo48k() {
    std::array<float, kTaps> taps = design(1.0F);
    filter_.reset(firdecim_crcf_create(kResampleFactor, taps.data(), kTaps));
    pending_.reserve(kResampleFactor);
}

void DownTo48k::process(std::span<const std::complex<float>> input,
                        std::vector<std::complex<float>>& out) {
    for (const std::complex<float>& x : input) {
        pending_.push_back(x);
        if (pending_.size() == kResampleFactor) {
            std::complex<float> y;
            firdecim_crcf_execute(filter_.get(), pending_.data(), &y);
            out.push_back(y);
            pending_.clear();
        }
    }
}

UpFrom48k::UpFrom48k() {
    // Each output phase sums one tap in kResampleFactor, so the taps sum to
    // the factor for a unity gain.
    std::array<float, kTaps> taps = design(static_cast<float>(kResampleFactor));
    filter_.reset(firinterp_crcf_create(kResampleFactor, taps.data(), kTaps));
}

void UpFrom48k::process(std::span<const std::complex<float>> input,
                        std::vector<std::complex<float>>& out) {
    const std::size_t at = out.size();
    out.resize(at + input.size() * kResampleFactor);
    for (std::size_t i = 0; i < input.size(); ++i) {
        firinterp_crcf_execute(filter_.get(), input[i], out.data() + at + i * kResampleFactor);
    }
}

}  // namespace ft8
