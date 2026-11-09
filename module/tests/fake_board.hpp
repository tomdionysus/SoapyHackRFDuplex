// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "board.hpp"

namespace hackrfduplex::test {

// A HackRF the test drives: every setting is recorded, transfers are
// delivered and pulled by hand, and the firmware's habits are copied
// where the pair depends on them (a sample rate resets the filter, going
// idle drops the bias tee).
class FakeBoard final : public Board {
public:
    explicit FakeBoard(std::string serial = "000000000000000000000000a06063c8",
                       int* stop_count = nullptr);

    std::string serial() const override { return serial_; }
    BoardInfo info() override;
    void set_frequency(std::uint64_t hz) override;
    void set_sample_rate(double hz) override;
    void set_bandwidth(std::uint32_t hz) override;
    void set_amp(bool on) override;
    void set_lna_gain(unsigned db) override;
    void set_vga_gain(unsigned db) override;
    void set_tx_vga_gain(unsigned db) override;
    void set_bias_tee(bool on) override;
    void set_clock_out(bool on) override;
    bool start_switches_reference() override { return reference_switch_pending; }
    void set_hardware_sync(bool on) override;
    StreamHealth stream_health() override;
    bool set_tx_underrun_limit(std::uint32_t samples) override;
    std::size_t transfer_bytes() const override { return transfer_bytes_value; }
    void start_rx(RxHandler handler) override;
    void start_tx(TxHandler handler) override;
    void stop() override;
    bool streaming() override { return !unplugged && (rx_streaming || tx_streaming); }

    // Test side: a transfer from the board. Throws std::logic_error
    // unless receiving.
    void deliver_rx(std::span<const std::uint8_t> bytes);
    // Test side: the board pulls one transfer of `bytes`. Throws
    // std::logic_error unless transmitting.
    std::vector<std::uint8_t> pull_tx(std::size_t bytes);

    // Every call, as "name value".
    std::vector<std::string> calls;
    std::uint64_t frequency_hz = 0;
    double sample_rate_hz = 0;
    std::uint32_t bandwidth_hz = 0;
    bool amp = false;
    unsigned lna_db = 0;
    unsigned vga_db = 0;
    unsigned tx_vga_db = 0;
    bool bias = false;
    bool clock_out = false;
    bool external_clock = false;
    // The next start moves the board to another reference, as the firmware
    // does when CLKIN has come or gone; the start clears it.
    bool reference_switch_pending = false;
    bool hardware_sync = false;
    // Atomic, as the reader's thread polls it while the test sets it.
    std::atomic<std::uint32_t> shortfall_count{0};
    // Atomic, so a thread playing the air between two boards can watch them.
    // A board whose transfer thread has died reads as not streaming.
    std::atomic<bool> rx_streaming{false};
    std::atomic<bool> tx_streaming{false};
    // Gone from the bus: not streaming, and stream_health() and stop() fail
    // as libhackrf does on a lost device.
    std::atomic<bool> unplugged{false};
    // The board went idle by itself, as over its underrun limit.
    std::atomic<bool> stopped_itself{false};
    std::uint32_t tx_underrun_limit = 0;
    // False: firmware too old to take a limit.
    bool takes_underrun_limit = true;
    int starts = 0;
    int stops = 0;
    // Small, so test vectors stay readable: 8 samples.
    std::size_t transfer_bytes_value = 16;
    // The next call of this name throws BoardError.
    std::string fail_on;
    // Called with each call's "name value" as it is recorded, so a test
    // can look at the pair from inside one of its operations.
    std::function<void(const std::string&)> on_call;

private:
    void record(const std::string& name, const std::string& value);

    std::string serial_;
    int* stop_count_;
    RxHandler rx_;
    TxHandler tx_;
};

}  // namespace hackrfduplex::test
