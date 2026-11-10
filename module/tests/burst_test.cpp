// SPDX-License-Identifier: GPL-3.0-or-later
#include "burst.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <complex>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "fft.hpp"

using namespace hackrfduplex;

namespace {

// What a receiver quantises: complex noise of standard deviation `sigma`
// per rail, a DC offset, and optionally the burst at sample `at`, scaled
// against its own full scale and rotated by `phase`. Summed in double and
// rounded once, so a burst far below one step still shows through the
// noise as it does on a real receiver.
struct Air {
    double sigma = 1.0;
    std::complex<double> dc{0, 0};
    std::optional<std::size_t> at;
    double scale = 0;
    double phase = 0;
    std::uint32_t seed = 1;
};

std::vector<std::int8_t> receive(std::size_t samples, const Air& air) {
    std::mt19937 rng(air.seed);
    std::normal_distribution<double> gauss(0.0, air.sigma);
    std::vector<std::complex<double>> x(samples);
    for (std::complex<double>& v : x) {
        v = air.dc + std::complex<double>(gauss(rng), gauss(rng));
    }
    if (air.at) {
        const std::vector<std::int8_t> burst = burst::samples();
        const std::complex<double> turn = std::polar(air.scale, air.phase);
        for (std::size_t k = 0; k < burst::kChips; ++k) {
            x[*air.at + k] += turn * std::complex<double>(burst[2 * k], burst[2 * k + 1]);
        }
    }
    std::vector<std::int8_t> out(2 * samples);
    for (std::size_t k = 0; k < samples; ++k) {
        out[2 * k] = static_cast<std::int8_t>(std::clamp(std::lround(x[k].real()), -127L, 127L));
        out[2 * k + 1] =
            static_cast<std::int8_t>(std::clamp(std::lround(x[k].imag()), -127L, 127L));
    }
    return out;
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

    // Its samples: each chip at full scale on both I and Q, the most
    // energy the converter can put into 4096 samples.
    const std::vector<std::int8_t> samples = burst::samples();
    REQUIRE(samples.size() == 2 * burst::kChips);
    for (std::size_t k = 0; k < burst::kChips; ++k) {
        CHECK(samples[2 * k] == chips[k] * 127);
        CHECK(samples[2 * k + 1] == chips[k] * 127);
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
        // 12 dB below the noise per sample.
        const std::vector<std::int8_t> iq =
            receive(length, Air{6.0, {}, at, 0.0119, 1.1, static_cast<std::uint32_t>(at + 1)});
        const burst::Found found = burst::find(iq);
        CHECK(found.found);
        CHECK(found.offset == at);
        CHECK(found.peak_over_median_db >= 20.0);
    }
}

TEST_CASE("the burst is found under a DC offset many times its size", "[burst]") {
    // A HackRF receiver's own DC offset dwarfs a weak burst; uncorrected, it
    // correlates with the chips at every lag and buries the peak.
    // As on the bench: the burst a fifth of a step, noise under one step
    // per rail, DC two to three steps.
    const std::vector<std::int8_t> iq =
        receive(300'000, Air{0.64, {3.0, -2.0}, 100'000, 0.0016, 0.4, 11});
    const burst::Found found = burst::find(iq);
    CHECK(found.found);
    CHECK(found.offset == 100'000);
}

TEST_CASE("noise alone finds no burst", "[burst]") {
    const std::vector<std::int8_t> iq = receive(300'000, Air{20.0, {}, {}, 0, 0, 7});
    const burst::Found found = burst::find(iq);
    CHECK_FALSE(found.found);
    CHECK(found.peak_over_median_db < 20.0);
}

TEST_CASE("a capture shorter than the burst finds nothing", "[burst]") {
    const std::vector<std::int8_t> iq(2 * (burst::kChips - 1), 0);
    CHECK_FALSE(burst::find(iq).found);
}

namespace {

// Feeds `iq` to a finder in pieces of `piece` samples, as a capture
// fills; returns how many samples it had when it settled, if it did.
std::optional<std::size_t> feed(burst::Finder& finder, const std::vector<std::int8_t>& iq,
                                std::size_t piece) {
    const std::size_t total = iq.size() / 2;
    for (std::size_t have = piece; have < total + piece; have += piece) {
        const std::size_t n = std::min(have, total);
        finder.advance(std::span<const std::int8_t>(iq.data(), 2 * n));
        if (finder.settled()) {
            return n;
        }
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("the finder settles on the burst before the capture ends, where find does", "[burst]") {
    const std::size_t length = 400'000;
    for (const std::size_t at : {std::size_t{30'000}, std::size_t{131'072}, std::size_t{200'001}}) {
        CAPTURE(at);
        const std::vector<std::int8_t> iq =
            receive(length, Air{6.0, {}, at, 0.0119, 1.1, static_cast<std::uint32_t>(at + 3)});
        burst::Finder finder;
        const std::optional<std::size_t> settled_at = feed(finder, iq, 8192);
        REQUIRE(settled_at);
        // A burst length of lags past the peak, and at most one FFT block more.
        CHECK(*settled_at >= at + 2 * burst::kChips);
        CHECK(*settled_at < at + 2 * burst::kChips + 16384 + 8192);
        const burst::Found early = finder.result();
        const burst::Found whole = burst::find(iq);
        CHECK(early.found);
        CHECK(early.offset == at);
        CHECK(whole.offset == at);
    }
}

TEST_CASE("the finder settles under a DC offset many times the burst", "[burst]") {
    const std::vector<std::int8_t> iq =
        receive(300'000, Air{0.64, {3.0, -2.0}, 100'000, 0.0016, 0.4, 11});
    burst::Finder finder;
    REQUIRE(feed(finder, iq, 131'072 / 8));
    CHECK(finder.result().offset == 100'000);
}

TEST_CASE("the finder never settles on noise, and finishing reports what find does", "[burst]") {
    const std::vector<std::int8_t> iq = receive(300'000, Air{20.0, {}, {}, 0, 0, 7});
    burst::Finder finder;
    CHECK_FALSE(feed(finder, iq, 4096));
    finder.finish(iq);
    CHECK_FALSE(finder.settled());
    CHECK_FALSE(finder.result().found);
    // The blocks correlated early took the DC of the capture so far.
    CHECK(std::abs(finder.result().peak_over_median_db - burst::find(iq).peak_over_median_db) <
          0.5);
}

TEST_CASE("a burst at the very end is found only when the finder finishes", "[burst]") {
    const std::size_t length = 100'000;
    const std::vector<std::int8_t> iq =
        receive(length, Air{6.0, {}, length - burst::kChips, 0.0119, 0.3, 5});
    burst::Finder finder;
    CHECK_FALSE(feed(finder, iq, 10'000));
    finder.finish(iq);
    CHECK(finder.result().found);
    CHECK(finder.result().offset == length - burst::kChips);
}
