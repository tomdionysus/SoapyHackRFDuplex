// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <stdexcept>

#include "bandwidth.hpp"

namespace hackrfduplex {

const char* direction_name(Direction direction) {
    return direction == Direction::kRx ? "RX" : "TX";
}

Pair::Pair(std::unique_ptr<Board> rx, std::unique_ptr<Board> tx, Log log) : log_(std::move(log)) {
    if (!rx || !tx) {
        throw std::invalid_argument("a pair needs two boards");
    }
    if (rx->serial() == tx->serial()) {
        throw std::invalid_argument("a pair needs two different boards, not " + rx->serial() +
                                    " twice");
    }
    rx_.board = std::move(rx);
    tx_.board = std::move(tx);
    apply_rx_gains();
    apply_tx_gains();
    this->log(LogLevel::kInfo,
              "pair opened: RX " + rx_.board->serial() + ", TX " + tx_.board->serial());
}

Pair::~Pair() {
    for (Direction direction : {Direction::kRx, Direction::kTx}) {
        try {
            close_stream(direction);
        } catch (const std::exception& e) {
            log(LogLevel::kWarning,
                std::string("stopping ") + direction_name(direction) + " failed: " + e.what());
        }
    }
}

Board& Pair::board(Direction direction) { return *side(direction).board; }

Pair::Side& Pair::side(Direction direction) { return direction == Direction::kRx ? rx_ : tx_; }

const Pair::Side& Pair::side(Direction direction) const {
    return direction == Direction::kRx ? rx_ : tx_;
}

void Pair::log(LogLevel level, const std::string& message) {
    if (log_) {
        log_(level, message);
    }
}

void Pair::set_frequency(Direction direction, std::uint64_t hz) {
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.board->set_frequency(hz);
    s.frequency_hz = hz;
}

std::uint64_t Pair::frequency(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.frequency_hz;
}

void Pair::set_sample_rate(Direction direction, double hz) {
    if (!(hz > 0)) {
        throw std::invalid_argument("sample rate must be positive");
    }
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.board->set_sample_rate(hz);
    s.sample_rate_hz = hz;
    if (s.bandwidth_hz != 0) {
        s.board->set_bandwidth(s.bandwidth_hz);
    }
}

double Pair::sample_rate(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.sample_rate_hz;
}

void Pair::set_bandwidth(Direction direction, std::uint32_t hz) {
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (hz == 0) {
        s.bandwidth_hz = 0;
        if (s.sample_rate_hz > 0) {
            s.board->set_bandwidth(auto_bandwidth(s.sample_rate_hz));
        }
        return;
    }
    const std::uint32_t chosen = round_down_bandwidth(hz);
    s.board->set_bandwidth(chosen);
    s.bandwidth_hz = chosen;
}

std::uint32_t Pair::bandwidth(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.bandwidth_hz != 0 ? s.bandwidth_hz : auto_bandwidth(s.sample_rate_hz);
}

bool Pair::bandwidth_is_auto(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.bandwidth_hz == 0;
}

void Pair::apply_rx_gains() {
    rx_.board->set_lna_gain(rx_gains_.lna_db);
    rx_.board->set_vga_gain(rx_gains_.vga_db);
    rx_.board->set_amp(rx_gains_.amp);
}

void Pair::apply_tx_gains() {
    tx_.board->set_tx_vga_gain(tx_gains_.vga_db);
    tx_.board->set_amp(tx_gains_.amp);
}

void Pair::set_rx_gains(RxGains gains) {
    const std::lock_guard<std::mutex> lock(rx_.mutex);
    rx_gains_.lna_db = gain::quantise(gains.lna_db, gain::kRxLnaMaxDb, gain::kRxLnaStepDb);
    rx_gains_.vga_db = gain::quantise(gains.vga_db, gain::kRxVgaMaxDb, gain::kRxVgaStepDb);
    rx_gains_.amp = gains.amp;
    apply_rx_gains();
}

RxGains Pair::rx_gains() const {
    const std::lock_guard<std::mutex> lock(rx_.mutex);
    return rx_gains_;
}

void Pair::set_tx_gains(TxGains gains) {
    const std::lock_guard<std::mutex> lock(tx_.mutex);
    tx_gains_.vga_db = gain::quantise(gains.vga_db, gain::kTxVgaMaxDb, gain::kTxVgaStepDb);
    tx_gains_.amp = gains.amp;
    apply_tx_gains();
}

TxGains Pair::tx_gains() const {
    const std::lock_guard<std::mutex> lock(tx_.mutex);
    return tx_gains_;
}

void Pair::set_overall_gain(Direction direction, double db) {
    if (direction == Direction::kRx) {
        const gain::RxSplit split = gain::split_rx(db);
        set_rx_gains(RxGains{split.lna_db, split.vga_db, split.amp});
    } else {
        const gain::TxSplit split = gain::split_tx(db);
        set_tx_gains(TxGains{split.vga_db, split.amp});
    }
}

double Pair::overall_gain(Direction direction) const {
    if (direction == Direction::kRx) {
        const RxGains g = rx_gains();
        return g.lna_db + g.vga_db + (g.amp ? gain::kAmpDb : 0);
    }
    const TxGains g = tx_gains();
    return g.vga_db + (g.amp ? gain::kAmpDb : 0);
}

void Pair::set_bias_tee(bool on) {
    const std::lock_guard<std::mutex> lock(tx_.mutex);
    tx_.board->set_bias_tee(on);
    bias_ = on;
}

void Pair::open_stream(Direction direction, Format format, std::size_t slots) {
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.open) {
        throw std::logic_error(std::string(direction_name(direction)) + " stream already open");
    }
    if (slots == 0) {
        throw std::invalid_argument("a stream needs at least one buffer");
    }
    if (direction == Direction::kRx) {
        rx_path_ = std::make_unique<RxPath>(slots, s.board->transfer_bytes());
    } else {
        tx_path_ = std::make_unique<TxPath>(slots, s.board->transfer_bytes());
    }
    s.format = format;
    s.open = true;
    log(LogLevel::kDebug, std::string(direction_name(direction)) + " stream opened: " +
                              format_name(format) + ", " + std::to_string(slots) + " buffers");
}

