// SPDX-License-Identifier: GPL-3.0-or-later
#include "rx_correction.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
// liquid.h defines its complex types as std::complex in C++.
#include <liquid/liquid.h>

namespace hackrfduplex {

namespace {

using Sample = std::complex<float>;

// The DC blocker's pole sits 1e-4 inside the unit circle: it follows the
// offset over about 10 000 samples (1.25 ms at 8 Msps) and passes
// anything a few hundred hertz from centre untouched.
constexpr float kDcAlpha = 1e-4F;
// The I/Q estimate's memory, in samples: each block moves it by its share
// of this, so a block of 16 384 corrects a fifth of the error it sees.
constexpr double kIqMemory = 65'536;

iirfilt_crcf blocker(void* handle) { return static_cast<iirfilt_crcf>(handle); }

// Back to an integer format: rounded, saturated.
template <typename Int>
Int to_int(float value, float scale) {
    const float scaled = std::nearbyint(value * scale);
    const auto low = static_cast<float>(std::numeric_limits<Int>::min());
    const auto high = static_cast<float>(std::numeric_limits<Int>::max());
    return static_cast<Int>(std::clamp(scaled, low, high));
}

}  // namespace

RxCorrection::RxCorrection() : dc_blocker_(iirfilt_crcf_create_dc_blocker(kDcAlpha)) {}

RxCorrection::~RxCorrection() { iirfilt_crcf_destroy(blocker(dc_blocker_)); }

void RxCorrection::set_dc_automatic(bool on) {
    const Lock lock(mutex_);
    if (on && !dc_automatic_) {
        iirfilt_crcf_reset(blocker(dc_blocker_));
    }
    dc_automatic_ = on;
}

bool RxCorrection::dc_automatic() const {
    const Lock lock(mutex_);
    return dc_automatic_;
}

void RxCorrection::set_dc_offset(std::complex<double> offset) {
    const Lock lock(mutex_);
    dc_offset_ = offset;
}

std::complex<double> RxCorrection::dc_offset() const {
    const Lock lock(mutex_);
    return dc_offset_;
}

void RxCorrection::set_iq_automatic(bool on) {
    const Lock lock(mutex_);
    iq_automatic_ = on;
}

bool RxCorrection::iq_automatic() const {
    const Lock lock(mutex_);
    return iq_automatic_;
}

void RxCorrection::set_iq_balance(std::complex<double> balance) {
    const Lock lock(mutex_);
    iq_balance_ = balance;
}

std::complex<double> RxCorrection::iq_balance() const {
    const Lock lock(mutex_);
    return iq_balance_;
}

bool RxCorrection::active() const {
    const Lock lock(mutex_);
    return active_locked();
}

bool RxCorrection::active_locked() const {
    return dc_automatic_ || dc_offset_ != std::complex<double>{} || iq_automatic_ ||
           iq_balance_ != std::complex<double>{};
}

void RxCorrection::reset() {
    const Lock lock(mutex_);
    iirfilt_crcf_reset(blocker(dc_blocker_));
}

void RxCorrection::apply(void* samples, Format format, std::size_t count) {
    const Lock lock(mutex_);
    if (count == 0 || !active_locked()) {
        return;
    }
    if (format == Format::kCf32) {
        correct(static_cast<Sample*>(samples), count);
        return;
    }
    scratch_.resize(count);
    switch (format) {
        case Format::kCs8: {
            auto* raw = static_cast<std::int8_t*>(samples);
            for (std::size_t i = 0; i < count; ++i) {
                scratch_[i] = Sample(raw[2 * i], raw[2 * i + 1]) / 128.0F;
            }
            correct(scratch_.data(), count);
            for (std::size_t i = 0; i < count; ++i) {
                raw[2 * i] = to_int<std::int8_t>(scratch_[i].real(), 128.0F);
                raw[2 * i + 1] = to_int<std::int8_t>(scratch_[i].imag(), 128.0F);
            }
            return;
        }
        case Format::kCs16: {
            auto* raw = static_cast<std::int16_t*>(samples);
            for (std::size_t i = 0; i < count; ++i) {
                scratch_[i] = Sample(raw[2 * i], raw[2 * i + 1]) / 32768.0F;
            }
            correct(scratch_.data(), count);
            for (std::size_t i = 0; i < count; ++i) {
                raw[2 * i] = to_int<std::int16_t>(scratch_[i].real(), 32768.0F);
                raw[2 * i + 1] = to_int<std::int16_t>(scratch_[i].imag(), 32768.0F);
            }
            return;
        }
        case Format::kCf64: {
            auto* raw = static_cast<std::complex<double>*>(samples);
            for (std::size_t i = 0; i < count; ++i) {
                scratch_[i] = Sample(raw[i]);
            }
            correct(scratch_.data(), count);
            for (std::size_t i = 0; i < count; ++i) {
                raw[i] = std::complex<double>(scratch_[i]);
            }
            return;
        }
        case Format::kCf32:
            return;
    }
}

void RxCorrection::correct(Sample* samples, std::size_t count) {
    if (dc_automatic_) {
        iirfilt_crcf_execute_block(blocker(dc_blocker_), samples, static_cast<unsigned>(count),
                                   samples);
    } else if (dc_offset_ != std::complex<double>{}) {
        const Sample offset(dc_offset_);
        for (std::size_t i = 0; i < count; ++i) {
            samples[i] -= offset;
        }
    }
    if (!iq_automatic_ && iq_balance_ == std::complex<double>{}) {
        return;
    }
    const Sample b(iq_balance_);
    std::complex<double> square{};
    double power = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const Sample y = samples[i] + b * std::conj(samples[i]);
        samples[i] = y;
        square += std::complex<double>(y * y);
        power += std::norm(y);
    }
    if (iq_automatic_ && power > 0) {
        // For y = (1 + b conj(k)) s + (k + b) conj(s), with s the balanced
        // signal and k the receiver's image, E[y^2] / E[|y|^2] is close to
        // 2 (k + b): half of it is how far b is from -k.
        const double share = static_cast<double>(count) / (static_cast<double>(count) + kIqMemory);
        iq_balance_ -= share * 0.5 * square / power;
    }
}

}  // namespace hackrfduplex
