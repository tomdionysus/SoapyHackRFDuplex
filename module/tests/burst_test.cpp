// SPDX-License-Identifier: GPL-3.0-or-later
#include "burst.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <random>
#include <vector>

#include "fft.hpp"

using namespace hackrfduplex;

namespace {

// I/Q int8 bytes of complex noise of standard deviation `sigma` per rail.
std::vector<std::int8_t> noise(std::size_t samples, double sigma, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> gauss(0.0, sigma);
    std::vector<std::int8_t> out(samples * 2);
    for (std::int8_t& v : out) {
        v = static_cast<std::int8_t>(std::clamp(std::lround(gauss(rng)), -127L, 127L));
    }
    return out;
}

// Adds the burst into `iq` at sample `at`, scaled and rotated by `phase`.
void add_burst(std::vector<std::int8_t>& iq, std::size_t at, double scale, double phase) {
    const std::vector<std::int8_t> chips = burst::chips();
    for (std::size_t k = 0; k < chips.size(); ++k) {
        const std::complex<double> s = std::polar(scale * chips[k], phase);
        for (std::size_t rail = 0; rail < 2; ++rail) {
            const double v = (rail == 0 ? s.real() : s.imag()) + iq[(at + k) * 2 + rail];
            iq[(at + k) * 2 + rail] =
                static_cast<std::int8_t>(std::clamp(std::lround(v), -127L, 127L));
        }
    }
}

}  // namespace

TEST_CASE("an FFT and its inverse round trip and find a tone", "[burst]") {
    std::vector<std::complex<float>> data(64);
    for (std::size_t n = 0; n < data.size(); ++n) {
        data[n] = std::polar(1.0f, 2.0f * 3.14159265f * 5.0f * static_cast<float>(n) / 64.0f);
    }
    const auto original = data;
    fft(data, false);
    for (std::size_t k = 0; k < data.size(); ++k) {
        if (k != 5) {
            CHECK(std::abs(data[k]) < 1e-3f);
        }
    }
    CHECK(std::abs(std::abs(data[5]) - 64.0f) < 1e-3f);
    fft(data, true);
    for (std::size_t n = 0; n < data.size(); ++n) {
        CHECK(std::abs(data[n] - original[n]) < 1e-4f);
    }
    std::vector<std::complex<float>> odd(48);
    CHECK_THROWS_AS(fft(odd, false), std::invalid_argument);
}

TEST_CASE("the burst is 4096 chips of a maximal-length sequence", "[burst]") {
    const std::vector<std::int8_t> chips = burst::chips();
    REQUIRE(chips.size() == burst::kChips);
    REQUIRE(burst::kChips == 4096);
    for (const std::int8_t c : chips) {
        CHECK((c == 1 || c == -1));
    }
    // Close to balanced, as a stretch of an m-sequence is.
    long sum = 0;
    for (const std::int8_t c : chips) {
        sum += c;
    }
    CHECK(std::abs(sum) < 200);

    // Its samples: the chips on I at 0.7 of full scale, Q zero.
    const std::vector<std::int8_t> samples = burst::samples();
    REQUIRE(samples.size() == 2 * burst::kChips);
    for (std::size_t k = 0; k < burst::kChips; ++k) {
        CHECK(samples[2 * k] == chips[k] * 89);
        CHECK(samples[2 * k + 1] == 0);
    }
}

TEST_CASE("the burst's autocorrelation peak stands 30 dB over every sidelobe", "[burst]") {
    // One chip per sample, so this holds at any sample rate.
    const std::vector<std::int8_t> chips = burst::chips();
    const long n = static_cast<long>(chips.size());
    long peak = 0;
    long worst = 0;
    for (long lag = 0; lag < n; ++lag) {
        long sum = 0;
        for (long k = 0; k + lag < n; ++k) {
            sum += chips[static_cast<std::size_t>(k)] * chips[static_cast<std::size_t>(k + lag)];
        }
        if (lag == 0) {
            peak = sum;
        } else {
            worst = std::max(worst, std::abs(sum));
        }
    }
    CHECK(peak == n);
    CHECK(20.0 * std::log10(static_cast<double>(peak) / static_cast<double>(worst)) >= 30.0);
}

TEST_CASE("the burst is found at its offset in noise", "[burst]") {
    const std::size_t length = 300'000;
    for (const std::size_t at :
         {std::size_t{0}, std::size_t{12'345}, std::size_t{12'289}, length - burst::kChips}) {
        std::vector<std::int8_t> iq = noise(length, 6.0, static_cast<std::uint32_t>(at + 1));
        add_burst(iq, at, 4.0, 1.1);  // 3.5 dB below the noise per sample
        const burst::Found found = burst::find(iq);
        CHECK(found.found);
        CHECK(found.offset == at);
        CHECK(found.peak_over_median_db >= 20.0);
    }
}

TEST_CASE("noise alone finds no burst", "[burst]") {
    const std::vector<std::int8_t> iq = noise(300'000, 20.0, 7);
    const burst::Found found = burst::find(iq);
    CHECK_FALSE(found.found);
    CHECK(found.peak_over_median_db < 20.0);
}

TEST_CASE("a capture shorter than the burst finds nothing", "[burst]") {
    const std::vector<std::int8_t> iq(2 * (burst::kChips - 1), 0);
    CHECK_FALSE(burst::find(iq).found);
}
