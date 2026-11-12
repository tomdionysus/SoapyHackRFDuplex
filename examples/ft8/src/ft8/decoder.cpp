// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
//
// Copyright (C) 2026 Tom Cully. Licensed under
// GPL-2.0-or-later (here GPL-3.0-or-later). Ported from kgoba/ft8_lib (MIT, Copyright (c) 2018
// Karlis Goba); see src/ft8_lib/LICENSE.
#include "decoder.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace ft8 {

namespace {

constexpr int kTimeOsr = 2;
constexpr int kFreqOsr = 2;

// The analysis frame is two symbols long and is filed under the block in which it ends,
// so a candidate's block index runs this far behind the signal. Measured: 0.12 to 0.19 s
// over start offsets from 0 to 2 s, against a block resolution of 0.08 s.
constexpr float kAnalysisDelay = 0.15f;

float magnitude_db(std::uint8_t value) { return static_cast<float>(value) * 0.5f - 120.0f; }

float max4(float a, float b, float c, float d) { return std::max(std::max(a, b), std::max(c, d)); }

int floor_div(float value) { return static_cast<int>(std::floor(value)); }

}  // namespace

bool Decoder::valid_rate(float sample_rate) {
    const float block = sample_rate * kSymbolPeriod;
    if (block < 16 || std::fabs(block - std::round(block)) > 1e-3f) return false;
    // A power of two, as the analysis assumes.
    const auto size = static_cast<std::size_t>(std::lround(block)) * kFreqOsr;
    return (size & (size - 1)) == 0;
}

Decoder::Decoder(Options options) : _options(options) {
    if (!valid_rate(_options.sample_rate)) _options.sample_rate = 3200;

    _block_size = static_cast<int>(std::lround(_options.sample_rate * kSymbolPeriod));
    _subblock = _block_size / kTimeOsr;
    _nfft = _block_size * kFreqOsr;
    _fft_in.assign(static_cast<std::size_t>(_nfft), {0, 0});
    _fft_out.assign(static_cast<std::size_t>(_nfft), {0, 0});
    _fft.reset(fft_create_plan(static_cast<unsigned>(_nfft), _fft_in.data(), _fft_out.data(),
                               LIQUID_FFT_FORWARD, 0));

    // The range in whole tones, clamped to what the sample rate can see.
    const float nyquist = _options.sample_rate / 2;
    const float f_min = std::max(_options.f_min_hz, -nyquist);
    const float f_max = std::min(_options.f_max_hz, nyquist - kTones * kToneSpacing);
    _min_bin = floor_div(f_min * kSymbolPeriod);
    _num_bins = std::max(kTones, floor_div(f_max * kSymbolPeriod) + 1 - _min_bin);

    _max_blocks = static_cast<int>(kSlotTime / kSymbolPeriod);
    _block_stride = kTimeOsr * kFreqOsr * _num_bins;

    // Hann, scaled so a full-scale tone on a bin reads 0 dB.
    _window.resize(_nfft);
    for (int i = 0; i < _nfft; ++i) {
        const float s =
            std::sin(std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(_nfft));
        _window[i] = (2.0f / static_cast<float>(_nfft)) * s * s;
    }

    reset();
}

void Decoder::reset() {
    _frame.assign(_nfft, {0, 0});
    _pending.clear();
    _time_sub = 0;
    _blocks = 0;
    _mag.assign(static_cast<std::size_t>(_max_blocks) * _block_stride, 0);
}

void Decoder::process(std::span<const std::complex<float>> samples) {
    for (const auto& sample : samples) {
        if (_blocks >= _max_blocks) return;

        _pending.push_back(sample);
        if (static_cast<int>(_pending.size()) == _subblock) {
            _analyse();
            _pending.clear();
        }
    }
}

