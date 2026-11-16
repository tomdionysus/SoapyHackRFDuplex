// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_libhackrf.hpp"

#include <algorithm>
#include <list>
#include <optional>
#include <stdexcept>

#include "m0.hpp"

// libhackrf's handle, opaque to its callers.
struct hackrf_device {
    std::string serial;
};

namespace hackrfduplex::test::fake_hackrf {

namespace {

// Handles live until reset(), so one closed is still safe to compare.
std::list<hackrf_device>& handles() {
    static std::list<hackrf_device> list;
    return list;
}

// Records the call and answers what the test set for it, if anything.
std::optional<int> called(const char* function, const std::string& serial = "") {
    State& s = state();
    s.calls.push_back(std::string(function) + (serial.empty() ? "" : " " + serial));
    const auto it = s.fail_next.find(function);
    if (it == s.fail_next.end()) {
        return std::nullopt;
    }
    const int result = it->second;
    s.fail_next.erase(it);
    return result;
}

Device& of(hackrf_device* handle) {
    if (handle == nullptr) {
        throw std::logic_error("libhackrf called with a null device");
    }
    Device& d = device(handle->serial);
    if (!d.open) {
        throw std::logic_error("libhackrf called with a closed device " + handle->serial);
    }
    return d;
}

// A call on a device that sets something: the failure the test chose, or
// success with the value stored.
template <typename T, typename U>
int set(const char* function, hackrf_device* handle, T Device::* field, U value) {
    Device& d = of(handle);
    if (const auto failed = called(function, d.serial)) {
        return *failed;
    }
    d.*field = static_cast<T>(value);
    return HACKRF_SUCCESS;
}

int start(const char* function, hackrf_device* handle, hackrf_sample_block_cb_fn callback,
          void* context, int mode) {
    Device& d = of(handle);
    if (const auto failed = called(function, d.serial)) {
        return *failed;
    }
    d.mode = mode;
    d.callback = callback;
    d.context = context;
    d.thread_running = d.dead_starts == 0;
    if (d.dead_starts > 0) {
        --d.dead_starts;
    }
    if (d.m0_follows) {
        d.m0.active_mode = mode == 1 ? kM0ModeRx : kM0ModeTxRun;
        d.m0.m0_count = 0;
        d.m0.num_shortfalls = 0;
        d.m0.error = 0;
    }
    return HACKRF_SUCCESS;
}

int stop(const char* function, hackrf_device* handle) {
    Device& d = of(handle);
    if (const auto failed = called(function, d.serial)) {
        return *failed;
    }
    d.mode = 0;
    d.callback = nullptr;
    d.context = nullptr;
    if (d.m0_follows) {
        d.m0.active_mode = kM0ModeIdle;
    }
    return HACKRF_SUCCESS;
}

}  // namespace

State& state() {
    static State s;
    return s;
}

void reset() {
    state() = State{};
    handles().clear();
}

Device& device(const std::string& serial) {
    for (Device& d : state().devices) {
        if (d.serial == serial) {
            return d;
        }
    }
    throw std::logic_error("no fake board " + serial);
}

int deliver_rx(const std::string& serial, std::vector<std::uint8_t>& bytes, int valid) {
    const Device& d = device(serial);
    if (d.mode != 1) {
        throw std::logic_error("deliver_rx: " + serial + " is not receiving");
    }
    hackrf_transfer transfer{};
    transfer.buffer = bytes.data();
    transfer.buffer_length = static_cast<int>(bytes.size());
    transfer.valid_length = valid;
    transfer.rx_ctx = d.context;
    return d.callback(&transfer);
}

int pull_tx(const std::string& serial, std::vector<std::uint8_t>& buffer, int& valid) {
    const Device& d = device(serial);
    if (d.mode != 2) {
        throw std::logic_error("pull_tx: " + serial + " is not transmitting");
    }
    hackrf_transfer transfer{};
    transfer.buffer = buffer.data();
    transfer.buffer_length = static_cast<int>(buffer.size());
    transfer.tx_ctx = d.context;
    const int result = d.callback(&transfer);
    valid = transfer.valid_length;
    return result;
}

}  // namespace hackrfduplex::test::fake_hackrf

using namespace hackrfduplex::test::fake_hackrf;

