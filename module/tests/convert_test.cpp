// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace hackrfduplex;
using Catch::Matchers::WithinAbs;

TEST_CASE("CS8 passes through unchanged", "[convert]") {
    const std::array<std::int8_t, 6> samples = {-128, 127, 0, 1, -1, 64};
    std::array<std::int8_t, 6> native{};
    to_native(samples.data(), Format::kCs8, native.data(), 3);
    CHECK(native == samples);
    std::array<std::int8_t, 6> back{};
    from_native(native.data(), Format::kCs8, back.data(), 3);
    CHECK(back == samples);
}

TEST_CASE("CS16 keeps its top byte each way", "[convert]") {
    const std::array<std::int16_t, 4> samples = {0x7F00, -32768, 0x0100, 0x00FF};
    std::array<std::int8_t, 4> native{};
    to_native(samples.data(), Format::kCs16, native.data(), 2);
    CHECK(native == std::array<std::int8_t, 4>{127, -128, 1, 0});

    const std::array<std::int8_t, 4> raw = {127, -128, 1, -1};
    std::array<std::int16_t, 4> wide{};
    from_native(raw.data(), Format::kCs16, wide.data(), 2);
    CHECK(wide == std::array<std::int16_t, 4>{32512, -32768, 256, -256});
}

TEST_CASE("CF32 scales by 128, rounds and clamps", "[convert]") {
    const std::array<float, 8> samples = {1.0f,          -1.0f,          0.5f, 0.3f / 128.0f,
                                          0.7f / 128.0f, -0.3f / 128.0f, 2.0f, -2.0f};
    std::array<std::int8_t, 8> native{};
    to_native(samples.data(), Format::kCf32, native.data(), 4);
    CHECK(native == std::array<std::int8_t, 8>{127, -128, 64, 0, 1, 0, 127, -128});

    const std::array<std::int8_t, 4> raw = {-128, 0, 64, 127};
    std::array<float, 4> floats{};
    from_native(raw.data(), Format::kCf32, floats.data(), 2);
    CHECK_THAT(floats[0], WithinAbs(-1.0, 1e-7));
    CHECK_THAT(floats[1], WithinAbs(0.0, 1e-7));
    CHECK_THAT(floats[2], WithinAbs(0.5, 1e-7));
    CHECK_THAT(floats[3], WithinAbs(127.0 / 128.0, 1e-7));
}

TEST_CASE("CF64 converts like CF32", "[convert]") {
    const std::array<double, 4> samples = {1.0, -0.25, 0.75, -3.0};
    std::array<std::int8_t, 4> native{};
    to_native(samples.data(), Format::kCf64, native.data(), 2);
    CHECK(native == std::array<std::int8_t, 4>{127, -32, 96, -128});

    std::array<double, 4> back{};
    from_native(native.data(), Format::kCf64, back.data(), 2);
    CHECK_THAT(back[0], WithinAbs(127.0 / 128.0, 1e-12));
    CHECK_THAT(back[1], WithinAbs(-0.25, 1e-12));
    CHECK_THAT(back[2], WithinAbs(0.75, 1e-12));
    CHECK_THAT(back[3], WithinAbs(-1.0, 1e-12));
}

namespace {

// The rule both float formats follow: scale by 128, round half to even,
// clamp to the int8 range.
template <typename Float>
std::int8_t expected_native(Float value) {
    const double scaled = std::nearbyint(static_cast<double>(value) * 128.0);
    return static_cast<std::int8_t>(std::clamp(scaled, -128.0, 127.0));
}

template <typename Float>
void check_against_rule(Format format) {
    std::vector<Float> values;
    // Every tie between two codes, and a dense grid across and beyond full
    // scale, then far out of range.
    for (int k = -260; k <= 260; ++k) {
        values.push_back(static_cast<Float>((k + 0.5) / 128.0));
    }
    for (int i = -3000; i <= 3000; ++i) {
        values.push_back(static_cast<Float>(i / 1024.0));
    }
    for (const Float far :
         {static_cast<Float>(1e6), static_cast<Float>(-1e6), std::numeric_limits<Float>::max(),
          std::numeric_limits<Float>::lowest(), std::numeric_limits<Float>::infinity(),
          -std::numeric_limits<Float>::infinity()}) {
        values.push_back(far);
    }
    if (values.size() % 2 != 0) {
        values.push_back(0);
    }
    std::vector<std::int8_t> native(values.size());
    to_native(values.data(), format, native.data(), values.size() / 2);
    for (std::size_t i = 0; i < values.size(); ++i) {
        CAPTURE(i, static_cast<double>(values[i]));
        CHECK(static_cast<int>(native[i]) == static_cast<int>(expected_native(values[i])));
    }
}

}  // namespace

TEST_CASE("float formats round half to even and saturate, everywhere", "[convert]") {
    check_against_rule<float>(Format::kCf32);
    check_against_rule<double>(Format::kCf64);
}

TEST_CASE("format names round trip and sizes are per complex sample", "[convert]") {
    for (const Format format : {Format::kCs8, Format::kCs16, Format::kCf32, Format::kCf64}) {
        CHECK(parse_format(format_name(format)) == format);
    }
    CHECK(sample_bytes(Format::kCs8) == 2);
    CHECK(sample_bytes(Format::kCs16) == 4);
    CHECK(sample_bytes(Format::kCf32) == 8);
    CHECK(sample_bytes(Format::kCf64) == 16);
    CHECK_THROWS_AS(parse_format("CU8"), std::invalid_argument);
}
