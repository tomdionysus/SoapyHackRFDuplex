// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
//
// Copyright (C) 2026 Tom Cully. Licensed under
// GPL-2.0-or-later (here GPL-3.0-or-later). Ported from kgoba/ft8_lib (MIT, Copyright (c) 2018
// Karlis Goba); see src/ft8_lib/LICENSE.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

#include "constants.h"

namespace ft8 {

// The 14-bit CRC of the first num_bits of message (most significant bit first).
std::uint16_t crc(const std::uint8_t* message, int num_bits);

// The payload with its CRC appended: 91 bits in 12 bytes. The CRC is computed over the
// 77 bits zero-extended to 82, as standard FT8 does.
std::array<std::uint8_t, kLdpcKBytes> with_crc(const Payload& payload);

// The CRC carried in 91 bits.
std::uint16_t crc_of(const std::array<std::uint8_t, kLdpcKBytes>& a91);

// 91 bits to the 174-bit codeword.
std::array<std::uint8_t, kLdpcNBytes> ldpc_encode(const std::array<std::uint8_t, kLdpcKBytes>& a91);

// How many of the 83 parity checks the 174 hard bits (each 0 or 1) fail.
int ldpc_check(const std::uint8_t* bits);

// Belief-propagation decoding of 174 log-likelihoods, log(P(1)/P(0)), into 174 bits.
// Returns the parity checks still failing: zero is a codeword.
int ldpc_decode(const float* llr, int max_iterations, std::uint8_t* bits);

using Tones = std::array<std::uint8_t, kSymbols>;

// The 79 tones of a payload, with `sync` in the three sync positions.
Tones encode(const Payload& payload, const SyncPattern& sync = kStandardFt8Sync);

// The tones as complex baseband: Gaussian-filtered FSK (BT 2, modulation index 1) with
// tone 0 at f0_hz from the centre, at sample_rate, amplitude 1, the first and last
// symbols' envelope ramped. kSymbols * kSymbolPeriod * sample_rate samples.
std::vector<std::complex<float>> synthesize(const Tones& tones, float f0_hz, float sample_rate);

}  // namespace ft8
