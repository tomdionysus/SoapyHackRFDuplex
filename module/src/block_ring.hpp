// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hackrfduplex {

// One USB transfer's worth of int8 I/Q bytes and where it sits in the
// stream.
struct Block {
    std::vector<std::uint8_t> bytes;
    // Bytes in use; a whole transfer except at the end of a TX burst.
    std::size_t size = 0;
    // Stream index, in samples, of the first sample.
    std::uint64_t position = 0;
    // TX: the writer closed a burst with this block, so silence after it
    // is expected rather than an underflow.
    bool end_of_burst = false;
};

// Single-producer single-consumer ring of preallocated blocks between a
// libhackrf transfer thread and the thread reading or writing the
// stream. The producer fills acquire() then publish()es it; the consumer
// uses front() then pop()s it. Either side may wait with a timeout for
// the other, and wake() releases both so a stream can be stopped.
class BlockRing {
public:
    // Throws std::invalid_argument when either is 0.
    BlockRing(std::size_t slots, std::size_t block_bytes);
    BlockRing(const BlockRing&) = delete;
    BlockRing& operator=(const BlockRing&) = delete;

    std::size_t slots() const { return slots_.size(); }
    std::size_t block_bytes() const { return block_bytes_; }
    // Blocks published and not yet popped.
    std::size_t size() const;

    // Producer: the next free block, or nullptr when the ring is full.
    Block* acquire();
    // Producer: hands the block from acquire() to the consumer.
    void publish();
    // Producer: true once acquire() will succeed; false on timeout or wake().
    bool wait_free(std::chrono::microseconds timeout);

    // Consumer: the oldest published block, or nullptr when empty.
    Block* front();
    // Consumer: frees the block from front().
    void pop();
    // Consumer: true once front() will return a block; false on timeout or wake().
    bool wait_front(std::chrono::microseconds timeout);

    // Releases both waits, which then report false.
    void wake();
    // Drops every block. Only with neither side running.
    void reset();

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Block> slots_;
    std::size_t block_bytes_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    std::uint64_t wakes_ = 0;
};

}  // namespace hackrfduplex