void Decoder::_analyse() {
    // The analysis frame slides by one step.
    std::move(_frame.begin() + _subblock, _frame.end(), _frame.begin());
    std::copy(_pending.begin(), _pending.end(), _frame.end() - _subblock);

    for (int i = 0; i < _nfft; ++i) _fft_in[i] = _frame[i] * _window[i];
    fft_execute(_fft.get());
    const auto& spectrum = _fft_out;

    std::size_t offset = static_cast<std::size_t>(_blocks) * _block_stride +
                         static_cast<std::size_t>(_time_sub) * kFreqOsr * _num_bins;
    for (int freq_sub = 0; freq_sub < kFreqOsr; ++freq_sub) {
        for (int bin = 0; bin < _num_bins; ++bin) {
            // Negative frequencies are the top half of the transform.
            const int source = (((_min_bin + bin) * kFreqOsr + freq_sub) % _nfft + _nfft) % _nfft;
            const float db = 10.0f * std::log10(1e-12f + std::norm(spectrum[source]));
            const int scaled = static_cast<int>(2 * db + 240);
            _mag[offset++] = static_cast<std::uint8_t>(std::clamp(scaled, 0, 255));
        }
    }

    if (++_time_sub == kTimeOsr) {
        _time_sub = 0;
        ++_blocks;
    }
}

const std::uint8_t* Decoder::_at(const Candidate& candidate) const {
    long long offset = candidate.time_offset;
    offset = offset * kTimeOsr + candidate.time_sub;
    offset = offset * kFreqOsr + candidate.freq_sub;
    offset = offset * _num_bins + candidate.freq_offset;
    return _mag.data() + offset;
}

int Decoder::_sync_score(const Candidate& candidate) const {
    int score = 0;
    int counted = 0;
    const std::uint8_t* base = _at(candidate);

    for (int group = 0; group < kSyncGroups; ++group) {
        for (int k = 0; k < kSyncLength; ++k) {
            const int block = kSyncOffset * group + k;
            const int absolute = candidate.time_offset + block;
            if (absolute < 0) continue;
            if (absolute >= _blocks) break;

            const std::uint8_t* p = base + static_cast<long long>(block) * _block_stride;
            const int expected = _options.sync[k];

            // The expected tone against its neighbours in frequency and in time.
            if (expected > 0) {
                score += p[expected] - p[expected - 1];
                ++counted;
            }
            if (expected < kTones - 1) {
                score += p[expected] - p[expected + 1];
                ++counted;
            }
            if (k > 0 && absolute > 0) {
                score += p[expected] - p[expected - _block_stride];
                ++counted;
            }
            if (k + 1 < kSyncLength && absolute + 1 < _blocks) {
                score += p[expected] - p[expected + _block_stride];
                ++counted;
            }
        }
    }

    return counted > 0 ? score / counted : 0;
}

