// SPDX-License-Identifier: GPL-3.0-or-later
#include "tx_path.hpp"

#include <algorithm>
#include <cstring>

#include "convert.hpp"

namespace hackrfduplex {

TxPath::TxPath(std::size_t slots, std::size_t transfer_bytes) : ring_(slots, transfer_bytes) {}

std::size_t TxPath::write(const void* src, std::size_t samples, Format format, bool end_of_burst,
                          std::chrono::microseconds timeout) {
    if (current_ == nullptr) {
        if (!ring_.wait_free(timeout)) {
            return 0;
        }
        current_ = ring_.acquire();
        if (current_ == nullptr) {
            return 0;
        }
        current_->size = 0;
        current_->end_of_burst = false;
    }
    const std::size_t room = (ring_.block_bytes() - current_->size) / kNativeSampleBytes;
    const std::size_t taken = std::min(samples, room);
    auto* dst = reinterpret_cast<std::int8_t*>(current_->bytes.data()) + current_->size;
    to_native(src, format, dst, taken);
    current_->size += taken * kNativeSampleBytes;
    const bool closes_burst = end_of_burst && taken == samples;
    if (current_->size == ring_.block_bytes() || closes_burst) {
        current_->end_of_burst = closes_burst;
        ring_.publish();
        current_ = nullptr;
    }
    return taken;
}

void TxPath::on_transfer(std::span<std::uint8_t> buffer) {
    std::size_t filled = 0;
    while (filled < buffer.size()) {
        Block* block = ring_.front();
        if (block == nullptr) {
            break;
        }
        const std::size_t take = std::min(block->size - offset_, buffer.size() - filled);
        std::memcpy(buffer.data() + filled, block->bytes.data() + offset_, take);
        offset_ += take;
        filled += take;
        if (offset_ == block->size) {
            idle_expected_ = block->end_of_burst;
            offset_ = 0;
            ring_.pop();
        } else {
            idle_expected_ = false;
        }
    }
    if (filled < buffer.size()) {
        std::memset(buffer.data() + filled, 0, buffer.size() - filled);
        if (!idle_expected_) {
            underflow_ = true;
            ++underflows_;
        }
    }
    sent_ += buffer.size() / kNativeSampleBytes;
}

void TxPath::reset() {
    ring_.reset();
    current_ = nullptr;
    offset_ = 0;
    idle_expected_ = true;
    underflow_ = false;
    sent_ = 0;
    underflows_ = 0;
}

}  // namespace hackrfduplex
