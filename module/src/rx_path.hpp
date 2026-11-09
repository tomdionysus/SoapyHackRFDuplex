// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "block_ring.hpp"
#include "format.hpp"

namespace hackrfduplex {

struct RxRead {
    // Samples delivered; 0 on timeout or when `overflow` is reported.
    std::size_t samples = 0;
    // Stream index of the first sample delivered, or, with `overflow`, of
    // the first sample after the gap.
    std::uint64_t position = 0;
    // Transfers were dropped before `position` because the ring was full.
    // Reported once per gap, before the data that follows it.
    bool overflow = false;
};

// The receive stream: the board's transfer thread pushes transfers in,
// the reading thread takes samples out in its own format. A transfer
// that finds the ring full is dropped whole and counted, so positions
// stay true to the board's sample clock.
class RxPath {
public:
    RxPath(std::size_t slots, std::size_t transfer_bytes);

    // Samples per transfer, the most one read() returns.
    std::size_t mtu() const { return ring_.block_bytes() / kNativeSampleBytes; }

    // Board thread.
    void on_transfer(std::span<const std::uint8_t> bytes);

    // Reader: up to `max_samples` into `dst`, waiting at most `timeout`.
    RxRead read(void* dst, std::size_t max_samples, Format format,
                std::chrono::microseconds timeout);

    // Releases a reader blocked in read(), which then reports a timeout.
    void wake() { ring_.wake(); }
    // Back to the start of a stream. Only with neither side running.
    void reset();

    // Samples the board delivered, dropped ones included.
    std::uint64_t received() const { return received_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }

private:
    BlockRing ring_;
    // Producer's: where the next transfer starts.
    std::uint64_t produced_ = 0;
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> dropped_{0};
    // Consumer's: the position the next sample should have, and how far
    // into the front block it has read.
    std::uint64_t expected_ = 0;
    std::size_t offset_ = 0;
};

}  // namespace hackrfduplex
