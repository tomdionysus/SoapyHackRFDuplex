// SPDX-License-Identifier: GPL-3.0-or-later
#include "rx_correction.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <vector>

using namespace hackrfduplex;
using Catch::Matchers::WithinAbs;

namespace {

using Sample = std::complex<float>;

// A tone `cycles_per_sample` from centre at `level`, its image at
// `image` (x + image conj(x), what an imbalanced receiver makes of it) and
// a DC offset.
std::vector<Sample> tone(std::size_t count, double cycles_per_sample, double level,
                         std::complex<double> image = {}, std::complex<double> dc = {}) {
    std::vector<Sample> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        const std::complex<double> x =
            level *
            std::polar(1.0, 2.0 * std::numbers::pi * cycles_per_sample * static_cast<double>(n));
        out[n] = Sample(x + image * std::conj(x) + dc);
    }
    return out;
}

// Power at `cycles_per_sample` over the samples from `from`, by a single
// DFT bin.
double power_at(const std::vector<Sample>& x, double cycles_per_sample, std::size_t from = 0) {
    std::complex<double> sum{};
    for (std::size_t n = from; n < x.size(); ++n) {
        sum +=
            std::complex<double>(x[n]) *
            std::polar(1.0, -2.0 * std::numbers::pi * cycles_per_sample * static_cast<double>(n));
    }
    const double len = static_cast<double>(x.size() - from);
    return std::norm(sum / len);
}

double db(double ratio) { return 10.0 * std::log10(ratio); }

std::complex<double> mean(const std::vector<Sample>& x, std::size_t from = 0) {
    std::complex<double> sum{};
    for (std::size_t n = from; n < x.size(); ++n) {
        sum += std::complex<double>(x[n]);
    }
    return sum / static_cast<double>(x.size() - from);
}

}  // namespace

TEST_CASE("with nothing set the samples pass untouched", "[dsp]") {
    RxCorrection correction;
    CHECK_FALSE(correction.active());
    CHECK_FALSE(correction.dc_automatic());
    CHECK_FALSE(correction.iq_automatic());
    CHECK(correction.dc_offset() == std::complex<double>{});
    CHECK(correction.iq_balance() == std::complex<double>{});
    std::vector<Sample> x = tone(1000, 0.01, 0.5, {0.05, 0.0}, {0.1, -0.1});
    const std::vector<Sample> before = x;
    correction.apply(x.data(), Format::kCf32, x.size());
    CHECK(x == before);
}

TEST_CASE("a fixed DC offset is subtracted", "[dsp]") {
    RxCorrection correction;
    correction.set_dc_offset({0.1, -0.05});
    CHECK(correction.active());
    CHECK(correction.dc_offset() == std::complex<double>{0.1, -0.05});
    std::vector<Sample> x = tone(4000, 0.01, 0.5, {}, {0.1, -0.05});
    correction.apply(x.data(), Format::kCf32, x.size());
    const std::complex<double> m = mean(x);
    CHECK_THAT(m.real(), WithinAbs(0.0, 1e-5));
    CHECK_THAT(m.imag(), WithinAbs(0.0, 1e-5));
}

