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

void RxPath::set_window(std::optional<std::uint64_t> start, std::optional<std::uint64_t> count) {
    {
        const std::lock_guard<std::mutex> lock(window_mutex_);
        window_ = Window{start, count, false, window_.generation + 1};
    }
}

RxRead RxPath::read(void* dst, std::size_t max_samples, Format format,
                    std::chrono::microseconds timeout) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + timeout;
    while (true) {
        Window window;
        {
            const std::lock_guard<std::mutex> lock(window_mutex_);
            window = window_;
        }
        Block* block = ring_.front();
        if (block == nullptr) {
            const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
                std::max(deadline - Clock::now(), Clock::duration::zero()));
            if (!ring_.wait_front(left)) {
                return {};
            }
            continue;
        }
        const std::uint64_t end = block->position + block->size / kNativeSampleBytes;
        // Nothing in this block is wanted: dropped without a word, gaps
        // included, since the positions say where the window falls anyway.
        if (window.done || (window.start && end <= *window.start)) {
            expected_ = end;
            offset_ = 0;
            ring_.pop();
            continue;
        }
        if (offset_ == 0 && block->position != expected_) {
            expected_ = block->position;
            return RxRead{0, block->position, true};
        }
        if (window.start && block->position + offset_ < *window.start) {
            offset_ = static_cast<std::size_t>(*window.start - block->position);
            expected_ = *window.start;
        }
        std::size_t taken = std::min(max_samples, block->size / kNativeSampleBytes - offset_);
        bool end_burst = false;
        if (window.remaining) {
            taken = static_cast<std::size_t>(std::min<std::uint64_t>(taken, *window.remaining));
            end_burst = taken == *window.remaining;
        }
        const auto* first = reinterpret_cast<const std::int8_t*>(block->bytes.data()) +
                            offset_ * kNativeSampleBytes;
        from_native(first, format, dst, taken);
        const RxRead result{taken, block->position + offset_, false, end_burst};
        offset_ += taken;
        expected_ = block->position + offset_;
        if (offset_ * kNativeSampleBytes == block->size) {
            ring_.pop();
            offset_ = 0;
        }
        {
            const std::lock_guard<std::mutex> lock(window_mutex_);
            if (window_.generation == window.generation) {
                window_.start.reset();
                if (window_.remaining) {
                    *window_.remaining -= taken;
                    window_.done = *window_.remaining == 0;
                }
            }
        }
        return result;
    }
}

void RxPath::reset() {
    {
        const std::lock_guard<std::mutex> lock(window_mutex_);
        window_ = Window{{}, {}, false, window_.generation + 1};
    }
    ring_.reset();
    produced_ = 0;
    received_ = 0;
    dropped_ = 0;
    expected_ = 0;
    offset_ = 0;
}

}  // namespace hackrfduplex