extern "C" {

int hackrf_init() {
    called("hackrf_init");
    ++state().inits;
    return state().init_result;
}

int hackrf_exit() {
    called("hackrf_exit");
    ++state().exits;
    return HACKRF_SUCCESS;
}

const char* hackrf_library_release() { return "fake-libhackrf"; }

const char* hackrf_error_name(enum hackrf_error errcode) {
    switch (errcode) {
        case HACKRF_SUCCESS:
            return "HACKRF_SUCCESS";
        case HACKRF_TRUE:
            return "HACKRF_TRUE";
        case HACKRF_ERROR_NOT_FOUND:
            return "HACKRF_ERROR_NOT_FOUND";
        case HACKRF_ERROR_LIBUSB:
            return "HACKRF_ERROR_LIBUSB";
        case HACKRF_ERROR_STREAMING_THREAD_ERR:
            return "HACKRF_ERROR_STREAMING_THREAD_ERR";
        case HACKRF_ERROR_STREAMING_EXIT_CALLED:
            return "HACKRF_ERROR_STREAMING_EXIT_CALLED";
        default:
            return "HACKRF_ERROR_OTHER";
    }
}

const char* hackrf_board_id_name(enum hackrf_board_id board_id) {
    switch (board_id) {
        case BOARD_ID_HACKRF1_OG:
            return "HackRF One";
        case BOARD_ID_HACKRF1_R9:
            return "HackRF One r9";
        default:
            return "unknown board id";
    }
}

const char* hackrf_usb_board_id_name(enum hackrf_usb_board_id usb_board_id) {
    return usb_board_id == USB_BOARD_ID_HACKRF_ONE ? "HackRF One" : "unknown usb board";
}

const char* hackrf_board_rev_name(enum hackrf_board_rev board_rev) {
    return board_rev == BOARD_REV_HACKRF1_R10 ? "r10" : "unknown revision";
}

hackrf_device_list_t* hackrf_device_list() {
    called("hackrf_device_list");
    State& s = state();
    if (s.list_fails) {
        return nullptr;
    }
    auto* list = new hackrf_device_list_t{};
    const std::size_t n = s.devices.size() + (s.list_unreadable ? 1 : 0);
    list->devicecount = static_cast<int>(n);
    list->serial_numbers = new char*[n];
    list->usb_board_ids = new hackrf_usb_board_id[n];
    for (std::size_t i = 0; i < s.devices.size(); ++i) {
        const std::string& serial = s.devices[i].serial;
        list->serial_numbers[i] = new char[serial.size() + 1];
        std::copy_n(serial.c_str(), serial.size() + 1, list->serial_numbers[i]);
        list->usb_board_ids[i] = s.devices[i].usb_board_id;
    }
    if (s.list_unreadable) {
        list->serial_numbers[n - 1] = nullptr;
        list->usb_board_ids[n - 1] = USB_BOARD_ID_HACKRF_ONE;
    }
    return list;
}

void hackrf_device_list_free(hackrf_device_list_t* list) {
    called("hackrf_device_list_free");
    for (int i = 0; i < list->devicecount; ++i) {
        delete[] list->serial_numbers[i];
    }
    delete[] list->serial_numbers;
    delete[] list->usb_board_ids;
    delete list;
}

int hackrf_open_by_serial(const char* const desired_serial_number, hackrf_device** device) {
    const std::string wanted = desired_serial_number;
    if (const auto failed = called("hackrf_open_by_serial", wanted)) {
        return *failed;
    }
    for (Device& d : state().devices) {
        if (d.serial.ends_with(wanted)) {
            d.open = true;
            ++d.opens;
            handles().push_back(hackrf_device{d.serial});
            *device = &handles().back();
            return HACKRF_SUCCESS;
        }
    }
    return HACKRF_ERROR_NOT_FOUND;
}

int hackrf_close(hackrf_device* device) {
    Device& d = of(device);
    called("hackrf_close", d.serial);
    d.open = false;
    d.mode = 0;
    ++d.closes;
    return HACKRF_SUCCESS;
}

size_t hackrf_get_transfer_buffer_size(hackrf_device* device) { return of(device).transfer_size; }

int hackrf_usb_api_version_read(hackrf_device* device, uint16_t* version) {
    const Device& d = of(device);
    if (const auto failed = called("hackrf_usb_api_version_read", d.serial)) {
        return *failed;
    }
    *version = d.usb_api;
    return HACKRF_SUCCESS;
}

int hackrf_board_id_read(hackrf_device* device, uint8_t* value) {
    const Device& d = of(device);
    if (const auto failed = called("hackrf_board_id_read", d.serial)) {
        return *failed;
    }
    *value = d.board_id;
    return HACKRF_SUCCESS;
}

int hackrf_board_rev_read(hackrf_device* device, uint8_t* value) {
    const Device& d = of(device);
    if (const auto failed = called("hackrf_board_rev_read", d.serial)) {
        return *failed;
    }
    *value = d.board_rev;
    return HACKRF_SUCCESS;
}

int hackrf_version_string_read(hackrf_device* device, char* version, uint8_t length) {
    Device& d = of(device);
    if (const auto failed = called("hackrf_version_string_read", d.serial)) {
        return *failed;
    }
    // As libhackrf: at most `length` characters, the terminator the caller's.
    const std::size_t n = std::min<std::size_t>(d.firmware.size(), length);
    std::copy_n(d.firmware.data(), n, version);
    return HACKRF_SUCCESS;
}

int hackrf_get_clkin_status(hackrf_device* device, uint8_t* status) {
    const Device& d = of(device);
    if (const auto failed = called("hackrf_get_clkin_status", d.serial)) {
        return *failed;
    }
    *status = d.clkin;
    return HACKRF_SUCCESS;
}

int hackrf_si5351c_read(hackrf_device* device, uint16_t register_number, uint16_t* value) {
    Device& d = of(device);
    if (const auto failed = called("hackrf_si5351c_read", d.serial)) {
        return *failed;
    }
    const auto it = d.si5351.find(register_number);
    *value = it == d.si5351.end() ? 0 : it->second;
    return HACKRF_SUCCESS;
}

int hackrf_set_freq(hackrf_device* device, const uint64_t freq_hz) {
    return set("hackrf_set_freq", device, &Device::frequency_hz, freq_hz);
}

int hackrf_set_sample_rate(hackrf_device* device, const double freq_hz) {
    return set("hackrf_set_sample_rate", device, &Device::sample_rate_hz, freq_hz);
}

int hackrf_set_baseband_filter_bandwidth(hackrf_device* device, const uint32_t bandwidth_hz) {
    return set("hackrf_set_baseband_filter_bandwidth", device, &Device::bandwidth_hz, bandwidth_hz);
}

int hackrf_set_amp_enable(hackrf_device* device, const uint8_t value) {
    return set("hackrf_set_amp_enable", device, &Device::amp, value);
}

int hackrf_set_lna_gain(hackrf_device* device, uint32_t value) {
    return set("hackrf_set_lna_gain", device, &Device::lna_db, value);
}

int hackrf_set_vga_gain(hackrf_device* device, uint32_t value) {
    return set("hackrf_set_vga_gain", device, &Device::vga_db, value);
}

int hackrf_set_txvga_gain(hackrf_device* device, uint32_t value) {
    return set("hackrf_set_txvga_gain", device, &Device::tx_vga_db, value);
}

int hackrf_set_antenna_enable(hackrf_device* device, const uint8_t value) {
    return set("hackrf_set_antenna_enable", device, &Device::antenna, value);
}

int hackrf_set_clkout_enable(hackrf_device* device, const uint8_t value) {
    return set("hackrf_set_clkout_enable", device, &Device::clkout, value);
}

int hackrf_set_hw_sync_mode(hackrf_device* device, const uint8_t value) {
    return set("hackrf_set_hw_sync_mode", device, &Device::hw_sync, value);
}

int hackrf_set_tx_underrun_limit(hackrf_device* device, uint32_t value) {
    return set("hackrf_set_tx_underrun_limit", device, &Device::tx_underrun_limit, value);
}

int hackrf_get_m0_state(hackrf_device* device, hackrf_m0_state* value) {
    const Device& d = of(device);
    if (const auto failed = called("hackrf_get_m0_state", d.serial)) {
        return *failed;
    }
    *value = d.m0;
    return HACKRF_SUCCESS;
}

int hackrf_start_rx(hackrf_device* device, hackrf_sample_block_cb_fn callback, void* rx_ctx) {
    return start("hackrf_start_rx", device, callback, rx_ctx, 1);
}

int hackrf_start_tx(hackrf_device* device, hackrf_sample_block_cb_fn callback, void* tx_ctx) {
    return start("hackrf_start_tx", device, callback, tx_ctx, 2);
}

int hackrf_stop_rx(hackrf_device* device) { return stop("hackrf_stop_rx", device); }

int hackrf_stop_tx(hackrf_device* device) { return stop("hackrf_stop_tx", device); }

int hackrf_is_streaming(hackrf_device* device) {
    const Device& d = of(device);
    if (const auto failed = called("hackrf_is_streaming", d.serial)) {
        return *failed;
    }
    if (d.mode == 0) {
        return HACKRF_ERROR_STREAMING_EXIT_CALLED;
    }
    return d.thread_running ? HACKRF_TRUE : HACKRF_ERROR_STREAMING_THREAD_ERR;
}

}  // extern "C"
