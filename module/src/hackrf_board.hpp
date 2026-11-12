// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <hackrf.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "board.hpp"
#include "m0.hpp"
#include "mutex.hpp"

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
    StreamHealth stream_health() override;
    bool set_tx_underrun_limit(std::uint32_t samples) override;

    std::size_t transfer_bytes() const override { return transfer_bytes_; }
    void start_rx(RxHandler handler) override;
    void start_tx(TxHandler handler) override;
    void stop() override;
    bool streaming() override;

private:
    static int rx_trampoline(hackrf_transfer* transfer);
    static int tx_trampoline(hackrf_transfer* transfer);
    void open() REQUIRES(mutex_);
    void close() REQUIRES(mutex_);
    // The libhackrf handle, or BoardError naming the board when a reopen
    // failed and left none, so a call never hands libhackrf a null device.
    hackrf_device* handle() REQUIRES(mutex_);
    // Starts with `begin`, and once more after a close and reopen if the
    // board did not stream: libhackrf has refused a restart that way.
    void start(int (*begin)(hackrf_device*, hackrf_sample_block_cb_fn, void*),
               hackrf_sample_block_cb_fn callback, const char* what) REQUIRES(mutex_);
    static void check(int result, const char* what);
    // The M0's state; the caller holds mutex_.
    M0Reading read_m0() REQUIRES(mutex_);
    // Reads the M0 until `done`, for at most 500 ms, then throws BoardError
    // with `what`. The caller holds mutex_.
    void await_m0(const std::function<bool(const M0Reading&)>& done, const std::string& what)
        REQUIRES(mutex_);

    HackrfSession session_;
    std::string serial_;
    Mutex mutex_;
    hackrf_device* device_ GUARDED_BY(mutex_) = nullptr;
    std::size_t transfer_bytes_ = 0;
    // Firmware with USB API 1.06 reports CLKIN status, board revision and
    // its M0 state, and takes a transmit underrun limit.
    bool reports_clkin_ = false;
    // Boards with id HACKRF1_R9 select CLKIN with a switch in front of
    // PLL A rather than by moving the outputs to PLL B.
    bool r9_clocking_ = false;
    // Set before a start and cleared after the stop, which joins
    // libhackrf's thread; that thread calls them without the lock.
    RxHandler rx_;
    TxHandler tx_;
    bool streaming_ GUARDED_BY(mutex_) = false;
};

}  // namespace hackrfduplex
