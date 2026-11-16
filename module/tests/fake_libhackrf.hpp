// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// libhackrf's C API, played by the test: hackrf_board.cpp is built against
// libhackrf's own header and linked with this instead of the library, so
// the binding runs as it does on the boards while the test sets what each
// board answers. The boards are the test's to describe; each call is
// recorded, and any can be made to fail once. Single-threaded: the test
// runs the transfer callbacks itself, as libhackrf's thread would.

#include <hackrf.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hackrfduplex::test::fake_hackrf {

struct Device {
    std::string serial;
    hackrf_usb_board_id usb_board_id = USB_BOARD_ID_HACKRF_ONE;
    // What the board reports.
    std::uint8_t board_id = BOARD_ID_HACKRF1_OG;
    std::uint8_t board_rev = BOARD_REV_HACKRF1_R10;
    std::uint16_t usb_api = 0x0110;
    std::string firmware = "2026.01.3";
    std::uint8_t clkin = 0;
    std::map<std::uint16_t, std::uint16_t> si5351;
    std::size_t transfer_size = 16;

    // What it was told.
    std::uint64_t frequency_hz = 0;
    double sample_rate_hz = 0;
    std::uint32_t bandwidth_hz = 0;
    std::uint8_t amp = 0;
    std::uint32_t lna_db = 0;
    std::uint32_t vga_db = 0;
    std::uint32_t tx_vga_db = 0;
    std::uint8_t antenna = 0;
    std::uint8_t clkout = 0;
    std::uint8_t hw_sync = 0;
    std::uint32_t tx_underrun_limit = 0;

    // Streaming: 0 none, 1 receive, 2 transmit, with the callback libhackrf
    // was handed.
    int mode = 0;
    hackrf_sample_block_cb_fn callback = nullptr;
    void* context = nullptr;
    // The next this many starts leave the transfer thread not running, as
    // a board needing a fresh handle does.
    int dead_starts = 0;
    bool thread_running = false;
    // The streaming engine: false keeps it where it is through a start or
    // a stop, as one that never takes them up.
    bool m0_follows = true;
    hackrf_m0_state m0{};

    bool open = false;
    int opens = 0;
    int closes = 0;
};

struct State {
    std::vector<Device> devices;
    int init_result = HACKRF_SUCCESS;
    int inits = 0;
    int exits = 0;
    // hackrf_device_list answers NULL.
    bool list_fails = false;
    // A device listed with no serial, as libhackrf lists one it cannot read.
    bool list_unreadable = false;
    // The next call of this function (any board) returns this.
    std::map<std::string, int> fail_next;
    // Every call, as "function serial".
    std::vector<std::string> calls;
};

// The one state, emptied by reset().
State& state();
void reset();
// The board with this serial; it must exist.
Device& device(const std::string& serial);

// The test as libhackrf's transfer thread: one receive transfer of `bytes`,
// of which `valid` are data. Returns the callback's answer.
int deliver_rx(const std::string& serial, std::vector<std::uint8_t>& bytes, int valid);
// One transmit transfer into `buffer`; `valid` gets the length the
// callback set.
int pull_tx(const std::string& serial, std::vector<std::uint8_t>& buffer, int& valid);

}  // namespace hackrfduplex::test::fake_hackrf
