// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <thread>
#include <vector>

#include "fake_board.hpp"

namespace hackrfduplex::test {

// The air between two fake boards: once both stream, each transmitted
// transfer is pulled and delivered to the receiver `delay` samples later,
// a quarter as strong, both stream positions moving in step. The boards'
// transfers must be one size. Uncoupled, the receiver hears only a little
// noise. Stop it before stopping either board.
class Loopback {
public:
    Loopback(FakeBoard& tx, FakeBoard& rx, std::size_t delay)
        : tx_(tx), rx_(rx), air_(delay * 2, 0), thread_([this] { run(); }) {}
    ~Loopback() { stop(); }
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    void stop() {
        stop_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    std::atomic<bool> coupled{true};
    // Paused, no transfer moves either way, so a test can hold the pair
    // inside an operation that waits on the air.
    std::atomic<bool> paused{false};

private:
    void run() {
        std::uint32_t noise = 12345;
        while (!stop_) {
            if (paused || !tx_.tx_streaming || !rx_.rx_streaming) {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }
            const std::vector<std::uint8_t> sent = tx_.pull_tx(tx_.transfer_bytes_value);
            for (const std::uint8_t b : sent) {
                air_.push_back(coupled ? static_cast<std::int8_t>(b) : 0);
            }
            std::vector<std::uint8_t> heard(sent.size());
            for (std::uint8_t& b : heard) {
                noise = noise * 1103515245U + 12345U;
                const auto jitter =
                    static_cast<std::int8_t>(static_cast<int>((noise >> 16) % 7) - 3);
                b = static_cast<std::uint8_t>(static_cast<std::int8_t>(air_.front() / 4 + jitter));
                air_.pop_front();
            }
            rx_.deliver_rx(heard);
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }

    FakeBoard& tx_;
    FakeBoard& rx_;
    std::deque<std::int8_t> air_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace hackrfduplex::test
