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

// The transmit stream: the writing thread fills transfer-sized blocks
// in its own format, the board's transfer thread drains them into USB
// transfers. A transfer that finds nothing to send goes out as zeros and
// counts as an underflow, unless the writer closed a burst, after which
// silence is what was asked for.
class TxPath {
public:
    TxPath(std::size_t slots, std::size_t transfer_bytes);

    // Samples per transfer, the most one write() takes.
    std::size_t mtu() const { return ring_.block_bytes() / kNativeSampleBytes; }

    // Writer: up to `samples` from `src`, returning how many were taken;
    // 0 when no block came free within `timeout`. With `end_of_burst`,
    // once every sample given is taken the block is sent as it stands.
    std::size_t write(const void* src, std::size_t samples, Format format, bool end_of_burst,
                      std::chrono::microseconds timeout);

    // Board thread: fills the whole buffer.
    void on_transfer(std::span<std::uint8_t> buffer);

    // True once for each underflow since the last call.
    bool take_underflow() { return underflow_.exchange(false); }

    // Releases a writer blocked in write(), which then reports 0.
    void wake() { ring_.wake(); }
    // Back to the start of a stream. Only with neither side running.
    void reset();

    // Samples handed to the board, zeros included.
    std::uint64_t sent() const { return sent_.load(); }
    std::uint64_t underflows() const { return underflows_.load(); }

private:
    BlockRing ring_;
    // Writer's: the block being filled, if any.
    Block* current_ = nullptr;
    // Board thread's: how far into the front block it has sent, and
    // whether running dry is expected (nothing written yet, or a burst
    // just ended).
    std::size_t offset_ = 0;
    bool idle_expected_ = true;
    std::atomic<bool> underflow_{false};
    std::atomic<std::uint64_t> sent_{0};
    std::atomic<std::uint64_t> underflows_{0};
};

}  // namespace hackrfduplex
