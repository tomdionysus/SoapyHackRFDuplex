// SPDX-License-Identifier: GPL-3.0-or-later
//
// From corvus-radioberry (examples/ft8), GPL-2.0-or-later, taken here
// under GPL-3.0-or-later as its "or later" allows.
//
// Copyright (C) 2026 Tom Cully. Licensed under
// GPL-2.0-or-later (here GPL-3.0-or-later). Ported from kgoba/ft8_lib (MIT, Copyright (c) 2018
// Karlis Goba); see src/ft8_lib/LICENSE.
#include "codec.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ft8 {

namespace {

std::uint8_t parity8(std::uint8_t x) {
    x = static_cast<std::uint8_t>(x ^ (x >> 4));
    x = static_cast<std::uint8_t>(x ^ (x >> 2));
    x = static_cast<std::uint8_t>(x ^ (x >> 1));
    return x & 1;
}

// Rational approximations good to the precision belief propagation needs, and far
// cheaper than the library functions in the inner loop.
float fast_tanh(float x) {
    if (x < -4.97f) return -1.0f;
    if (x > 4.97f) return 1.0f;
    const float x2 = x * x;
    return x * (945.0f + x2 * (105.0f + x2)) / (945.0f + x2 * (420.0f + x2 * 15.0f));
}

float fast_atanh(float x) {
    const float x2 = x * x;
    return x * (945.0f + x2 * (-735.0f + x2 * 64.0f)) / (945.0f + x2 * (-1050.0f + x2 * 225.0f));
}

}  // namespace

std::uint16_t crc(const std::uint8_t* message, int num_bits) {
    constexpr std::uint16_t top = 1u << (kCrcWidth - 1);
    std::uint16_t remainder = 0;
    int byte = 0;

    for (int bit = 0; bit < num_bits; ++bit) {
        if (bit % 8 == 0)
            remainder ^= static_cast<std::uint16_t>(message[byte++] << (kCrcWidth - 8));
        remainder = (remainder & top)
                        ? static_cast<std::uint16_t>((remainder << 1) ^ kCrcPolynomial)
                        : static_cast<std::uint16_t>(remainder << 1);
    }

    return remainder & ((top << 1) - 1u);
}

std::array<std::uint8_t, kLdpcKBytes> with_crc(const Payload& payload) {
    std::array<std::uint8_t, kLdpcKBytes> a91{};
    std::copy(payload.begin(), payload.end(), a91.begin());

    // 77 bits zero-extended to 82.
    a91[9] &= 0xf8;
    a91[10] = 0;
    const auto checksum = crc(a91.data(), 96 - 14);

    a91[9] |= static_cast<std::uint8_t>(checksum >> 11);
    a91[10] = static_cast<std::uint8_t>(checksum >> 3);
    a91[11] = static_cast<std::uint8_t>(checksum << 5);
    return a91;
}

std::uint16_t crc_of(const std::array<std::uint8_t, kLdpcKBytes>& a91) {
    return static_cast<std::uint16_t>(((a91[9] & 0x07) << 11) | (a91[10] << 3) | (a91[11] >> 5));
}

std::array<std::uint8_t, kLdpcNBytes> ldpc_encode(
    const std::array<std::uint8_t, kLdpcKBytes>& a91) {
    std::array<std::uint8_t, kLdpcNBytes> codeword{};
    std::copy(a91.begin(), a91.end(), codeword.begin());

    std::uint8_t mask = static_cast<std::uint8_t>(0x80u >> (kLdpcK % 8));
    int index = kLdpcKBytes - 1;

    for (int row = 0; row < kLdpcM; ++row) {
        std::uint8_t sum = 0;
        for (int j = 0; j < kLdpcKBytes; ++j) sum ^= parity8(a91[j] & kLdpcGenerator[row][j]);

        if (sum) codeword[index] |= mask;

        mask >>= 1;
        if (mask == 0) {
            mask = 0x80;
            ++index;
        }
    }

    return codeword;
}

int ldpc_check(const std::uint8_t* bits) {
    int errors = 0;
    for (int m = 0; m < kLdpcM; ++m) {
        std::uint8_t x = 0;
        for (int i = 0; i < kLdpcNumRows[m]; ++i) x ^= bits[kLdpcNm[m][i] - 1];
        if (x) ++errors;
    }
    return errors;
}

