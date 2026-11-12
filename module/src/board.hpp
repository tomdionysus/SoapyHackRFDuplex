// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>

namespace hackrfduplex {

// A libhackrf call that failed, with hackrf_error_name's text.
class BoardError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A board seen on the bus, without opening it.
struct ListedBoard {
    std::string serial;  // 32 hex digits as libhackrf prints them
    std::string name;    // from the USB product id
};

struct BoardInfo {
    std::string serial;
    std::string board;     // hackrf_board_id_name
    std::string revision;  // hackrf_board_rev_name, or "unknown" on old firmware
    std::string firmware;  // version string
    std::string usb_api;   // "1.08"
    // The firmware reports a clock on CLKIN, so the board runs from it
    // rather than its own crystal. Firmware before USB API 1.06 cannot
    // say, and reports false.
    bool external_clock = false;
    // The board's CLKOUT is driving 10 MHz, read back from the clock
    // chip. Off at every power-up.
    bool clock_out = false;
};

struct StreamHealth {
    // Times since the stream started that the board's own buffer ran full
    // receiving or empty transmitting, each losing or delaying samples the
    // stream positions cannot see. The firmware counts from every start.
    std::uint32_t shortfalls = 0;
    // It went idle by itself: a shortfall outlasted its limit
    // (set_tx_underrun_limit).
    bool stopped_itself = false;
};

// Called on the board's transfer thread with each received transfer's
// bytes: interleaved int8 I/Q.
using RxHandler = std::function<void(std::span<const std::uint8_t>)>;
// Called on the board's transfer thread to fill a whole transfer.
using TxHandler = std::function<void(std::span<std::uint8_t>)>;

// One HackRF, behind an interface so the pair is tested against fakes.
// Every method but the handlers runs on the caller's thread; the
// settings take effect at once and persist across start and stop, except
// the bias tee, which the firmware drops whenever the board goes idle.
// Failures throw BoardError.
class Board {
public:
    virtual ~Board() = default;

    virtual std::string serial() const = 0;
    virtual BoardInfo info() = 0;

    virtual void set_frequency(std::uint64_t hz) = 0;
    // Also resets the baseband filter to the firmware's default for the rate.
    virtual void set_sample_rate(double hz) = 0;
    virtual void set_bandwidth(std::uint32_t hz) = 0;
    virtual void set_amp(bool on) = 0;
    virtual void set_lna_gain(unsigned db) = 0;
    virtual void set_vga_gain(unsigned db) = 0;
    virtual void set_tx_vga_gain(unsigned db) = 0;
    virtual void set_bias_tee(bool on) = 0;
    // 10 MHz on the CLKOUT connector, for another board's CLKIN.
    virtual void set_clock_out(bool on) = 0;
    // Whether the next stream start will move the board to another
    // reference: the firmware uses CLKIN when it sees a clock there and its
    // crystal otherwise, choosing at each start. Asked while idle, when the
    // clock chip reads reliably.
    virtual bool start_switches_reference() = 0;
    // With sync on, the next start waits for a rising edge on the trigger
    // input (P28 pin 16) before it moves a sample. A board's start drives
    // its own trigger output (P28 pin 15).
    virtual void set_hardware_sync(bool on) = 0;
    // What the board's own streaming engine reports; all clear on firmware
    // too old to say (USB API before 1.06).
    virtual StreamHealth stream_health() = 0;
    // From the next transmit start, the board idles itself once it has
    // gone `samples` samples without data in one stretch, so it stops when
    // its host does; 0 never. False when the firmware cannot (USB API
    // before 1.06). The firmware keeps the limit until it is set again.
    virtual bool set_tx_underrun_limit(std::uint32_t samples) = 0;

    // Bytes per transfer; every RX handler call carries at most this
    // many and every TX call exactly this many.
    virtual std::size_t transfer_bytes() const = 0;
    virtual void start_rx(RxHandler handler) = 0;
    virtual void start_tx(TxHandler handler) = 0;
    // After it returns no handler runs again.
    virtual void stop() = 0;
    virtual bool streaming() = 0;
};

}  // namespace hackrfduplex
