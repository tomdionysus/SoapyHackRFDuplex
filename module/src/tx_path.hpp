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

struct TxWrite {
    // Samples taken; 0 on timeout or when late.
    std::size_t taken = 0;
    // The position asked for was already queued or sent.
    bool late = false;
};

// The transmit stream: the writing thread fills transfer-sized blocks
// in its own format, the board's transfer thread drains them into USB
// transfers. A transfer that finds nothing to send goes out as zeros and
// counts as an underflow, unless the writer closed a burst, after which
// silence is what was asked for.
//
// Stream positions count every sample handed to the board, silence
// included. A timed write puts its block at a position: the board thread
// sends silence until it gets there, which is not an underflow, and a
// block it reaches too late is dropped with the rest of its burst and
// reported. Placement is the board thread's, not the writer's, because
// only the board thread knows where silence it inserted has moved things.
class TxPath {
public:
    TxPath(std::size_t slots, std::size_t transfer_bytes);

    // Samples per transfer, the most one write() takes.
    std::size_t mtu() const { return ring_.block_bytes() / kNativeSampleBytes; }
    // Ring depth in transfers.
    std::size_t slots() const { return ring_.slots(); }

    // Writer: up to `samples` from `src`, returning how many were taken;
    // 0 when no block came free within `timeout`. With `end_of_burst`,
    // once every sample given is taken the block is sent as it stands.
    std::size_t write(const void* src, std::size_t samples, Format format, bool end_of_burst,
                      std::chrono::microseconds timeout);
    // Writer: as write(), the first sample at stream position `position`,
    // taking at most one block; the rest follows with write(). Late, and
    // taking nothing, when `position` is before queued().
    TxWrite write_at(const void* src, std::size_t samples, Format format, bool end_of_burst,
                     std::uint64_t position, std::chrono::microseconds timeout);
    // Writer: where the next untimed sample will go out, at the earliest.
    std::uint64_t queued() const;

    // Board thread: fills the whole buffer.
    void on_transfer(std::span<std::uint8_t> buffer);

    // True once for each underflow since the last call.
    bool take_underflow() { return underflow_.exchange(false); }
    // True once after any late timed write since the last call.
    bool take_late() { return late_.exchange(false); }

    // Releases a writer blocked in write(), which then reports 0.
    void wake() { ring_.wake(); }
    // Back to the start of a stream. Only with neither side running.
    void reset();

    // Samples handed to the board, zeros included.
    std::uint64_t sent() const { return sent_.load(); }
    std::uint64_t underflows() const { return underflows_.load(); }
    // Timed writes refused or dropped as late.
    std::uint64_t lates() const { return lates_.load(); }

private:
    // Fills `current_` and publishes it when full or closing a burst.
    std::size_t fill(const void* src, std::size_t samples, Format format, bool end_of_burst);
    void note_late();

    BlockRing ring_;
    // Writer's: the block being filled, if any, and where the sample after
    // the last one written goes out, as far as the writer knows.
    Block* current_ = nullptr;
    std::uint64_t next_ = 0;
    // Board thread's: how far into the front block it has sent, and
    // whether running dry is expected (nothing written yet, or a burst
    // just ended).
    std::size_t offset_ = 0;
    bool idle_expected_ = true;
    // Dropping the rest of a burst whose timed block was late.
    bool dropping_ = false;
    std::atomic<bool> underflow_{false};
    std::atomic<bool> late_{false};
    std::atomic<std::uint64_t> lates_{0};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> underflows_{0};
};

}  // namespace hackrfduplex
