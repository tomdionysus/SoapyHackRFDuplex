// SPDX-License-Identifier: GPL-3.0-or-later
//
// Between the pair's 1.92 Msps and the FT8 chain's 48 ksps, without a
// radio: tones through each direction, and an FT8 frame taken up to the
// pair's rate, as hackrfduplex-ft8 transmits it, and down again, as it
// receives it, still decoding.
#include "resampler.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <numbers>
#include <vector>

#include "decimator.hpp"
#include "ft8/codec.h"
#include "ft8/decoder.h"
#include "message_text.hpp"

using namespace ft8;

namespace {

std::vector<std::complex<float>> tone(float hz, float rate, std::size_t count) {
    std::vector<std::complex<float>> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double phase = 2 * std::numbers::pi * hz * static_cast<double>(i) / rate;
        out[i] = {static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
    }
    return out;
}

// Mean power in dB of the second half, after the filter has filled.
double level_db(const std::vector<std::complex<float>>& x) {
    double power = 0;
    for (std::size_t i = x.size() / 2; i < x.size(); ++i) {
        power += std::norm(x[i]);
    }
    return 10 * std::log10(power / static_cast<double>(x.size() - x.size() / 2));
}

// Power in dB at `hz` over the second half, by one DFT bin.
double bin_db(const std::vector<std::complex<float>>& x, float hz, float rate) {
    std::complex<double> sum{};
    const std::size_t from = x.size() / 2;
    for (std::size_t i = from; i < x.size(); ++i) {
        sum += std::complex<double>(x[i]) *
               std::polar(1.0, -2 * std::numbers::pi * hz * static_cast<double>(i) / rate);
    }
    return 10 * std::log10(std::norm(sum / static_cast<double>(x.size() - from)));
}

}  // namespace

TEST_CASE("1.92 Msps comes down to 48 ksps with FT8's band whole and aliases stopped",
          "[ft8][resample]") {
    const auto down = [](float hz) {
        DownTo48k d;
        std::vector<std::complex<float>> out;
        d.process(tone(hz, kPairRate, 192000), out);
        return level_db(out);
    };
    CHECK(down(1000) == Catch::Approx(0).margin(0.2));
    CHECK(down(-1500) == Catch::Approx(0).margin(0.2));
    // Each would alias to within FT8's band at 48 ksps.
    CHECK(down(48000 + 1000) < -60);
    CHECK(down(-96000 + 500) < -60);
}

TEST_CASE("48 ksps goes up to 1.92 Msps at unity gain with its images stopped", "[ft8][resample]") {
    UpFrom48k u;
    std::vector<std::complex<float>> out;
    u.process(tone(1000, kAudioRate, 4800), out);
    REQUIRE(out.size() == 4800 * kResampleFactor);
    CHECK(level_db(out) == Catch::Approx(0).margin(0.2));
    CHECK(bin_db(out, 1000, kPairRate) == Catch::Approx(0).margin(0.2));
    // The first image, where a 48 ksps tone at 1 kHz repeats.
    CHECK(bin_db(out, 48000 + 1000, kPairRate) < -60);
}

TEST_CASE("an FT8 frame sent at 1.92 Msps and received back decodes", "[ft8][resample]") {
    const auto payload = message_payload("CQ K1ABC FN42");
    REQUIRE(payload);
    const auto audio = synthesize(encode(*payload), 1000.0F, kAudioRate);
    // A 15 s slot at 48 ksps with the frame 0.5 s in, then up and down
    // again in pieces, as the transmitter and the receiver handle it.
    std::vector<std::complex<float>> slot(static_cast<std::size_t>(kSlotTime * kAudioRate));
    const auto start = static_cast<std::size_t>(kStartOffset * kAudioRate);
    for (std::size_t i = 0; i < audio.size() && start + i < slot.size(); ++i) {
        slot[start + i] = 0.5F * audio[i];
    }
    UpFrom48k up;
    DownTo48k down;
    Decimator decimator;
    std::vector<std::complex<float>> wide;
    std::vector<std::complex<float>> audio_back;
    std::vector<std::complex<float>> narrow;
    for (std::size_t i = 0; i < slot.size(); i += 4800) {
        wide.clear();
        audio_back.clear();
        up.process(std::span(slot).subspan(i, std::min<std::size_t>(4800, slot.size() - i)), wide);
        down.process(wide, audio_back);
        decimator.process(audio_back, narrow);
    }
    Decoder::Options o;
    o.sample_rate = Decimator::kOutputRate;
    Decoder decoder(o);
    decoder.process(narrow);
    const auto frames = decoder.decode();
    REQUIRE(frames.size() == 1);
    CHECK(message_text(frames[0].payload) == "CQ K1ABC FN42");
    CHECK(frames[0].freq_hz == Catch::Approx(1000.0F).margin(3.2F));
}
