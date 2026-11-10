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
#include <cstddef>
#include <cstdint>

namespace ft8 {

// FT8: timing, coding and synchronisation.

inline constexpr float kSymbolPeriod = 0.160f;  // seconds: 6.25 Hz tone spacing
inline constexpr float kSlotTime = 15.0f;
inline constexpr float kToneSpacing = 1.0f / kSymbolPeriod;
inline constexpr int kTones = 8;

// S7 D29 S7 D29 S7.
inline constexpr int kDataSymbols = 58;
inline constexpr int kSymbols = 79;
inline constexpr int kSyncLength = 7;
inline constexpr int kSyncGroups = 3;
inline constexpr int kSyncOffset = 36;

// A transmission starts this long after the slot's start, as standard FT8 does, and lasts
// kSymbols * kSymbolPeriod = 12.64 s.
inline constexpr float kStartOffset = 0.5f;

// LDPC(174,91) over 77 payload bits and a 14-bit CRC.
inline constexpr int kPayloadBits = 77;
inline constexpr int kPayloadBytes = 10;
inline constexpr int kLdpcN = 174;
inline constexpr int kLdpcK = 91;
inline constexpr int kLdpcM = 83;
inline constexpr int kLdpcNBytes = (kLdpcN + 7) / 8;
inline constexpr int kLdpcKBytes = (kLdpcK + 7) / 8;

inline constexpr std::uint16_t kCrcPolynomial = 0x2757;
inline constexpr int kCrcWidth = 14;

// The synchronisation tones, sent in all three sync positions: FT8's
// 7x7 Costas array.
using SyncPattern = std::array<std::uint8_t, kSyncLength>;
inline constexpr SyncPattern kStandardFt8Sync = {3, 1, 4, 0, 6, 5, 2};

// Gray code from 3 bits to a tone, as standard FT8.
inline constexpr std::array<std::uint8_t, 8> kGrayMap = {0, 1, 3, 2, 5, 6, 4, 7};

// The 77 payload bits, most significant bit first in 10 bytes; the last 3 bits of the
// last byte are unused.
using Payload = std::array<std::uint8_t, kPayloadBytes>;

// The parity generator, parity check rows (Nm), and the checks each bit is in (Mn), as
// WSJT-X defines the code. Indexes in Nm and Mn are 1-based.
extern const std::uint8_t kLdpcGenerator[kLdpcM][kLdpcKBytes];
extern const std::uint8_t kLdpcNm[kLdpcM][7];
extern const std::uint8_t kLdpcMn[kLdpcN][3];
extern const std::uint8_t kLdpcNumRows[kLdpcM];

}  // namespace ft8
