// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A HackRF's streaming engine (the LPC43xx's M0 core) as its state reads
// over USB, and what a reading says about a start. The firmware
// acknowledges a start or stop before its M4 core passes it to the M0,
// and the M0 keeps the last stream's counts, error included, until it
// takes up the next start (firmware sgpio_m0.s, "Don't reset counts on a
// transition to IDLE"). So for a moment after either call the M0 reads as
// it did before, and a board waits for it to move before trusting what it
// says about the stream.

#include <cstdint>

namespace hackrfduplex {

// The firmware's m0_mode values.
inline constexpr std::uint32_t kM0ModeIdle = 0;
inline constexpr std::uint32_t kM0ModeWait = 1;
inline constexpr std::uint32_t kM0ModeRx = 2;
inline constexpr std::uint32_t kM0ModeTxStart = 3;
inline constexpr std::uint32_t kM0ModeTxRun = 4;

struct M0Reading {
    std::uint32_t active_mode = kM0ModeIdle;
    // Bytes moved since the M0 took up its mode.
    std::uint32_t bytes = 0;
    std::uint32_t shortfalls = 0;
    // 0, or the timeout that idled it (1 receive, 2 transmit).
    std::uint32_t error = 0;
};

// Whether `now`, read after a start, shows the M0 has taken it up, given
// `before`, read idle just before the start: it streams, or it has gone
// idle again on counts the start reset.
bool m0_took_up_start(const M0Reading& before, const M0Reading& now);

// Whether the M0 has dropped to idle, as it must before a stop is done.
bool m0_idle(const M0Reading& now);

}  // namespace hackrfduplex
