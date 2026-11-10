// SPDX-License-Identifier: GPL-3.0-or-later
#include "rx_path.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

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
    if (held_) {
        throw std::logic_error("read: a direct-access buffer is held; release it first");
    }
    const Next n = next(max_samples, timeout);
    if (n.block == nullptr) {
        return n.result;
    }
    from_native(n.first, format, dst, n.result.samples);
    commit(n);
    return n.result;
}

RxView RxPath::acquire(std::size_t max_samples, std::chrono::microseconds timeout) {
    if (held_) {
        throw std::logic_error("acquire: a direct-access buffer is held; one at a time");
    }
    const Next n = next(max_samples, timeout);
    if (n.block == nullptr) {
        return {n.result};
    }
    held_ = n;
    return {n.result, reinterpret_cast<const std::uint8_t*>(n.first), ring_.index_of(n.block)};
}

void RxPath::release(std::size_t slot) {
    if (!held_ || ring_.index_of(held_->block) != slot) {
        throw std::invalid_argument("release: buffer " + std::to_string(slot) +
                                    " is not the one held");
    }
    const Next n = *held_;
    held_.reset();
    commit(n);
}

RxPath::Next RxPath::next(std::size_t max_samples, std::chrono::microseconds timeout) {
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
            Next gap;
            gap.result = RxRead{0, block->position, true};
            return gap;
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
        Next n;
        n.block = block;
        n.result = RxRead{taken, block->position + offset_, false, end_burst};
        n.first = reinterpret_cast<const std::int8_t*>(block->bytes.data()) +
                  offset_ * kNativeSampleBytes;
        n.generation = window.generation;
        return n;
    }
}

void RxPath::commit(const Next& taken) {
    const std::size_t samples = taken.result.samples;
    offset_ += samples;
    expected_ = taken.block->position + offset_;
    if (offset_ * kNativeSampleBytes == taken.block->size) {
        ring_.pop();
        offset_ = 0;
    }
    const std::lock_guard<std::mutex> lock(window_mutex_);
    if (window_.generation == taken.generation) {
        window_.start.reset();
        if (window_.remaining) {
            *window_.remaining -= samples;
            window_.done = *window_.remaining == 0;
        }
    }
}

void RxPath::reset() {
    {
        const std::lock_guard<std::mutex> lock(window_mutex_);
        window_ = Window{{}, {}, false, window_.generation + 1};
    }
    held_.reset();
    ring_.reset();
    produced_ = 0;
    received_ = 0;
    dropped_ = 0;
    expected_ = 0;
    offset_ = 0;
}

}  // namespace hackrfduplex
