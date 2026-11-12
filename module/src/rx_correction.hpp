// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Receive corrections, applied on the reader's thread to the samples an
// application reads, in its own format: first the DC offset, removed
// automatically by a one-pole DC blocker (liquid-dsp) or subtracted as a
// fixed offset; then the I/Q balance, corrected as y = x + b conj(x), with
// b set by the application or estimated blind, by driving the output's
// E[y^2] / E[|y|^2] (which is 0 for a balanced receiver) to 0. Everything
// is off until set. Built only with HACKRFDUPLEX_DSP; samples are at full
// scale 1.0, whatever the format.

#include <complex>
#include <cstddef>
#include <vector>

#include "format.hpp"
#include "mutex.hpp"

namespace hackrfduplex {

class RxCorrection {
public:
    RxCorrection();
    ~RxCorrection();
    RxCorrection(const RxCorrection&) = delete;
    RxCorrection& operator=(const RxCorrection&) = delete;

    // Automatic DC removal; turning it off leaves the fixed offset, if any.
    void set_dc_automatic(bool on);
    bool dc_automatic() const;
    // Subtracted from every sample while automatic removal is off.
    void set_dc_offset(std::complex<double> offset);
    std::complex<double> dc_offset() const;

    // Automatic I/Q balance: b estimated from the samples as they pass.
    void set_iq_automatic(bool on);
    bool iq_automatic() const;
    // The correction's b; in automatic mode, where the estimate starts.
    void set_iq_balance(std::complex<double> balance);
    // The b in use: in automatic mode, the current estimate.
    std::complex<double> iq_balance() const;

    // Whether anything would change a sample.
    bool active() const;
    // Corrects `count` samples of `format` in place.
    void apply(void* samples, Format format, std::size_t count);
    // A new stream: the DC blocker's state is cleared; the settings and
    // the I/Q estimate are kept.
    void reset();

private:
    void correct(std::complex<float>* samples, std::size_t count) REQUIRES(mutex_);
    bool active_locked() const REQUIRES(mutex_);

    mutable Mutex mutex_;
    bool dc_automatic_ GUARDED_BY(mutex_) = false;
    std::complex<double> dc_offset_ GUARDED_BY(mutex_){};
    bool iq_automatic_ GUARDED_BY(mutex_) = false;
    std::complex<double> iq_balance_ GUARDED_BY(mutex_){};
    // liquid-dsp's iirfilt_crcf, kept opaque so this header does not
    // include liquid.h.
    void* dc_blocker_ GUARDED_BY(mutex_) = nullptr;
    // The integer and double formats are corrected through this.
    std::vector<std::complex<float>> scratch_ GUARDED_BY(mutex_);
};

}  // namespace hackrfduplex
