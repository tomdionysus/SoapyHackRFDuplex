// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace hackrfduplex::gain {

// The HackRF One's gain stages. The RF amplifier is a switch; libhackrf
// documents it as about 11 dB, and SoapySDR drivers for the board have
// always presented it as 14 dB, which this keeps so applications tuned
// for them see the same numbers.
inline constexpr unsigned kRxLnaMaxDb = 40;  // MAX2837 RX IF gain
inline constexpr unsigned kRxLnaStepDb = 8;
inline constexpr unsigned kRxVgaMaxDb = 62;  // MAX2837 RX baseband gain
inline constexpr unsigned kRxVgaStepDb = 2;
inline constexpr unsigned kTxVgaMaxDb = 47;  // MAX2837 TX IF gain
inline constexpr unsigned kTxVgaStepDb = 1;
inline constexpr unsigned kAmpDb = 14;

inline constexpr unsigned kRxMaxDb = kRxLnaMaxDb + kRxVgaMaxDb + kAmpDb;
inline constexpr unsigned kTxMaxDb = kTxVgaMaxDb + kAmpDb;

struct RxSplit {
    unsigned lna_db = 0;
    unsigned vga_db = 0;
    bool amp = false;
    unsigned total() const { return lna_db + vga_db + (amp ? kAmpDb : 0); }
};

struct TxSplit {
    unsigned vga_db = 0;
    bool amp = false;
    unsigned total() const { return vga_db + (amp ? kAmpDb : 0); }
};

// Clamps `value` to [0, max] and rounds it down to a multiple of `step`.
unsigned quantise(double value, unsigned max, unsigned step);

// Spreads one overall RX gain over the stages: the amplifier only when
// the two variable stages cannot reach it, then LNA and VGA in
// proportion to their ranges, the VGA taking what the LNA's 8 dB steps
// leave. The result is never more than requested and within 1 dB below
// it wherever the stages allow.
RxSplit split_rx(double total_db);

// The same for TX: the amplifier only above the VGA's reach.
TxSplit split_tx(double total_db);

}  // namespace hackrfduplex::gain