int ldpc_decode(const float* llr, int max_iterations, std::uint8_t* bits) {
    float to_variable[kLdpcN][3] = {};
    float to_check[kLdpcM][7] = {};
    int best = kLdpcM;

    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        int ones = 0;
        for (int n = 0; n < kLdpcN; ++n) {
            bits[n] =
                (llr[n] + to_variable[n][0] + to_variable[n][1] + to_variable[n][2]) > 0 ? 1 : 0;
            ones += bits[n];
        }

        // All zeros satisfies every check and is never a real message.
        if (ones == 0) break;

        const int errors = ldpc_check(bits);
        if (errors < best) {
            best = errors;
            if (errors == 0) break;
        }

        for (int m = 0; m < kLdpcM; ++m) {
            for (int k = 0; k < kLdpcNumRows[m]; ++k) {
                const int n = kLdpcNm[m][k] - 1;
                float t = llr[n];
                for (int j = 0; j < 3; ++j) {
                    if (kLdpcMn[n][j] - 1 != m) t += to_variable[n][j];
                }
                to_check[m][k] = fast_tanh(-t / 2);
            }
        }

        for (int n = 0; n < kLdpcN; ++n) {
            for (int j = 0; j < 3; ++j) {
                const int m = kLdpcMn[n][j] - 1;
                float t = 1.0f;
                for (int k = 0; k < kLdpcNumRows[m]; ++k) {
                    if (kLdpcNm[m][k] - 1 != n) t *= to_check[m][k];
                }
                to_variable[n][j] = -2 * fast_atanh(t);
            }
        }
    }

    return best;
}

Tones encode(const Payload& payload, const SyncPattern& sync) {
    const auto codeword = ldpc_encode(with_crc(payload));

    Tones tones{};
    std::uint8_t mask = 0x80;
    int byte = 0;
    const auto next_bit = [&]() {
        const bool bit = (codeword[byte] & mask) != 0;
        mask >>= 1;
        if (mask == 0) {
            mask = 0x80;
            ++byte;
        }
        return bit;
    };

    for (int i = 0; i < kSymbols; ++i) {
        const int in_group = i % kSyncOffset;
        if (in_group < kSyncLength && i / kSyncOffset < kSyncGroups) {
            tones[i] = sync[in_group];
            continue;
        }

        int bits3 = 0;
        for (int b = 0; b < 3; ++b) bits3 = (bits3 << 1) | (next_bit() ? 1 : 0);
        tones[i] = kGrayMap[bits3];
    }

    return tones;
}

std::vector<std::complex<float>> synthesize(const Tones& tones, float f0_hz, float sample_rate) {
    const int per_symbol = static_cast<int>(std::lround(sample_rate * kSymbolPeriod));
    const int samples = kSymbols * per_symbol;
    constexpr float bt = 2.0f;
    // pi * sqrt(2 / ln 2)
    constexpr float k = 5.336446f;

    // The Gaussian frequency pulse, three symbols long.
    std::vector<float> pulse(std::size_t{3} * static_cast<std::size_t>(per_symbol));
    for (int i = 0; i < 3 * per_symbol; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(per_symbol) - 1.5f;
        pulse[i] = (std::erf(k * bt * (t + 0.5f)) - std::erf(k * bt * (t - 0.5f))) / 2;
    }

    // Modulation index 1.
    const float peak = 2 * std::numbers::pi_v<float> / static_cast<float>(per_symbol);
    std::vector<float> dphi(samples + 2 * per_symbol,
                            2 * std::numbers::pi_v<float> * f0_hz / sample_rate);

    for (int i = 0; i < kSymbols; ++i) {
        for (int j = 0; j < 3 * per_symbol; ++j)
            dphi[i * per_symbol + j] += peak * tones[i] * pulse[j];
    }

    // The first and last tones extended beyond the frame, so the edges are not pulled to 0.
    for (int j = 0; j < 2 * per_symbol; ++j) {
        dphi[j] += peak * pulse[j + per_symbol] * tones[0];
        dphi[j + kSymbols * per_symbol] += peak * pulse[j] * tones[kSymbols - 1];
    }

    std::vector<std::complex<float>> out(samples);
    double phase = 0;
    for (int i = 0; i < samples; ++i) {
        out[i] = std::polar(1.0f, static_cast<float>(phase));
        phase = std::fmod(phase + dphi[i + per_symbol], 2 * std::numbers::pi);
    }

    const int ramp = per_symbol / 8;
    for (int i = 0; i < ramp; ++i) {
        const float envelope = (1 - std::cos(2 * std::numbers::pi_v<float> * static_cast<float>(i) /
                                             static_cast<float>(2 * ramp))) /
                               2;
        out[i] *= envelope;
        out[samples - 1 - i] *= envelope;
    }

    return out;
}

}  // namespace ft8
