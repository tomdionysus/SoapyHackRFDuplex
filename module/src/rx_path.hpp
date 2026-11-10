// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "block_ring.hpp"
#include "format.hpp"
#include "mutex.hpp"

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
    // The last samples of a finite window (set_window).
    bool end_burst = false;
};

// A read's samples left where they lie in the ring (RxPath::acquire).
struct RxView {
    RxRead read;
    // The first sample, when read.samples is not 0.
    const std::uint8_t* data = nullptr;
    // The ring slot holding them, to give to release().
    std::size_t slot = 0;
};

// Called on the board's thread with each transfer's stream position and
// its native bytes, whether or not the ring had room for it.
using RxTap = std::function<void(std::uint64_t, std::span<const std::uint8_t>)>;

// The receive stream: the board's transfer thread pushes transfers in,
// the reading thread takes samples out in its own format. A transfer
// that finds the ring full is dropped whole and counted, so positions
// stay true to the board's sample clock.
class RxPath {
public:
    RxPath(std::size_t slots, std::size_t transfer_bytes);

    // Samples per transfer, the most one read() returns.
    std::size_t mtu() const { return ring_.block_bytes() / kNativeSampleBytes; }
    std::size_t slots() const { return ring_.slots(); }
    // A slot's bytes, wherever its block is in the stream.
    std::uint8_t* slot_bytes(std::size_t slot) { return ring_.slot(slot).bytes.data(); }

    // Board thread.
    void on_transfer(std::span<const std::uint8_t> bytes);

    // Any thread: a listener beside the reader, which it does not disturb;
    // nullptr removes it. Once this returns the old tap is not called again.
    void set_tap(RxTap tap);

    // Reader: up to `max_samples` into `dst`, waiting at most `timeout`.
    // Throws std::logic_error while acquire() holds a block.
    RxRead read(void* dst, std::size_t max_samples, Format format,
                std::chrono::microseconds timeout);
    // Reader: as read(), but the samples stay in the ring, native, and the
    // block holding them is held until release(): one at a time, so this
    // and read() throw std::logic_error while one is held.
    RxView acquire(std::size_t max_samples, std::chrono::microseconds timeout);
    // Reader: gives back what acquire() handed out. Throws
    // std::invalid_argument for any other slot.
    void release(std::size_t slot);

    // Any thread: what the reader delivers from here on. With `start`, the
    // samples before that position are dropped and the first read begins
    // exactly at it; with `count`, that many samples are delivered, the last
    // read flagged end_burst, and nothing after. Neither: everything, as
    // after reset().
    void set_window(std::optional<std::uint64_t> start, std::optional<std::uint64_t> count);

    // Releases a reader blocked in read(), which then reports a timeout.
    void wake() { ring_.wake(); }
    // Back to the start of a stream. Only with neither side running.
    void reset();

    // Samples the board delivered, dropped ones included.
    std::uint64_t received() const { return received_.load(); }
    std::uint64_t dropped() const { return dropped_.load(); }

private:
    // What the next read delivers: `block` null when it delivers nothing
    // (a timeout or a gap, in `result`).
    struct Next {
        Block* block = nullptr;
        RxRead result;
        const std::int8_t* first = nullptr;
        std::uint64_t generation = 0;
    };
    Next next(std::size_t max_samples, std::chrono::microseconds timeout);
    // Moves past what `taken` delivered.
    void commit(const Next& taken);

    BlockRing ring_;
    std::optional<Next> held_;
    // Producer's: where the next transfer starts.
    std::uint64_t produced_ = 0;
    std::atomic<std::uint64_t> received_{0};
    std::atomic<std::uint64_t> dropped_{0};
    Mutex tap_mutex_;
    RxTap tap_ GUARDED_BY(tap_mutex_);
    // Consumer's: the position the next sample should have, and how far
    // into the front block it has read.
    std::uint64_t expected_ = 0;
    std::size_t offset_ = 0;

    struct Window {
        std::optional<std::uint64_t> start;
        // Samples still to deliver.
        std::optional<std::uint64_t> remaining;
        // A finite window delivered in full.
        bool done = false;
        // Changes with every set_window, so the reader updates only the
        // window it read from.
        std::uint64_t generation = 0;
    };
    Mutex window_mutex_;
    Window window_ GUARDED_BY(window_mutex_);
};

}  // namespace hackrfduplex