TEST_CASE("automatic DC removal takes out an offset the application did not give", "[dsp]") {
    RxCorrection correction;
    correction.set_dc_automatic(true);
    CHECK(correction.dc_automatic());
    CHECK(correction.active());
    std::vector<Sample> x = tone(400'000, 0.01, 0.5, {}, {0.2, 0.1});
    // In blocks, as reads deliver them.
    for (std::size_t at = 0; at < x.size(); at += 8192) {
        correction.apply(x.data() + at, Format::kCf32, std::min<std::size_t>(8192, x.size() - at));
    }
    // Settled after the first quarter: the offset is gone, the tone kept.
    const std::size_t from = x.size() / 4;
    CHECK(std::abs(mean(x, from)) < 0.002);
    CHECK_THAT(power_at(x, 0.01, from), WithinAbs(0.25, 0.01));
}

TEST_CASE("a set I/Q balance cancels the image it describes", "[dsp]") {
    RxCorrection correction;
    const std::complex<double> image{0.05, 0.02};
    // y = x + b conj(x) with b = -image removes it to first order.
    correction.set_iq_balance(-image);
    CHECK(correction.iq_balance() == -image);
    std::vector<Sample> x = tone(8192, 0.0625, 0.5, image);
    const double image_before = power_at(x, -0.0625);
    correction.apply(x.data(), Format::kCf32, x.size());
    const double tone_after = power_at(x, 0.0625);
    const double image_after = power_at(x, -0.0625);
    CHECK(db(tone_after / image_before) < 30);  // it was there
    CHECK(db(tone_after / image_after) > 50);   // and is gone
}

TEST_CASE("automatic I/Q balance finds and removes an image", "[dsp]") {
    RxCorrection correction;
    correction.set_iq_automatic(true);
    CHECK(correction.iq_automatic());
    const std::complex<double> image{0.04, -0.03};  // about 26 dB down
    std::vector<Sample> x = tone(1 << 20, 0.0625, 0.5, image);
    for (std::size_t at = 0; at < x.size(); at += 16384) {
        correction.apply(x.data() + at, Format::kCf32, 16384);
    }
    const std::size_t from = x.size() / 2;
    CHECK(db(power_at(x, 0.0625, from) / power_at(x, -0.0625, from)) > 50);
    // The estimate is the correction that does it.
    const std::complex<double> b = correction.iq_balance();
    CHECK_THAT(b.real(), WithinAbs(-image.real(), 0.002));
    CHECK_THAT(b.imag(), WithinAbs(-image.imag(), 0.002));
}

TEST_CASE("automatic I/Q balance is not led astray by a DC offset", "[dsp]") {
    // On the target the receiver's DC was stronger than the tone, and an
    // offset d alone adds d^2 to E[y^2]: the estimate must be of the signal
    // about its mean, with or without DC removal on.
    RxCorrection correction;
    correction.set_iq_automatic(true);
    const std::complex<double> image{0.02, 0.01};
    std::vector<Sample> x = tone(1 << 20, 0.0625, 0.05, image, {0.12, -0.08});
    for (std::size_t at = 0; at < x.size(); at += 16384) {
        correction.apply(x.data() + at, Format::kCf32, 16384);
    }
    const std::complex<double> b = correction.iq_balance();
    CHECK_THAT(b.real(), WithinAbs(-image.real(), 0.002));
    CHECK_THAT(b.imag(), WithinAbs(-image.imag(), 0.002));
    const std::size_t from = x.size() / 2;
    CHECK(db(power_at(x, 0.0625, from) / power_at(x, -0.0625, from)) > 45);
}

TEST_CASE("the integer and double formats are corrected at their own scale", "[dsp]") {
    RxCorrection correction;
    correction.set_dc_offset({0.25, -0.25});
    std::array<std::int8_t, 4> cs8 = {64, -64, -128, 127};
    correction.apply(cs8.data(), Format::kCs8, 2);
    // The second sample, past full scale once corrected, saturates.
    CHECK(cs8 == std::array<std::int8_t, 4>{32, -32, -128, 127});
    std::array<std::int16_t, 4> cs16 = {16384, 0, -32768, 32767};
    correction.apply(cs16.data(), Format::kCs16, 2);
    CHECK(cs16 == std::array<std::int16_t, 4>{8192, 8192, -32768, 32767});
    std::array<double, 2> cf64 = {0.5, 0.5};
    correction.apply(cf64.data(), Format::kCf64, 1);
    CHECK_THAT(cf64[0], WithinAbs(0.25, 1e-6));
    CHECK_THAT(cf64[1], WithinAbs(0.75, 1e-6));
}

TEST_CASE("turning the corrections off restores the samples as received", "[dsp]") {
    RxCorrection correction;
    correction.set_dc_automatic(true);
    correction.set_iq_automatic(true);
    correction.set_dc_automatic(false);
    correction.set_iq_automatic(false);
    correction.set_iq_balance({});
    CHECK_FALSE(correction.active());
}
