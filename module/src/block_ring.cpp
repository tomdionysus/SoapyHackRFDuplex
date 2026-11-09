// SPDX-License-Identifier: GPL-3.0-or-later
#include "block_ring.hpp"

#include <stdexcept>

namespace hackrfduplex {

BlockRing::BlockRing(std::size_t slots, std::size_t block_bytes)
    : slots_(slots), block_bytes_(block_bytes) {
    if (slots == 0 || block_bytes == 0) {
        throw std::invalid_argument("a block ring needs at least one slot and one byte");
    }
    for (Block& block : slots_) {
        block.bytes.resize(block_bytes);
    }
}

std::size_t BlockRing::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

Block* BlockRing::acquire() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == slots_.size()) {
        return nullptr;
    }
    return &slots_[head_];
}

void BlockRing::publish() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        head_ = (head_ + 1) % slots_.size();
        ++count_;
    }
    changed_.notify_all();
}

bool BlockRing::wait_free(std::chrono::microseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t seen = wakes_;
    changed_.wait_for(lock, timeout, [&] { return count_ < slots_.size() || wakes_ != seen; });
    return count_ < slots_.size() && wakes_ == seen;
}

Block* BlockRing::front() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) {
        return nullptr;
    }
    return &slots_[tail_];
}

void BlockRing::pop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Block& block = slots_[tail_];
        block.size = 0;
        block.end_of_burst = false;
        tail_ = (tail_ + 1) % slots_.size();
        --count_;
    }
    changed_.notify_all();
}

bool BlockRing::wait_front(std::chrono::microseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t seen = wakes_;
    changed_.wait_for(lock, timeout, [&] { return count_ > 0 || wakes_ != seen; });
    return count_ > 0 && wakes_ == seen;
}

void BlockRing::wake() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ++wakes_;
    }
    changed_.notify_all();
}

void BlockRing::reset() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (Block& block : slots_) {
        block.size = 0;
        block.position = 0;
        block.end_of_burst = false;
    }
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

}  // namespace hackrfduplex
