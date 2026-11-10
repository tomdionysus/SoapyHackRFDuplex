// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
//
// Copyright (C) 2026 Tom Cully. Licensed under
// GPL-2.0-or-later (here GPL-3.0-or-later). Ported from kgoba/ft8_lib (MIT, Copyright (c) 2018
// Karlis Goba); see src/ft8_lib/LICENSE.
#pragma once

#include <complex>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "codec.h"
#include "constants.h"
#include "liquid.hpp"

namespace ft8 {

// A slot's worth of complex baseband in, the FT8 frames in it out.
//
// ft8_lib's decoder with its front end rewritten for complex input: a short-time FFT of
// the samples (two analyses per symbol, two bins per tone) makes a waterfall over the
// configured frequency range, which may be either side of the centre; a search over it
// scores every time and frequency offset against the sync pattern; the best candidates'
// symbols become bit likelihoods for the LDPC decoder; the CRC decides.
//
// The sample rate must make a symbol a whole number of samples whose double is a power of
// two (800, 1600, 3200, 6400 or 12800 Hz), because the FFT is radix 2.
class Decoder {
public:
    struct Options {
        float sample_rate = 3200;
        float f_min_hz = -1500;  // relative to the centre of the samples
        float f_max_hz = 1500;
        SyncPattern sync = kStandardFt8Sync;
        int max_candidates = 140;
        int min_score = 10;
        int ldpc_iterations = 25;
    };

    struct Frame {
        Payload payload{};
        float freq_hz = 0;  // tone 0's frequency, relative to the centre
        float time_s = 0;   // the first symbol's start, relative to the first sample fed
        int score = 0;      // the sync score of the candidate it came from
        float snr_db = 0;   // in 2500 Hz, as WSJT-X reports it
    };

    explicit Decoder(Options options);
    static bool valid_rate(float sample_rate);

    // Feeds samples; anything past one slot (93 symbols) is ignored until reset().
    void process(std::span<const std::complex<float>> samples);

    // Every frame in what was fed, each payload once, strongest candidate first.
    std::vector<Frame> decode() const;

    void reset();

    const Options& options() const { return _options; }
    int blocks() const { return _blocks; }

private:
    struct Candidate {
        int score;
        int time_offset;
        int freq_offset;
        int time_sub;
        int freq_sub;
    };

    void _analyse();
    const std::uint8_t* _at(const Candidate& candidate) const;
    int _sync_score(const Candidate& candidate) const;
    bool _decode(const Candidate& candidate, Frame& frame) const;

    Options _options;
    int _block_size;  // samples per symbol
    int _subblock;    // samples per analysis step
    int _nfft;
    int _min_bin;  // in tones, may be negative
    int _num_bins;
    int _max_blocks;
    int _block_stride;

    std::vector<float> _window;
    // liquid-dsp's FFT over _fft_in into _fft_out.
    std::vector<std::complex<float>> _fft_in;
    std::vector<std::complex<float>> _fft_out;
    struct FftDestroy {
        void operator()(fftplan plan) const { fft_destroy_plan(plan); }
    };
    std::unique_ptr<std::remove_pointer_t<fftplan>, FftDestroy> _fft;
    std::vector<std::complex<float>> _frame;    // the last nfft samples
    std::vector<std::complex<float>> _pending;  // samples not yet a whole analysis step
    int _time_sub = 0;
    int _blocks = 0;

    // dB in half-dB steps from -120 dB, [block][time_sub][freq_sub][bin], as ft8_lib keeps it.
    std::vector<std::uint8_t> _mag;
};

}  // namespace ft8
