// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
#pragma once

#include <complex>
#include <memory>
#include <span>
#include <vector>

#include "liquid.hpp"

namespace ft8 {

// 48 ksps complex baseband down to the decoder's 3200 sps: a unity-gain
// Kaiser low-pass at the output Nyquist, 1600 Hz (1501 taps, 70 dB
// stopband), and every fifteenth sample, through liquid-dsp. It is flat to
// about 1530 Hz and what aliases into that starts about 70 dB down, so
// FT8's 3 kHz, centred, comes through whole.
// Stateful: a stream can be fed in pieces of any size.
class Decimator {
public:
    static constexpr unsigned kFactor = 15;
    static constexpr float kInputRate = 48000.0f;
    static constexpr float kOutputRate = kInputRate / kFactor;

    Decimator();

    // Appends the output for `input` to `out`.
    void process(std::span<const std::complex<float>> input, std::vector<std::complex<float>>& out);

private:
    struct Destroy {
        void operator()(firdecim_crcf q) const { firdecim_crcf_destroy(q); }
    };
    std::unique_ptr<std::remove_pointer_t<firdecim_crcf>, Destroy> filter_;
    std::vector<std::complex<float>> pending_;
};

}  // namespace ft8
