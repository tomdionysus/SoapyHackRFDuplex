// SPDX-License-Identifier: GPL-3.0-or-later
#include "gain.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

using namespace hackrfduplex::gain;

TEST_CASE("quantise clamps and rounds down to the step", "[gain]") {
    CHECK(quantise(0, 40, 8) == 0);
    CHECK(quantise(-5, 40, 8) == 0);
    CHECK(quantise(17, 40, 8) == 16);
    CHECK(quantise(100, 62, 2) == 62);
    CHECK(quantise(3.9, 47, 1) == 3);
    CHECK(quantise(40, 40, 8) == 40);
}

TEST_CASE("an RX gain splits into valid stage settings within 1 dB", "[gain]") {
    for (int half_db = 0; half_db <= 260; ++half_db) {
        const double wanted = half_db / 2.0;
        const RxSplit split = split_rx(wanted);
        INFO("wanted " << wanted);
        CHECK(split.lna_db % kRxLnaStepDb == 0);
        CHECK(split.lna_db <= kRxLnaMaxDb);
        CHECK(split.vga_db % kRxVgaStepDb == 0);
        CHECK(split.vga_db <= kRxVgaMaxDb);
        const unsigned reachable = std::min(static_cast<unsigned>(wanted), kRxMaxDb);
        CHECK(split.total() <= reachable);
        CHECK(reachable - split.total() <= 1);
    }
}

TEST_CASE("RX gain spends the amplifier last", "[gain]") {
    CHECK(split_rx(0).total() == 0);
    CHECK_FALSE(split_rx(0).amp);
    CHECK_FALSE(split_rx(102).amp);
    CHECK(split_rx(102).lna_db == 40);
    CHECK(split_rx(102).vga_db == 62);
    CHECK(split_rx(103).amp);
    const RxSplit mid = split_rx(50);
    CHECK(mid.lna_db == 16);
    CHECK(mid.vga_db == 34);
    CHECK_FALSE(mid.amp);
    const RxSplit max = split_rx(200);
    CHECK(max.lna_db == 40);
    CHECK(max.vga_db == 62);
    CHECK(max.amp);
}

TEST_CASE("a TX gain fills the VGA before switching the amplifier", "[gain]") {
    CHECK(split_tx(0).total() == 0);
    CHECK(split_tx(47).vga_db == 47);
    CHECK_FALSE(split_tx(47).amp);
    CHECK(split_tx(48).amp);
    CHECK(split_tx(48).vga_db == 34);
    CHECK(split_tx(61).vga_db == 47);
    CHECK(split_tx(61).amp);
    CHECK(split_tx(100).total() == kTxMaxDb);
    for (int half_db = 0; half_db <= 140; ++half_db) {
        const double wanted = half_db / 2.0;
        const TxSplit split = split_tx(wanted);
        const unsigned reachable = std::min(static_cast<unsigned>(wanted), kTxMaxDb);
        INFO("wanted " << wanted);
        CHECK(split.vga_db <= kTxVgaMaxDb);
        CHECK(split.total() == reachable);
    }
}
