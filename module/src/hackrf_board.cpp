// SPDX-License-Identifier: GPL-3.0-or-later
#include "hackrf_board.hpp"

#include <algorithm>
#include <cstdio>

namespace hackrfduplex {

namespace {

std::mutex g_session_mutex;
unsigned g_sessions = 0;

// Si5351C register 19 is CLK3's control byte; CLK3 is CLKOUT on the
// HackRF One and bit 7 powers it down.
constexpr std::uint16_t kSi5351Clk3Control = 19;
constexpr std::uint16_t kSi5351PowerDown = 0x80;
constexpr std::uint16_t kSi5351Clk0Control = 16;
constexpr std::uint16_t kSi5351PllSourceB = 0x20;
// MSNA_P1 bits 15..8: 0x0E for 25 MHz in, 0x26 for 10 MHz in (si5351c.c).
constexpr std::uint16_t kSi5351PllAMultiplier = 29;
constexpr std::uint16_t kSi5351PllAMultiplierClkin = 0x26;

}  // namespace

HackrfSession::HackrfSession() {
    const std::lock_guard<std::mutex> lock(g_session_mutex);
    if (g_sessions == 0) {
        const int result = hackrf_init();
        if (result != HACKRF_SUCCESS) {
            throw BoardError(std::string("hackrf_init: ") +
                             hackrf_error_name(static_cast<hackrf_error>(result)));
        }
    }
    ++g_sessions;
}

HackrfSession::~HackrfSession() {
    const std::lock_guard<std::mutex> lock(g_session_mutex);
    if (--g_sessions == 0) {
        hackrf_exit();
    }
}

std::vector<ListedBoard> list_boards() {
    const HackrfSession session;
    std::vector<ListedBoard> boards;
    hackrf_device_list_t* list = hackrf_device_list();
    if (list == nullptr) {
        return boards;
    }
    for (int i = 0; i < list->devicecount; ++i) {
        if (list->serial_numbers[i] == nullptr) {
            continue;
        }
        boards.push_back(
            ListedBoard{list->serial_numbers[i], hackrf_usb_board_id_name(list->usb_board_ids[i])});
    }
    hackrf_device_list_free(list);
    std::sort(boards.begin(), boards.end(),
              [](const ListedBoard& a, const ListedBoard& b) { return a.serial < b.serial; });
    return boards;
}

std::string libhackrf_version() { return hackrf_library_release(); }

void HackrfBoard::check(int result, const char* what) {
    if (result != HACKRF_SUCCESS) {
        throw BoardError(std::string(what) + ": " +
                         hackrf_error_name(static_cast<hackrf_error>(result)));
    }
}

HackrfBoard::HackrfBoard(const std::string& serial) : serial_(serial) {
    open();
    transfer_bytes_ = hackrf_get_transfer_buffer_size(device_);
    std::uint16_t api = 0;
    check(hackrf_usb_api_version_read(device_, &api), "usb api version");
    reports_clkin_ = api >= 0x0106;
    // The firmware picks its clocking by board id, not revision: a board
    // that reports revision r10 but id HACKRF1_OG (as clones do) is clocked
    // the older way.
    std::uint8_t board_id = BOARD_ID_UNDETECTED;
    check(hackrf_board_id_read(device_, &board_id), "board id");
    r9_clocking_ = board_id == BOARD_ID_HACKRF1_R9;
}

HackrfBoard::~HackrfBoard() {
    try {
        stop();
    } catch (const BoardError&) {
    }
    close();
}

void HackrfBoard::open() {
    const std::string what = "open " + serial_;
    check(hackrf_open_by_serial(serial_.c_str(), &device_), what.c_str());
}

void HackrfBoard::close() {
    if (device_ != nullptr) {
        hackrf_close(device_);
        device_ = nullptr;
    }
}

BoardInfo HackrfBoard::info() {
    const std::lock_guard<std::mutex> lock(mutex_);
    BoardInfo info;
    info.serial = serial_;

    std::uint8_t board_id = BOARD_ID_UNDETECTED;
    check(hackrf_board_id_read(device_, &board_id), "board id");
    info.board = hackrf_board_id_name(static_cast<hackrf_board_id>(board_id));

    char version[64] = {};
    check(hackrf_version_string_read(device_, version, sizeof(version) - 1), "version");
    info.firmware = version;

    std::uint16_t api = 0;
    check(hackrf_usb_api_version_read(device_, &api), "usb api version");
    char api_text[16];
    std::snprintf(api_text, sizeof(api_text), "%x.%02x", (api >> 8) & 0xff, api & 0xff);
    info.usb_api = api_text;

    info.revision = "unknown";
    if (api >= 0x0106) {
        std::uint8_t revision = BOARD_REV_UNDETECTED;
        if (hackrf_board_rev_read(device_, &revision) == HACKRF_SUCCESS) {
            info.revision = hackrf_board_rev_name(static_cast<hackrf_board_rev>(revision));
        }
        std::uint8_t clkin = 0;
        if (hackrf_get_clkin_status(device_, &clkin) == HACKRF_SUCCESS) {
            info.external_clock = clkin != 0;
        }
    }

    std::uint16_t clk3 = kSi5351PowerDown;
    if (hackrf_si5351c_read(device_, kSi5351Clk3Control, &clk3) == HACKRF_SUCCESS) {
        info.clock_out = (clk3 & kSi5351PowerDown) == 0;
    }
    return info;
}

void HackrfBoard::set_frequency(std::uint64_t hz) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_freq(device_, hz), "set frequency");
}

