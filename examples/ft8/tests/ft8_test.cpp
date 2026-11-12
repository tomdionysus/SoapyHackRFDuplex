// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
//
// The receive chain without a radio: messages encoded, synthesised at the
// radio's 48 ksps with noise, decimated and decoded as ft8rx does.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "decimator.hpp"
#include "ft8/codec.h"
#include "ft8/decoder.h"
#include "message_text.hpp"

using namespace ft8;

namespace {

constexpr float kRate = Decimator::kInputRate;

// A 15 s slot at 48 ksps: each message starts 0.5 s in plus its `dt`, at
// `audio_hz` above the dial (the centre is dial + 1500 Hz), in complex
// Gaussian noise of `noise` per component.
std::vector<std::complex<float>> slot(const std::vector<std::pair<std::string, float>>& messages,
                                      float dt, float noise) {
    std::vector<std::complex<float>> samples(static_cast<std::size_t>(kSlotTime * kRate));
    std::mt19937 rng(7);
    std::normal_distribution<float> gauss(0.0f, noise);
    for (auto& s : samples) {
        s = {gauss(rng), gauss(rng)};
    }
    for (const auto& [text, audio_hz] : messages) {
        const auto payload = message_payload(text);
        REQUIRE(payload);
        const auto signal = synthesize(encode(*payload), audio_hz - 1500.0f, kRate);
        const auto start = static_cast<std::size_t>((kStartOffset + dt) * kRate);
        for (std::size_t i = 0; i < signal.size() && start + i < samples.size(); ++i) {
            samples[start + i] += signal[i];
        }
    }
    return samples;
}

std::vector<Decoder::Frame> receive(const std::vector<std::complex<float>>& wide) {
    Decimator decimator;
    std::vector<std::complex<float>> narrow;
    // In packet-sized pieces, as ft8rx feeds it.
    for (std::size_t i = 0; i < wide.size(); i += 126) {
        const std::size_t n = std::min<std::size_t>(126, wide.size() - i);
        decimator.process(std::span(wide).subspan(i, n), narrow);
    }
    Decoder::Options o;
    o.sample_rate = Decimator::kOutputRate;
    Decoder decoder(o);
    decoder.process(narrow);
    return decoder.decode();
}

}  // namespace

TEST_CASE("messages round-trip through ft8_lib's text code", "[ft8]") {
    for (const char* text : {"CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC RR73"}) {
        const auto payload = message_payload(text);
        REQUIRE(payload);
        CHECK(message_text(*payload) == text);
    }
    CHECK_FALSE(message_payload("this is far too long to be any FT8 message"));
    CHECK_FALSE(message_text(Payload{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}));
}

TEST_CASE("the decimator passes FT8's band and stops the rest", "[ft8]") {
    const auto level = [](float hz) {
        std::vector<std::complex<float>> tone(48000);
        for (std::size_t i = 0; i < tone.size(); ++i) {
            const float phase = 2 * std::numbers::pi_v<float> * hz * static_cast<float>(i) / kRate;
            tone[i] = {std::cos(phase), std::sin(phase)};
        }
        Decimator d;
        std::vector<std::complex<float>> out;
        d.process(tone, out);
        double power = 0;
        for (std::size_t i = out.size() / 2; i < out.size(); ++i) {
            power += std::norm(out[i]);
        }
        return 10 * std::log10(power / static_cast<double>(out.size() - out.size() / 2));
    };
    CHECK(level(0) == Catch::Approx(0).margin(0.5));
    CHECK(level(1500) == Catch::Approx(0).margin(0.5));
    CHECK(level(-1500) == Catch::Approx(0).margin(0.5));
    CHECK(level(2000) < -60);  // would alias into the band
    CHECK(level(-5000) < -60);
}

TEST_CASE("FT8 at 48 ksps decodes through the decimator", "[ft8]") {
    // -10 dB SNR in 2500 Hz: tone power 1, noise 10 in 2500 Hz of 48 kHz.
    const float noise = std::sqrt(10.0f * kRate / 2500.0f / 2.0f);
    const auto frames = receive(slot({{"CQ K1ABC FN42", 1234.0f}}, 0.3f, noise));
    REQUIRE(frames.size() == 1);
    CHECK(message_text(frames[0].payload) == "CQ K1ABC FN42");
    CHECK(frames[0].freq_hz + 1500.0f == Catch::Approx(1234.0f).margin(3.2f));
    CHECK(frames[0].time_s - kStartOffset == Catch::Approx(0.3f).margin(0.1f));
    CHECK(frames[0].snr_db == Catch::Approx(-10.0f).margin(3.0f));
}

TEST_CASE("several messages across the band decode in one slot", "[ft8]") {
    const auto frames = receive(slot(
        {{"CQ K1ABC FN42", 300.0f}, {"K1ABC W9XYZ -11", 1500.0f}, {"W9XYZ K1ABC RR73", 2700.0f}},
        0.0f, 1.0f));
    std::vector<std::string> texts;
    for (const auto& f : frames) {
        texts.push_back(message_text(f.payload).value_or("?"));
    }
    std::sort(texts.begin(), texts.end());
    CHECK(texts ==
          std::vector<std::string>{"CQ K1ABC FN42", "K1ABC W9XYZ -11", "W9XYZ K1ABC RR73"});
}
