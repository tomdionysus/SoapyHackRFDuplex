// SPDX-License-Identifier: GPL-3.0-or-later
#include "rx_path.hpp"

#include <algorithm>
#include <cstring>

#include "convert.hpp"

namespace hackrfduplex {

RxPath::RxPath(std::size_t slots, std::size_t transfer_bytes) : ring_(slots, transfer_bytes) {}

void RxPath::on_transfer(std::span<const std::uint8_t> bytes) {
    const std::size_t size = std::min(bytes.size(), ring_.block_bytes());
    const std::size_t samples = size / kNativeSampleBytes;
    {
        const std::lock_guard<std::mutex> lock(tap_mutex_);
        if (tap_) {
            tap_(produced_, bytes.first(samples * kNativeSampleBytes));
        }
    }
    Block* block = ring_.acquire();
    if (block == nullptr) {
        dropped_ += samples;
    } else {
        std::memcpy(block->bytes.data(), bytes.data(), size);
        block->size = samples * kNativeSampleBytes;
        block->position = produced_;
    }
    produced_ += samples;
    received_ += samples;
    if (block != nullptr) {
        ring_.publish();
    }
}

void RxPath::set_tap(RxTap tap) {
    const std::lock_guard<std::mutex> lock(tap_mutex_);
    tap_ = std::move(tap);
}

RxRead RxPath::read(void* dst, std::size_t max_samples, Format format,
                    std::chrono::microseconds timeout) {
    if (!ring_.wait_front(timeout)) {
        return {};
    }
    Block* block = ring_.front();
    if (block == nullptr) {
        return {};
    }
    if (offset_ == 0 && block->position != expected_) {
        expected_ = block->position;
        return RxRead{0, block->position, true};
    }
    const std::size_t available = block->size / kNativeSampleBytes - offset_;
    const std::size_t taken = std::min(max_samples, available);
    const auto* start =
        reinterpret_cast<const std::int8_t*>(block->bytes.data()) + offset_ * kNativeSampleBytes;
    from_native(start, format, dst, taken);
    const RxRead result{taken, block->position + offset_, false};
    offset_ += taken;
    expected_ = block->position + offset_;
    if (offset_ * kNativeSampleBytes == block->size) {
        ring_.pop();
        offset_ = 0;
    }
    return result;
}

void RxPath::reset() {
    ring_.reset();
    produced_ = 0;
    received_ = 0;
    dropped_ = 0;
    expected_ = 0;
    offset_ = 0;
}

}  // namespace hackrfduplex
