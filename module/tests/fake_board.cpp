// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_board.hpp"

#include <stdexcept>

#include "bandwidth.hpp"

namespace hackrfduplex::test {

FakeBoard::FakeBoard(std::string serial, int* stop_count)
    : serial_(std::move(serial)), stop_count_(stop_count) {}

void FakeBoard::record(const std::string& name, const std::string& value) {
    calls.push_back(name + " " + value);
    if (on_call) {
        on_call(calls.back());
    }
    if (fail_on == name) {
        fail_on.clear();
        throw BoardError(name + ": HACKRF_ERROR_LIBUSB");
    }
}

BoardInfo FakeBoard::info() {
    record("info", "");
    BoardInfo info;
    info.serial = serial_;
    info.board = "HackRF One";
    info.revision = "r9";
    info.firmware = "2026.01.3";
    info.usb_api = "1.10";
    info.external_clock = external_clock;
    info.clock_out = clock_out;
    return info;
}

void FakeBoard::set_frequency(std::uint64_t hz) {
    record("set_frequency", std::to_string(hz));
    frequency_hz = hz;
}

void FakeBoard::set_sample_rate(double hz) {
    record("set_sample_rate", std::to_string(static_cast<std::uint64_t>(hz)));
    sample_rate_hz = hz;
    bandwidth_hz = auto_bandwidth(hz);
}

void FakeBoard::set_bandwidth(std::uint32_t hz) {
    record("set_bandwidth", std::to_string(hz));
    bandwidth_hz = hz;
}

void FakeBoard::set_amp(bool on) {
    record("set_amp", on ? "on" : "off");
    amp = on;
}

void FakeBoard::set_lna_gain(unsigned db) {
    record("set_lna_gain", std::to_string(db));
    lna_db = db;
}

void FakeBoard::set_vga_gain(unsigned db) {
    record("set_vga_gain", std::to_string(db));
    vga_db = db;
}

void FakeBoard::set_tx_vga_gain(unsigned db) {
    record("set_tx_vga_gain", std::to_string(db));
    tx_vga_db = db;
}

void FakeBoard::set_bias_tee(bool on) {
    record("set_bias_tee", on ? "on" : "off");
    bias = on;
}

void FakeBoard::set_clock_out(bool on) {
    record("set_clock_out", on ? "on" : "off");
    clock_out = on;
}

void FakeBoard::set_hardware_sync(bool on) {
    record("set_hardware_sync", on ? "on" : "off");
    hardware_sync = on;
}

void FakeBoard::start_rx(RxHandler handler) {
    record("start_rx", "");
    if (rx_streaming || tx_streaming) {
        throw BoardError("start_rx: already streaming");
    }
    rx_ = std::move(handler);
    rx_streaming = true;
    ++starts;
    reference_switch_pending = false;
}

void FakeBoard::start_tx(TxHandler handler) {
    record("start_tx", "");
    if (rx_streaming || tx_streaming) {
        throw BoardError("start_tx: already streaming");
    }
    tx_ = std::move(handler);
    tx_streaming = true;
    ++starts;
    reference_switch_pending = false;
}

void FakeBoard::stop() {
    record("stop", "");
    rx_streaming = false;
    tx_streaming = false;
    rx_ = nullptr;
    tx_ = nullptr;
    bias = false;
    ++stops;
    if (stop_count_ != nullptr) {
        ++*stop_count_;
    }
}

void FakeBoard::deliver_rx(std::span<const std::uint8_t> bytes) {
    if (!rx_streaming) {
        throw std::logic_error("deliver_rx: not receiving");
    }
    rx_(bytes);
}

std::vector<std::uint8_t> FakeBoard::pull_tx(std::size_t bytes) {
    if (!tx_streaming) {
        throw std::logic_error("pull_tx: not transmitting");
    }
    std::vector<std::uint8_t> buffer(bytes, 0xAA);
    tx_(buffer);
    return buffer;
}

}  // namespace hackrfduplex::test
