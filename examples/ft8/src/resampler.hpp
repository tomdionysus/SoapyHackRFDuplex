// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <complex>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "liquid.hpp"

namespace ft8 {

// Between the pair's 1.92 Msps and the 48 ksps the FT8 chain starts at
// (decimator.hpp): one Kaiser low-pass at 20 kHz (401 taps, 70 dB
// stopband), used both ways by liquid-dsp's polyphase filters. 1.92 Msps
// is the lowest rate both directions take that is a whole multiple of
// 48 kHz. Unity gain both ways; stateful, so a stream can be fed in
// pieces of any size.
inline constexpr unsigned kResampleFactor = 40;
inline constexpr float kPairRate = 1.92e6F;
inline constexpr float kAudioRate = kPairRate / kResampleFactor;

class DownTo48k {
public:
    DownTo48k();
    // Appends the output for `input` to `out`.
    void process(std::span<const std::complex<float>> input, std::vector<std::complex<float>>& out);

private:
    struct Destroy {
        void operator()(firdecim_crcf q) const { firdecim_crcf_destroy(q); }
    };
    std::unique_ptr<std::remove_pointer_t<firdecim_crcf>, Destroy> filter_;
    std::vector<std::complex<float>> pending_;
};

class UpFrom48k {
public:
    UpFrom48k();
    // Appends kResampleFactor samples to `out` for each of `input`.
    void process(std::span<const std::complex<float>> input, std::vector<std::complex<float>>& out);

private:
    struct Destroy {
        void operator()(firinterp_crcf q) const { firinterp_crcf_destroy(q); }
    };
    std::unique_ptr<std::remove_pointer_t<firinterp_crcf>, Destroy> filter_;
};

}  // namespace ft8