void Pair::close_stream(Direction direction) {
    deactivate(direction);
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.open) {
        return;
    }
    if (direction == Direction::kRx) {
        rx_path_.reset();
    } else {
        tx_path_.reset();
    }
    s.open = false;
}

bool Pair::stream_open(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.open;
}

void Pair::activate(Direction direction) {
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.open) {
        throw std::logic_error(std::string(direction_name(direction)) + " stream not open");
    }
    if (s.active) {
        return;
    }
    if (direction == Direction::kRx) {
        rx_path_->reset();
        RxPath* path = rx_path_.get();
        s.board->start_rx(
            [path](std::span<const std::uint8_t> bytes) { path->on_transfer(bytes); });
    } else {
        tx_path_->reset();
        if (bias_) {
            s.board->set_bias_tee(true);
        }
        TxPath* path = tx_path_.get();
        s.board->start_tx([path](std::span<std::uint8_t> buffer) { path->on_transfer(buffer); });
    }
    s.active = true;
    log(LogLevel::kDebug, std::string(direction_name(direction)) + " started");
}

void Pair::deactivate(Direction direction) {
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.active) {
        return;
    }
    s.board->stop();
    s.active = false;
    if (direction == Direction::kRx) {
        rx_path_->wake();
    } else {
        tx_path_->wake();
    }
    log(LogLevel::kDebug, std::string(direction_name(direction)) + " stopped");
}

bool Pair::active(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.active;
}

std::size_t Pair::mtu(Direction direction) const {
    return side(direction).board->transfer_bytes() / kNativeSampleBytes;
}

RxRead Pair::read(void* dst, std::size_t max_samples, std::chrono::microseconds timeout) {
    if (!rx_path_) {
        throw std::logic_error("RX stream not open");
    }
    return rx_path_->read(dst, max_samples, rx_.format, timeout);
}

std::size_t Pair::write(const void* src, std::size_t samples, bool end_of_burst,
                        std::chrono::microseconds timeout) {
    if (!tx_path_) {
        throw std::logic_error("TX stream not open");
    }
    return tx_path_->write(src, samples, tx_.format, end_of_burst, timeout);
}

bool Pair::take_tx_underflow() { return tx_path_ ? tx_path_->take_underflow() : false; }

std::uint64_t Pair::rx_received() const { return rx_path_ ? rx_path_->received() : 0; }
std::uint64_t Pair::rx_dropped() const { return rx_path_ ? rx_path_->dropped() : 0; }
std::uint64_t Pair::tx_sent() const { return tx_path_ ? tx_path_->sent() : 0; }
std::uint64_t Pair::tx_underflows() const { return tx_path_ ? tx_path_->underflows() : 0; }

}  // namespace hackrfduplex
