// SPDX-License-Identifier: GPL-3.0-or-later
#include "gain.hpp"

#include <algorithm>
#include <cmath>

namespace hackrfduplex::gain {

unsigned quantise(double value, unsigned max, unsigned step) {
    if (!(value > 0)) {
        return 0;
    }
    const double clamped = std::min(value, static_cast<double>(max));
    const auto whole = static_cast<unsigned>(std::floor(clamped));
    return whole - whole % step;
}

RxSplit split_rx(double total_db) {
    RxSplit split;
    unsigned wanted = quantise(total_db, kRxMaxDb, 1);
    if (wanted > kRxLnaMaxDb + kRxVgaMaxDb) {
        split.amp = true;
        wanted -= kAmpDb;
    }
    // LNA share in proportion to its range, in its own steps.
    const double share =
        static_cast<double>(wanted) * kRxLnaMaxDb / static_cast<double>(kRxLnaMaxDb + kRxVgaMaxDb);
    split.lna_db = quantise(share, kRxLnaMaxDb, kRxLnaStepDb);
    // The VGA takes the rest; where that exceeds its range, the LNA steps up.
    while (wanted - split.lna_db > kRxVgaMaxDb && split.lna_db < kRxLnaMaxDb) {
        split.lna_db += kRxLnaStepDb;
    }
    split.vga_db = quantise(static_cast<double>(wanted - split.lna_db), kRxVgaMaxDb, kRxVgaStepDb);
    return split;
}

TxSplit split_tx(double total_db) {
    TxSplit split;
    unsigned wanted = quantise(total_db, kTxMaxDb, 1);
    if (wanted > kTxVgaMaxDb) {
        split.amp = true;
        wanted -= kAmpDb;
    }
    split.vga_db = quantise(static_cast<double>(wanted), kTxVgaMaxDb, kTxVgaStepDb);
    return split;
}

}  // namespace hackrfduplex::gain
