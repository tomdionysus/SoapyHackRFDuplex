// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <stdexcept>
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
    // Keep every byte the transmitter was pulled for; set before it starts.
    std::atomic<bool> record{false};
    // The wait between transfers, in microseconds. Real transfers are 6.5 ms
    // or more apart; the default keeps the tests quick.
    std::atomic<long> pace_us{20};
    // After stop(): what was recorded.
    const std::vector<std::int8_t>& sent() const { return sent_; }

private:
    void run() {
        std::uint32_t noise = 12345;
        while (!stop_) {
            if (paused || !tx_.tx_streaming || !rx_.rx_streaming) {
                std::this_thread::sleep_for(std::chrono::microseconds(20));
                continue;
            }
            // A board may stop between the look above and the call.
            std::vector<std::uint8_t> sent;
            try {
                sent = tx_.pull_tx(tx_.transfer_bytes_value);
            } catch (const std::logic_error&) {
                continue;
            }
            for (const std::uint8_t b : sent) {
                air_.push_back(coupled ? static_cast<std::int8_t>(b) : 0);
                if (record) {
                    sent_.push_back(static_cast<std::int8_t>(b));
                }
            }
            std::vector<std::uint8_t> heard(sent.size());
            for (std::uint8_t& b : heard) {
                noise = noise * 1103515245U + 12345U;
                const auto jitter =
                    static_cast<std::int8_t>(static_cast<int>((noise >> 16) % 7) - 3);
                b = static_cast<std::uint8_t>(static_cast<std::int8_t>(air_.front() / 4 + jitter));
                air_.pop_front();
            }
            try {
                rx_.deliver_rx(heard);
            } catch (const std::logic_error&) {
            }
            std::this_thread::sleep_for(std::chrono::microseconds(pace_us.load()));
        }
    }

    FakeBoard& tx_;
    FakeBoard& rx_;
    std::deque<std::int8_t> air_;
    std::vector<std::int8_t> sent_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace hackrfduplex::test