bool Decoder::_decode(const Candidate& candidate, Frame& frame) const {
    const std::uint8_t* base = _at(candidate);

    float llr[kLdpcN];
    for (int k = 0; k < kDataSymbols; ++k) {
        const int symbol = k + (k < 29 ? 7 : 14);
        const int block = candidate.time_offset + symbol;
        float* out = llr + static_cast<std::ptrdiff_t>(3) * k;

        if (block < 0 || block >= _blocks) {
            out[0] = out[1] = out[2] = 0;
            continue;
        }

        const std::uint8_t* p = base + static_cast<long long>(symbol) * _block_stride;
        float s[8];
        for (int j = 0; j < 8; ++j) s[j] = magnitude_db(p[kGrayMap[j]]);

        out[0] = max4(s[4], s[5], s[6], s[7]) - max4(s[0], s[1], s[2], s[3]);
        out[1] = max4(s[2], s[3], s[6], s[7]) - max4(s[0], s[1], s[4], s[5]);
        out[2] = max4(s[1], s[3], s[5], s[7]) - max4(s[0], s[2], s[4], s[6]);
    }

    // Normalised to the variance the decoder was tuned for.
    float sum = 0, sum2 = 0;
    for (float v : llr) {
        sum += v;
        sum2 += v * v;
    }
    const float variance = (sum2 - sum * sum / kLdpcN) / kLdpcN;
    if (variance <= 0) return false;
    const float norm = std::sqrt(24.0f / variance);
    for (float& v : llr) v *= norm;

    std::uint8_t bits[kLdpcN];
    if (ldpc_decode(llr, _options.ldpc_iterations, bits) > 0) return false;

    std::array<std::uint8_t, kLdpcKBytes> a91{};
    for (int i = 0; i < kLdpcK; ++i) {
        if (bits[i]) a91[i / 8] |= static_cast<std::uint8_t>(0x80 >> (i % 8));
    }

    const auto carried = crc_of(a91);
    auto zeroed = a91;
    zeroed[9] &= 0xf8;
    zeroed[10] = 0;
    zeroed[11] = 0;
    if (crc(zeroed.data(), 96 - 14) != carried) return false;

    std::copy(zeroed.begin(), zeroed.begin() + kPayloadBytes, frame.payload.begin());
    frame.freq_hz = (static_cast<float>(_min_bin + candidate.freq_offset) +
                     static_cast<float>(candidate.freq_sub) / kFreqOsr) *
                    kToneSpacing;
    frame.time_s = (static_cast<float>(candidate.time_offset) +
                    static_cast<float>(candidate.time_sub) / kTimeOsr) *
                       kSymbolPeriod -
                   kAnalysisDelay;
    frame.score = candidate.score;

    // The signal: the tones the frame was sent on, averaged over its symbols. The noise: the
    // waterfall's median. In WSJT-X's 2500 Hz reference bandwidth: 10 log10(2500 / 6.25).
    const auto tones = encode(frame.payload, _options.sync);
    double signal = 0;
    int counted = 0;
    for (int symbol = 0; symbol < kSymbols; ++symbol) {
        const int block = candidate.time_offset + symbol;
        if (block < 0 || block >= _blocks) continue;
        signal += std::pow(
            10.0,
            magnitude_db(base[static_cast<long long>(symbol) * _block_stride + tones[symbol]]) /
                10.0);
        ++counted;
    }

    std::vector<std::uint8_t> sample(
        _mag.begin(), _mag.begin() + static_cast<long long>(_blocks) * _block_stride);
    std::nth_element(sample.begin(), sample.begin() + sample.size() / 2, sample.end());
    const double noise = std::pow(10.0, magnitude_db(sample[sample.size() / 2]) / 10.0);

    if (counted > 0 && noise > 0) {
        const double ratio = std::max(signal / counted / noise - 1.0, 1e-3);
        frame.snr_db = static_cast<float>(10.0 * std::log10(ratio) - 26.0);
    }

    return true;
}

std::vector<Decoder::Frame> Decoder::decode() const {
    std::vector<Candidate> candidates;

    // Time offsets that run past the edges are allowed as long as the data symbols are
    // there; the score averages over the sync symbols it could see.
    for (int time_sub = 0; time_sub < kTimeOsr; ++time_sub) {
        for (int freq_sub = 0; freq_sub < kFreqOsr; ++freq_sub) {
            for (int time_offset = -10; time_offset < 20; ++time_offset) {
                for (int freq_offset = 0; freq_offset + kTones - 1 < _num_bins; ++freq_offset) {
                    Candidate candidate{0, time_offset, freq_offset, time_sub, freq_sub};
                    candidate.score = _sync_score(candidate);
                    if (candidate.score >= _options.min_score) candidates.push_back(candidate);
                }
            }
        }
    }

    const auto count =
        std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(_options.max_candidates));
    std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
                      [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    candidates.resize(count);

    std::vector<Frame> frames;
    std::set<Payload> seen;
    for (const auto& candidate : candidates) {
        Frame frame;
        if (!_decode(candidate, frame)) continue;
        if (!seen.insert(frame.payload).second) continue;
        frames.push_back(frame);
    }

    return frames;
}

}  // namespace ft8
