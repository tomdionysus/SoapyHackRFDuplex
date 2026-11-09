// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hackrfduplex {

namespace {

template <typename Float>
std::int8_t quantise(Float value) {
    const Float scaled = std::nearbyint(value * static_cast<Float>(kFullScale));
    const Float clamped = std::clamp(scaled, static_cast<Float>(-128), static_cast<Float>(127));
    return static_cast<std::int8_t>(clamped);
}

template <typename Float>
void floats_to_native(const Float* src, std::int8_t* dst, std::size_t samples) {
    for (std::size_t i = 0; i < samples * 2; ++i) {
        dst[i] = quantise(src[i]);
    }
}

template <typename Float>
void native_to_floats(const std::int8_t* src, Float* dst, std::size_t samples) {
    constexpr Float scale = static_cast<Float>(1) / static_cast<Float>(kFullScale);
    for (std::size_t i = 0; i < samples * 2; ++i) {
        dst[i] = static_cast<Float>(src[i]) * scale;
    }
}

}  // namespace

void to_native(const void* src, Format format, std::int8_t* dst, std::size_t samples) {
    switch (format) {
        case Format::kCs8:
            std::memcpy(dst, src, samples * kNativeSampleBytes);
            return;
        case Format::kCs16: {
            const auto* in = static_cast<const std::int16_t*>(src);
            for (std::size_t i = 0; i < samples * 2; ++i) {
                dst[i] = static_cast<std::int8_t>(in[i] >> 8);
            }
            return;
        }
        case Format::kCf32:
            floats_to_native(static_cast<const float*>(src), dst, samples);
            return;
        case Format::kCf64:
            floats_to_native(static_cast<const double*>(src), dst, samples);
            return;
    }
}

void from_native(const std::int8_t* src, Format format, void* dst, std::size_t samples) {
    switch (format) {
        case Format::kCs8:
            std::memcpy(dst, src, samples * kNativeSampleBytes);
            return;
        case Format::kCs16: {
            auto* out = static_cast<std::int16_t*>(dst);
            for (std::size_t i = 0; i < samples * 2; ++i) {
                out[i] = static_cast<std::int16_t>(src[i] * 256);
            }
            return;
        }
        case Format::kCf32:
            native_to_floats(src, static_cast<float*>(dst), samples);
            return;
        case Format::kCf64:
            native_to_floats(src, static_cast<double*>(dst), samples);
            return;
    }
}

}  // namespace hackrfduplex
