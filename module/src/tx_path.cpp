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
        current_->timed = false;
        // After silence the board inserted, an untimed block goes out no
        // earlier than where the board has got to.
        next_ = std::max(next_, sent_.load());
    }
    return fill(src, samples, format, end_of_burst);
}

TxWrite TxPath::write_at(const void* src, std::size_t samples, Format format, bool end_of_burst,
                         std::uint64_t position, std::chrono::microseconds timeout) {
    if (position < queued()) {
        note_late();
        return TxWrite{0, true};
    }
    if (current_ != nullptr) {
        // What came before goes as it stands; the timed block starts afresh.
        ring_.publish();
        current_ = nullptr;
    }
    if (!ring_.wait_free(timeout)) {
        return {};
    }
    current_ = ring_.acquire();
    if (current_ == nullptr) {
        return {};
    }
    current_->size = 0;
    current_->end_of_burst = false;
    current_->timed = true;
    current_->position = position;
    next_ = position;
    return TxWrite{fill(src, samples, format, end_of_burst), false};
}

std::size_t TxPath::fill(const void* src, std::size_t samples, Format format, bool end_of_burst) {
    const std::size_t room = (ring_.block_bytes() - current_->size) / kNativeSampleBytes;
    const std::size_t taken = std::min(samples, room);
    if (taken > 0) {
        auto* dst = reinterpret_cast<std::int8_t*>(current_->bytes.data()) + current_->size;
        to_native(src, format, dst, taken);
    }
    current_->size += taken * kNativeSampleBytes;
    next_ += taken;
    const bool closes_burst = end_of_burst && taken == samples;
    if (current_->size == ring_.block_bytes() || closes_burst) {
        current_->end_of_burst = closes_burst;
        ring_.publish();
        current_ = nullptr;
    }
    return taken;
}

std::uint64_t TxPath::queued() const { return std::max(next_, sent_.load()); }

void TxPath::note_late() {
    late_ = true;
    ++lates_;
}

void TxPath::on_transfer(std::span<std::uint8_t> buffer) {
    const std::uint64_t start = sent_.load();
    std::size_t filled = 0;
    while (filled < buffer.size()) {
        Block* block = ring_.front();
        if (block == nullptr) {
            break;
        }
        if (offset_ == 0) {
            const std::uint64_t here = start + filled / kNativeSampleBytes;
            if (block->timed && block->position > here) {
                const std::size_t gap = static_cast<std::size_t>(std::min<std::uint64_t>(
                    (block->position - here) * kNativeSampleBytes, buffer.size() - filled));
                std::memset(buffer.data() + filled, 0, gap);
                filled += gap;
                idle_expected_ = true;
                continue;
            }
            if (block->timed && block->position < here) {
                note_late();
                dropping_ = !block->end_of_burst;
                idle_expected_ = true;
                ring_.pop();
                continue;
            }
            if (block->timed) {
                dropping_ = false;
            } else if (dropping_) {
                dropping_ = !block->end_of_burst;
                ring_.pop();
                continue;
            }
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
    next_ = 0;
    offset_ = 0;
    idle_expected_ = true;
    dropping_ = false;
    underflow_ = false;
    late_ = false;
    sent_ = 0;
    underflows_ = 0;
    lates_ = 0;
}

}  // namespace hackrfduplex
