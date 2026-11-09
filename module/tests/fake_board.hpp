// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
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
    std::size_t transfer_bytes() const override { return transfer_bytes_value; }
    void start_rx(RxHandler handler) override;
    void start_tx(TxHandler handler) override;
    void stop() override;
    bool streaming() override { return rx_streaming || tx_streaming; }

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
    bool rx_streaming = false;
    bool tx_streaming = false;
    int starts = 0;
    int stops = 0;
    // Small, so test vectors stay readable: 8 samples.
    std::size_t transfer_bytes_value = 16;
    // The next call of this name throws BoardError.
    std::string fail_on;

private:
    void record(const std::string& name, const std::string& value);

    std::string serial_;
    int* stop_count_;
    RxHandler rx_;
    TxHandler tx_;
};

}  // namespace hackrfduplex::test