void HackrfBoard::set_sample_rate(double hz) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_sample_rate(device_, hz), "set sample rate");
}

void HackrfBoard::set_bandwidth(std::uint32_t hz) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_baseband_filter_bandwidth(device_, hz), "set bandwidth");
}

void HackrfBoard::set_amp(bool on) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_amp_enable(device_, on ? 1 : 0), "set amp");
}

void HackrfBoard::set_lna_gain(unsigned db) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_lna_gain(device_, db), "set lna gain");
}

void HackrfBoard::set_vga_gain(unsigned db) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_vga_gain(device_, db), "set vga gain");
}

void HackrfBoard::set_tx_vga_gain(unsigned db) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_txvga_gain(device_, db), "set tx vga gain");
}

void HackrfBoard::set_bias_tee(bool on) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_antenna_enable(device_, on ? 1 : 0), "set bias tee");
}

void HackrfBoard::set_clock_out(bool on) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_clkout_enable(device_, on ? 1 : 0), "set clock out");
}

bool HackrfBoard::start_switches_reference() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!reports_clkin_) {
        return false;
    }
    std::uint8_t clkin = 0;
    check(hackrf_get_clkin_status(device_, &clkin), "clkin status");
    // What the board runs on now. Before r9 the firmware moves the outputs
    // between PLL A (crystal) and PLL B (CLKIN), which shows in CLK0's
    // control byte. From r9 it keeps PLL A and sets its multiplier for a
    // 10 MHz or a 25 MHz input.
    bool on_clkin = false;
    if (r9_clocking_) {
        std::uint16_t multiplier = 0;
        check(hackrf_si5351c_read(device_, kSi5351PllAMultiplier, &multiplier),
              "read pll multiplier");
        on_clkin = multiplier == kSi5351PllAMultiplierClkin;
    } else {
        std::uint16_t clk0 = 0;
        check(hackrf_si5351c_read(device_, kSi5351Clk0Control, &clk0), "read clock control");
        on_clkin = (clk0 & kSi5351PllSourceB) != 0;
    }
    return on_clkin != (clkin != 0);
}

void HackrfBoard::set_hardware_sync(bool on) {
    const std::lock_guard<std::mutex> lock(mutex_);
    check(hackrf_set_hw_sync_mode(device_, on ? 1 : 0), "set hardware sync");
}

std::uint32_t HackrfBoard::shortfalls() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!reports_clkin_) {
        return 0;
    }
    hackrf_m0_state state{};
    check(hackrf_get_m0_state(device_, &state), "read m0 state");
    return state.num_shortfalls;
}

int HackrfBoard::rx_trampoline(hackrf_transfer* transfer) {
    auto* self = static_cast<HackrfBoard*>(transfer->rx_ctx);
    if (transfer->valid_length > 0) {
        self->rx_(std::span<const std::uint8_t>(transfer->buffer,
                                                static_cast<std::size_t>(transfer->valid_length)));
    }
    return 0;
}

int HackrfBoard::tx_trampoline(hackrf_transfer* transfer) {
    auto* self = static_cast<HackrfBoard*>(transfer->tx_ctx);
    self->tx_(std::span<std::uint8_t>(transfer->buffer,
                                      static_cast<std::size_t>(transfer->buffer_length)));
    transfer->valid_length = transfer->buffer_length;
    return 0;
}

void HackrfBoard::start(int (*begin)(hackrf_device*, hackrf_sample_block_cb_fn, void*),
                        hackrf_sample_block_cb_fn callback, const char* what) {
    check(begin(device_, callback, this), what);
    if (hackrf_is_streaming(device_) == HACKRF_TRUE) {
        streaming_ = true;
        return;
    }
    // The board accepted the command but its transfer thread is not
    // running. A fresh handle has always cured this.
    hackrf_stop_rx(device_);
    hackrf_stop_tx(device_);
    close();
    open();
    check(begin(device_, callback, this), what);
    const int state = hackrf_is_streaming(device_);
    if (state != HACKRF_TRUE) {
        throw BoardError(std::string(what) + ": board did not start streaming (" +
                         hackrf_error_name(static_cast<hackrf_error>(state)) + ")");
    }
    streaming_ = true;
}

void HackrfBoard::start_rx(RxHandler handler) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (streaming_) {
        throw BoardError("start rx: already streaming");
    }
    rx_ = std::move(handler);
    start(hackrf_start_rx, &HackrfBoard::rx_trampoline, "start rx");
}

void HackrfBoard::start_tx(TxHandler handler) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (streaming_) {
        throw BoardError("start tx: already streaming");
    }
    tx_ = std::move(handler);
    start(hackrf_start_tx, &HackrfBoard::tx_trampoline, "start tx");
}

void HackrfBoard::stop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!streaming_ || device_ == nullptr) {
        return;
    }
    // Both stops are harmless on a board in the other mode, and after
    // either returns libhackrf has joined its transfer thread, so no
    // handler runs again.
    const int rx = hackrf_stop_rx(device_);
    const int tx = hackrf_stop_tx(device_);
    streaming_ = false;
    rx_ = nullptr;
    tx_ = nullptr;
    if (rx != HACKRF_SUCCESS && tx != HACKRF_SUCCESS) {
        check(rx, "stop");
    }
}

bool HackrfBoard::streaming() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return streaming_ && hackrf_is_streaming(device_) == HACKRF_TRUE;
}

}  // namespace hackrfduplex
