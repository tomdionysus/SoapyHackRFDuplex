// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <hackrf.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "board.hpp"

namespace hackrfduplex {

// hackrf_init and hackrf_exit, reference counted across the process so
// several pairs and a discovery scan can overlap.
class HackrfSession {
public:
    HackrfSession();
    ~HackrfSession();
    HackrfSession(const HackrfSession&) = delete;
    HackrfSession& operator=(const HackrfSession&) = delete;
};

// Every HackRF on the bus, without opening any of them, so boards a
// pair holds open are listed too. Serial order.
std::vector<ListedBoard> list_boards();

// libhackrf's release string.
std::string libhackrf_version();

// One HackRF through libhackrf. Control calls are serialised on one
// mutex; the transfer handlers run on libhackrf's thread for the board.
class HackrfBoard final : public Board {
public:
    // Opens the board whose serial ends in `serial`. Throws BoardError.
    explicit HackrfBoard(const std::string& serial);
    ~HackrfBoard() override;

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
    bool start_switches_reference() override;
    void set_hardware_sync(bool on) override;
    std::uint32_t shortfalls() override;

    std::size_t transfer_bytes() const override { return transfer_bytes_; }
    void start_rx(RxHandler handler) override;
    void start_tx(TxHandler handler) override;
    void stop() override;
    bool streaming() override;

private:
    static int rx_trampoline(hackrf_transfer* transfer);
    static int tx_trampoline(hackrf_transfer* transfer);
    void open();
    void close();
    // Starts with `start`, and once more after a close and reopen if the
    // board did not stream: libhackrf has refused a restart that way.
    void start(int (*start)(hackrf_device*, hackrf_sample_block_cb_fn, void*),
               hackrf_sample_block_cb_fn callback, const char* what);
    static void check(int result, const char* what);

    HackrfSession session_;
    std::string serial_;
    std::mutex mutex_;
    hackrf_device* device_ = nullptr;
    std::size_t transfer_bytes_ = 0;
    // Firmware with USB API 1.06 reports CLKIN status and board revision.
    bool reports_clkin_ = false;
    // Boards with id HACKRF1_R9 select CLKIN with a switch in front of
    // PLL A rather than by moving the outputs to PLL B.
    bool r9_clocking_ = false;
    RxHandler rx_;
    TxHandler tx_;
    bool streaming_ = false;
};

}  // namespace hackrfduplex
