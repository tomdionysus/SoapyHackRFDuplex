// SPDX-License-Identifier: GPL-3.0-or-later
#include "pair.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>
#include <vector>

#include "bandwidth.hpp"
#include "burst.hpp"

namespace hackrfduplex {

namespace {

// Where a board is tuned briefly after its stream starts, before going
// back to its frequency (Pair::activate).
constexpr std::uint64_t kRetuneStepHz = 1'000'000;

// How often a running board's health and shortfall count are read, and
// so the longest a blocked read or write waits before looking again.
constexpr auto kBoardPoll = std::chrono::milliseconds(100);
// The longest shortfalls go unlogged after the first, while they recur.
constexpr auto kShortfallLog = std::chrono::seconds(1);
// How long a trigger start may take to reach the transmitter.
constexpr auto kTriggerWait = std::chrono::milliseconds(500);
// libhackrf keeps this many transfers in flight in each direction.
constexpr std::size_t kLibhackrfTransfers = 4;

Direction opposite(Direction direction) {
    return direction == Direction::kRx ? Direction::kTx : Direction::kRx;
}

// Receive samples captured during calibration, from the first transfer
// after it starts listening until it holds enough.
class Capture {
public:
    explicit Capture(std::size_t samples) : bytes_(samples * kNativeSampleBytes) {}

    void add(std::uint64_t position, std::span<const std::uint8_t> bytes) {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (filled_ == 0) {
            start_ = position;
        }
        const std::size_t take = std::min(bytes.size(), bytes_.size() - filled_);
        std::copy_n(bytes.begin(), take, bytes_.begin() + static_cast<long>(filled_));
        filled_ += take;
        if (filled_ == bytes_.size()) {
            full_.notify_all();
        }
    }

    // True when full within `timeout`.
    bool wait(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return full_.wait_for(lock, timeout, [&] { return filled_ == bytes_.size(); });
    }

    // After the tap is removed.
    std::span<const std::int8_t> samples() const {
        return {reinterpret_cast<const std::int8_t*>(bytes_.data()), filled_};
    }
    std::uint64_t start() const { return start_; }

private:
    std::mutex mutex_;
    std::condition_variable full_;
    std::vector<std::uint8_t> bytes_;
    std::size_t filled_ = 0;
    std::uint64_t start_ = 0;
};

}  // namespace

const char* direction_name(Direction direction) {
    return direction == Direction::kRx ? "RX" : "TX";
}

const char* clock_out_name(ClockOut which) {
    switch (which) {
        case ClockOut::kOff:
            return "off";
        case ClockOut::kRx:
            return "rx";
        case ClockOut::kTx:
            return "tx";
    }
    return "?";
}

const char* time_origin_name(TimeOrigin origin) {
    switch (origin) {
        case TimeOrigin::kBurst:
            return "burst";
        case TimeOrigin::kTrigger:
            return "trigger";
        case TimeOrigin::kNone:
            return "none";
    }
    return "?";
}

TimeOrigin parse_time_origin(const std::string& name) {
    if (name == "burst") {
        return TimeOrigin::kBurst;
    }
    if (name == "trigger") {
        return TimeOrigin::kTrigger;
    }
    if (name == "none") {
        return TimeOrigin::kNone;
    }
    throw std::invalid_argument("time origin must be burst, trigger or none, not " + name);
}

ClockOut parse_clock_out(const std::string& name) {
    if (name == "off") {
        return ClockOut::kOff;
    }
    if (name == "rx") {
        return ClockOut::kRx;
    }
    if (name == "tx") {
        return ClockOut::kTx;
    }
    throw std::invalid_argument("clock out must be off, rx or tx, not " + name);
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
    if (s.active) {
        lose_origin(std::string(direction_name(direction)) + " sample rate changed");
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

void Pair::set_tx_underrun_limit(std::uint32_t samples) {
    const std::lock_guard<std::mutex> lock(tx_.mutex);
    tx_underrun_limit_ = samples;
}

std::uint32_t Pair::tx_underrun_limit() const {
    const std::lock_guard<std::mutex> lock(tx_.mutex);
    return tx_underrun_limit_.value_or(
        static_cast<std::uint32_t>(tx_.board->transfer_bytes() / kNativeSampleBytes));
}

void Pair::set_clock_out(ClockOut which) {
    const std::scoped_lock lock(rx_.mutex, tx_.mutex);
    if (which != ClockOut::kRx) {
        rx_.board->set_clock_out(false);
    }
    if (which != ClockOut::kTx) {
        tx_.board->set_clock_out(false);
    }
    if (which == ClockOut::kRx) {
        rx_.board->set_clock_out(true);
    } else if (which == ClockOut::kTx) {
        tx_.board->set_clock_out(true);
    }
    rx_.drives_clkout = which == ClockOut::kRx;
    tx_.drives_clkout = which == ClockOut::kTx;
    log(LogLevel::kInfo, std::string("clock out: ") + clock_out_name(which));
}

ClockOut Pair::clock_out() {
    const std::scoped_lock lock(rx_.mutex, tx_.mutex);
    if (rx_.board->info().clock_out) {
        return ClockOut::kRx;
    }
    if (tx_.board->info().clock_out) {
        return ClockOut::kTx;
    }
    return ClockOut::kOff;
}

void Pair::open_stream(Direction direction, Format format, std::size_t slots) {
    const std::lock_guard<std::mutex> control(control_mutex_);
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
    const std::lock_guard<std::mutex> control(control_mutex_);
    deactivate_locked(direction);
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

void Pair::start_stream(Direction direction) {
    Side& s = side(direction);
    s.failed = false;
    s.shortfall_pending = false;
    {
        // The firmware counts shortfalls from each start.
        const std::lock_guard<std::mutex> lock(poll_mutex_);
        s.shortfalls_seen = 0;
        s.shortfalls_logged = 0;
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
        const std::uint32_t limit = tx_underrun_limit_.value_or(
            static_cast<std::uint32_t>(s.board->transfer_bytes() / kNativeSampleBytes));
        if (!s.board->set_tx_underrun_limit(limit) && limit != 0) {
            log(LogLevel::kWarning,
                "the TX board's firmware takes no underrun limit (USB API before 1.06): it "
                "will transmit on if this process dies while it streams");
        }
        TxPath* path = tx_path_.get();
        s.board->start_tx([path](std::span<std::uint8_t> buffer) { path->on_transfer(buffer); });
    }
    s.running = true;
}

void Pair::note_reference(Direction direction) {
    Side& s = side(direction);
    const BoardInfo info = s.board->info();
    s.on_clkin = info.external_clock;
    s.drives_clkout = info.clock_out;
}

bool Pair::settle_reference(Direction direction) {
    // The firmware chooses its clock source (CLKIN or its crystal) at every
    // stream start, under RF synthesisers tuned on whatever reference came
    // before. When that switches the reference they do not relock: the
    // board goes deaf, or transmits nothing. So when the reference changes,
    // retune on the new one and start the stream again on a steady clock.
    // The firmware skips a tune to the frequency it last applied, hence the
    // step away and back. Retuning when nothing switched is not harmless:
    // it left the r10 board's receiver 1.7 kHz off, so it is done only on
    // a switch.
    Side& s = side(direction);
    if (!s.board->start_switches_reference()) {
        return false;
    }
    // Stays false if handling the switch fails: the synthesisers are then
    // on a reference that has gone.
    s.lo_locked = false;
    start_stream(direction);
    if (s.frequency_hz != 0) {
        s.board->set_frequency(s.frequency_hz + kRetuneStepHz);
        s.board->set_frequency(s.frequency_hz);
    }
    s.running = false;
    s.board->stop();
    log(LogLevel::kInfo, std::string(direction_name(direction)) +
                             " board changed reference clock; retuned and restarted");
    return true;
}

void Pair::activate(Direction direction) {
    const std::lock_guard<std::mutex> control(control_mutex_);
    Side& s = side(direction);
    Side& o = side(opposite(direction));
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.open) {
            throw std::logic_error(std::string(direction_name(direction)) + " stream not open");
        }
        if (s.active) {
            return;
        }
    }
    bool other_active = false;
    bool other_armed = false;
    {
        const std::lock_guard<std::mutex> lock(o.mutex);
        other_active = o.active;
        other_armed = o.armed;
    }
    const TimeOrigin mode = time_origin();

    if (mode == TimeOrigin::kTrigger && (!other_active || other_armed)) {
        {
            const std::lock_guard<std::mutex> lock(s.mutex);
            note_reference(direction);
            settle_reference(direction);
            s.lo_locked = true;
            if (!other_active) {
                // The first of the two: both start on one edge with the second.
                s.active = true;
                s.armed = true;
                set_origin(OriginState::kPending);
                log(LogLevel::kDebug,
                    std::string(direction_name(direction)) + " armed for a trigger start");
                return;
            }
        }
        start_triggered();
        return;
    }

    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        note_reference(direction);
        settle_reference(direction);
        start_stream(direction);
        s.lo_locked = true;
        s.active = true;
        log(LogLevel::kDebug, std::string(direction_name(direction)) + " started");
    }
    if (!other_active) {
        set_origin(mode == TimeOrigin::kNone ? OriginState::kAbsent : OriginState::kPending);
        return;
    }
    switch (mode) {
        case TimeOrigin::kBurst:
            calibrate_locked();
            break;
        case TimeOrigin::kTrigger:
            log(LogLevel::kWarning,
                "trigger time origin: the other stream was already running, so the two did not "
                "start together; no common time");
            set_origin(OriginState::kAbsent);
            break;
        case TimeOrigin::kNone:
            set_origin(OriginState::kAbsent);
            break;
    }
}

void Pair::start_triggered() {
    // The transmitter waits in sync mode for an edge; the receiver's start
    // drives its trigger output, which a jumper takes to the transmitter's
    // trigger input. Both then start on the same sample clock edge, so
    // transmit position 0 is on the air at receive position 0.
    const std::scoped_lock lock(rx_.mutex, tx_.mutex);
    const auto stop_both = [&] {
        for (Side* side_of : {&rx_, &tx_}) {
            side_of->running = false;
            try {
                side_of->board->stop();
            } catch (const BoardError&) {
            }
            side_of->active = false;
            side_of->armed = false;
        }
        try {
            tx_.board->set_hardware_sync(false);
        } catch (const BoardError&) {
        }
        sync_on_ = false;
        set_origin(OriginState::kAbsent);
    };
    try {
        tx_.board->set_hardware_sync(true);
        sync_on_ = true;
        start_stream(Direction::kTx);
        const std::uint64_t before = tx_path_->sent();
        start_stream(Direction::kRx);
        const std::uint64_t moved = before + 2 * tx_.board->transfer_bytes() / kNativeSampleBytes;
        const auto deadline = std::chrono::steady_clock::now() + kTriggerWait;
        while (tx_path_->sent() < moved) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw BoardError(
                    "trigger start: the transmitting board did not start within 500 ms of the "
                    "receiving board; a jumper must run from the receiving board's P28 pin 15 "
                    "(trigger out) to the transmitting board's P28 pin 16 (trigger in)");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const BoardError&) {
        stop_both();
        throw;
    }
    for (Side* side_of : {&rx_, &tx_}) {
        side_of->active = true;
        side_of->armed = false;
    }
    poll_boards(true);
    set_origin(OriginState::kValid, 0);
    log(LogLevel::kInfo, "both boards started on one trigger edge; common time origin set");
}

void Pair::deactivate(Direction direction) {
    const std::lock_guard<std::mutex> control(control_mutex_);
    deactivate_locked(direction);
}

void Pair::deactivate_locked(Direction direction) {
    Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.active) {
        return;
    }
    // The stream ends even when the board cannot be told to stop, as when
    // it has gone from the bus; the error is passed on after.
    s.running = false;
    s.active = false;
    s.armed = false;
    {
        const std::lock_guard<std::mutex> poll(poll_mutex_);
        log_shortfalls(direction, std::nullopt);
    }
    std::exception_ptr error;
    try {
        s.board->stop();
        if (direction == Direction::kTx) {
            if (sync_on_) {
                s.board->set_hardware_sync(false);
            }
            // The firmware keeps the limit; leave the board as it was found.
            s.board->set_tx_underrun_limit(0);
        }
    } catch (const BoardError&) {
        error = std::current_exception();
    }
    if (direction == Direction::kTx) {
        sync_on_ = false;
    }
    if (direction == Direction::kRx) {
        rx_path_->wake();
    } else {
        tx_path_->wake();
    }
    // The origin ties two running streams; it is found again when both run.
    if (time_origin() != TimeOrigin::kNone) {
        set_origin(OriginState::kPending);
    }
    log(LogLevel::kDebug, std::string(direction_name(direction)) + " stopped");
    if (error) {
        std::rethrow_exception(error);
    }
}

bool Pair::active(Direction direction) const {
    const Side& s = side(direction);
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.active;
}

std::size_t Pair::mtu(Direction direction) const {
    return side(direction).board->transfer_bytes() / kNativeSampleBytes;
}

bool Pair::lo_locked(Direction direction) const { return side(direction).lo_locked.load(); }

bool Pair::failed(Direction direction) const { return side(direction).failed.load(); }

// Time

void Pair::set_time_origin(TimeOrigin origin) {
    const std::lock_guard<std::mutex> lock(time_mutex_);
    origin_mode_ = origin;
    log(LogLevel::kInfo, std::string("time origin: ") + time_origin_name(origin));
}

TimeOrigin Pair::time_origin() const {
    const std::lock_guard<std::mutex> lock(time_mutex_);
    return origin_mode_;
}

std::optional<std::int64_t> Pair::tx_of_rx() const {
    const std::lock_guard<std::mutex> lock(time_mutex_);
    if (origin_state_ != OriginState::kValid) {
        return std::nullopt;
    }
    return tx_of_rx_;
}

std::int64_t Pair::time_ticks() const {
    return static_cast<std::int64_t>(rx_received()) + time_offset_.load();
}

void Pair::set_time_ticks(std::int64_t ticks) {
    time_offset_ = ticks - static_cast<std::int64_t>(rx_received());
}

void Pair::set_origin(OriginState state, std::int64_t tx_of_rx) {
    {
        const std::lock_guard<std::mutex> lock(time_mutex_);
        origin_state_ = state;
        tx_of_rx_ = tx_of_rx;
    }
    time_changed_.notify_all();
}

void Pair::lose_origin(const std::string& why) {
    {
        const std::lock_guard<std::mutex> lock(time_mutex_);
        if (origin_state_ != OriginState::kValid) {
            return;
        }
        origin_state_ = OriginState::kAbsent;
    }
    time_changed_.notify_all();
    time_error_ = true;
    log(LogLevel::kWarning, "common time origin lost: " + why);
}

void Pair::mark_failed(Direction direction, const std::string& why) {
    Side& s = side(direction);
    if (s.failed.exchange(true)) {
        return;
    }
    log(LogLevel::kError, std::string(direction_name(direction)) + " board stopped streaming (" +
                              why +
                              "); its stream has ended until it is deactivated and activated "
                              "again");
    // Wake whoever waits on the stream, so it finds out now.
    if (direction == Direction::kRx) {
        rx_path_->wake();
    } else {
        tx_path_->wake();
    }
}

bool Pair::poll_boards(bool force) {
    std::string lost;
    std::vector<Direction> failing;
    std::vector<std::string> reasons;
    {
        const std::lock_guard<std::mutex> lock(poll_mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_poll_ < kBoardPoll) {
            return false;
        }
        last_poll_ = now;
        for (const Direction direction : {Direction::kRx, Direction::kTx}) {
            Side& s = side(direction);
            if (!s.running || s.failed) {
                continue;
            }
            std::uint32_t count = 0;
            // The health read first: on a board gone from the bus it fails
            // with libhackrf's own account of why.
            try {
                const StreamHealth health = s.board->stream_health();
                count = health.shortfalls;
                if (health.stopped_itself) {
                    failing.push_back(direction);
                    reasons.emplace_back(direction == Direction::kTx
                                             ? "it went without data for longer than its underrun "
                                               "limit (tx_underrun_limit) and stopped itself"
                                             : "it stopped itself over a shortfall past its limit");
                    continue;
                }
                if (!s.board->streaming()) {
                    failing.push_back(direction);
                    reasons.emplace_back("its transfers stopped");
                    continue;
                }
            } catch (const BoardError& e) {
                failing.push_back(direction);
                reasons.emplace_back(e.what());
                continue;
            }
            if (count == s.shortfalls_seen) {
                continue;
            }
            s.shortfalls_seen = count;
            s.shortfall_pending = true;
            log_shortfalls(direction, now);
            lost += std::string(lost.empty() ? "" : "; ") + "the " + direction_name(direction) +
                    " board lost or delayed samples";
        }
    }
    for (std::size_t i = 0; i < failing.size(); ++i) {
        mark_failed(failing[i], reasons[i]);
        lose_origin(std::string("the ") + direction_name(failing[i]) + " board stopped streaming");
    }
    if (!lost.empty()) {
        lose_origin(lost + "; write calibrate to find it again");
    }
    return !lost.empty();
}

void Pair::log_shortfalls(Direction direction,
                          std::optional<std::chrono::steady_clock::time_point> now) {
    Side& s = side(direction);
    const std::uint32_t added = s.shortfalls_seen - s.shortfalls_logged;
    if (added == 0) {
        return;
    }
    const bool first = s.shortfalls_logged == 0;
    if (!first && now && *now - s.shortfalls_logged_at < kShortfallLog) {
        return;
    }
    const auto at = now.value_or(std::chrono::steady_clock::now());
    std::string count = std::to_string(added) + (added == 1 ? " shortfall" : " shortfalls");
    if (!first) {
        char seconds[32];
        std::snprintf(seconds, sizeof seconds, "%.1f",
                      std::chrono::duration<double>(at - s.shortfalls_logged_at).count());
        count = std::to_string(added) + " more" + (added == 1 ? " shortfall" : " shortfalls") +
                " in the last " + seconds + " s";
    }
    s.shortfalls_logged = s.shortfalls_seen;
    s.shortfalls_logged_at = at;
    if (direction == Direction::kRx) {
        log(LogLevel::kWarning, "RX board ran out of room in its own buffer (" + count +
                                    "): samples were lost, and receive timestamps from here "
                                    "on are shifted by an unknown amount");
    } else {
        log(LogLevel::kWarning, "TX board ran out of samples in its own buffer (" + count +
                                    "): what follows goes out late by an unknown amount");
    }
}

void Pair::calibrate() {
    const std::lock_guard<std::mutex> control(control_mutex_);
    if (!active(Direction::kRx) || !active(Direction::kTx)) {
        throw std::logic_error("calibrate: both streams must be active");
    }
    calibrate_locked();
}

void Pair::calibrate_locked() {
    set_origin(OriginState::kPending);
    // Shortfalls from before the burst are reported but do not spoil it.
    poll_boards(true);
    const double rx_rate = sample_rate(Direction::kRx);
    const double tx_rate = sample_rate(Direction::kTx);
    if (!(rx_rate > 0) || rx_rate != tx_rate) {
        log(LogLevel::kWarning,
            "no common time origin: the burst needs both directions at one "
            "sample rate");
        set_origin(OriginState::kAbsent);
        return;
    }
    // Free-running, the crystals' offset turns the chips over within the
    // burst, so it cannot be heard: say why rather than send it.
    {
        const std::scoped_lock lock(rx_.mutex, tx_.mutex);
        const bool shared = (rx_.on_clkin && tx_.on_clkin) || (tx_.on_clkin && rx_.drives_clkout) ||
                            (rx_.on_clkin && tx_.drives_clkout);
        if (!shared) {
            const auto describe = [](const Side& s) {
                return std::string(s.on_clkin ? "on CLKIN" : "on its own crystal") +
                       (s.drives_clkout ? ", driving CLKOUT" : "");
            };
            log(LogLevel::kWarning,
                "no common time origin: the boards do not share a reference (RX " + describe(rx_) +
                    ", TX " + describe(tx_) +
                    "), so the calibration burst would not be heard; cable one board's CLKOUT "
                    "to the other's CLKIN and set clkout to that board, rx or tx, before the "
                    "streams start");
            set_origin(OriginState::kAbsent);
            return;
        }
    }

    // The burst must be heard, so the transmitter goes to the receive
    // frequency for it. The firmware skips a tune to the frequency it last
    // applied, so when the two are equal nothing is retuned at all.
    const std::uint64_t rx_hz = frequency(Direction::kRx);
    const std::uint64_t tx_hz = frequency(Direction::kTx);
    const bool retune = rx_hz != 0 && rx_hz != tx_hz;
    if (retune) {
        const std::lock_guard<std::mutex> lock(tx_.mutex);
        tx_.board->set_frequency(rx_hz);
    }

    // Listen from now for as long as the burst can take to come round: a
    // full transmit ring ahead of it, libhackrf's transfers in flight each
    // way, and some margin.
    const std::size_t transfer = std::max(mtu(Direction::kRx), mtu(Direction::kTx));
    const std::size_t listen =
        burst::kChips + (tx_path_->slots() + 2 * kLibhackrfTransfers + 4) * transfer;
    Capture capture(listen);
    rx_path_->set_tap([&capture](std::uint64_t position, std::span<const std::uint8_t> bytes) {
        capture.add(position, bytes);
    });

    std::optional<std::uint64_t> sent_at;
    {
        const std::lock_guard<std::mutex> lock(write_mutex_);
        const std::vector<std::int8_t> samples = burst::samples();
        const std::uint64_t at = tx_path_->queued() + mtu(Direction::kTx);
        const auto timeout = std::chrono::seconds(2);
        const TxWrite first =
            tx_path_->write_at(samples.data(), burst::kChips, Format::kCs8, true, at,
                               std::chrono::duration_cast<std::chrono::microseconds>(timeout));
        std::size_t written = first.taken;
        while (!first.late && written > 0 && written < burst::kChips) {
            const std::size_t n = tx_path_->write(
                samples.data() + 2 * written, burst::kChips - written, Format::kCs8, true,
                std::chrono::duration_cast<std::chrono::microseconds>(timeout));
            if (n == 0) {
                break;
            }
            written += n;
        }
        if (written == burst::kChips) {
            sent_at = at;
        }
    }
    const double seconds = static_cast<double>(listen) / rx_rate;
    const bool full =
        sent_at &&
        capture.wait(std::chrono::milliseconds(static_cast<long>(2000.0 * seconds) + 2000));
    rx_path_->set_tap(nullptr);
    if (retune) {
        // What the pair holds now, not what it held before the burst: the
        // application may have tuned the transmitter meanwhile.
        const std::lock_guard<std::mutex> lock(tx_.mutex);
        tx_.board->set_frequency(tx_.frequency_hz);
    }
    if (!sent_at) {
        log(LogLevel::kWarning,
            "no common time origin: the calibration burst could not be "
            "written to the transmit stream");
        set_origin(OriginState::kAbsent);
        return;
    }
    if (!full) {
        log(LogLevel::kWarning, "calibration: the receive stream delivered less than expected");
    }

    const burst::Found found = burst::find(capture.samples());
    char level[64];
    std::snprintf(level, sizeof(level), "%.1f dB", found.peak_over_median_db);
    if (!found.found) {
        log(LogLevel::kWarning, std::string("no common time origin: the calibration burst was "
                                            "not heard (best correlation ") +
                                    level + " over the median, " +
                                    std::to_string(static_cast<int>(burst::kMinPeakOverMedianDb)) +
                                    " needed)");
        set_origin(OriginState::kAbsent);
        return;
    }
    if (poll_boards(true) || rx_.failed || tx_.failed) {
        log(LogLevel::kWarning,
            "no common time origin: a board lost or delayed samples, or stopped, while the burst "
            "was out, so where it was heard says nothing");
        set_origin(OriginState::kAbsent);
        return;
    }
    const auto rx_at = static_cast<std::int64_t>(capture.start() + found.offset);
    const std::int64_t value = static_cast<std::int64_t>(*sent_at) - rx_at;
    set_origin(OriginState::kValid, value);
    log(LogLevel::kInfo, "common time origin from the calibration burst (" + std::string(level) +
                             " over the median): transmit position = receive position " +
                             (value < 0 ? "- " : "+ ") + std::to_string(std::llabs(value)));
}

namespace {

using Clock = std::chrono::steady_clock;

// The next wait of a read or write that looks at the boards between waits:
// what is left of its time, at most one poll interval.
std::chrono::microseconds slice_until(Clock::time_point deadline) {
    const auto left = std::max(deadline - Clock::now(), Clock::duration::zero());
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::min<Clock::duration>(left, kBoardPoll));
}

}  // namespace

RxRead Pair::read(void* dst, std::size_t max_samples, std::chrono::microseconds timeout) {
    if (!rx_path_) {
        throw std::logic_error("RX stream not open");
    }
    const auto deadline = Clock::now() + timeout;
    while (true) {
        poll_boards();
        if (rx_.failed) {
            return {};
        }
        if (rx_.shortfall_pending.exchange(false)) {
            return RxRead{0, 0, true};
        }
        const RxRead result = rx_path_->read(dst, max_samples, rx_.format, slice_until(deadline));
        if (result.samples > 0 || result.overflow || Clock::now() >= deadline ||
            !active(Direction::kRx)) {
            return result;
        }
    }
}

void Pair::set_rx_window(std::optional<std::uint64_t> start, std::optional<std::uint64_t> count) {
    if (!rx_path_) {
        throw std::logic_error("RX stream not open");
    }
    rx_path_->set_window(start, count);
}

std::size_t Pair::write(const void* src, std::size_t samples, bool end_of_burst,
                        std::chrono::microseconds timeout) {
    if (!tx_path_) {
        throw std::logic_error("TX stream not open");
    }
    const auto deadline = Clock::now() + timeout;
    while (true) {
        poll_boards();
        if (tx_.failed) {
            return 0;
        }
        std::size_t taken = 0;
        {
            const std::lock_guard<std::mutex> lock(write_mutex_);
            taken = tx_path_->write(src, samples, tx_.format, end_of_burst, slice_until(deadline));
        }
        if (taken > 0 || Clock::now() >= deadline || !active(Direction::kTx)) {
            return taken;
        }
    }
}

TimedWrite Pair::write_at(const void* src, std::size_t samples, bool end_of_burst,
                          std::int64_t ticks, std::chrono::microseconds timeout) {
    if (!tx_path_) {
        throw std::logic_error("TX stream not open");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::int64_t position = 0;
    {
        std::unique_lock<std::mutex> lock(time_mutex_);
        time_changed_.wait_until(lock, deadline,
                                 [&] { return origin_state_ != OriginState::kPending; });
        if (origin_state_ == OriginState::kPending) {
            return TimedWrite{0, TimedStatus::kTimeout};
        }
        if (origin_state_ == OriginState::kAbsent) {
            return TimedWrite{0, TimedStatus::kNoTime};
        }
        position = ticks - time_offset_.load() + tx_of_rx_;
    }
    if (position < 0) {
        time_error_ = true;
        return TimedWrite{0, TimedStatus::kLate};
    }
    while (true) {
        poll_boards();
        if (tx_.failed) {
            return TimedWrite{0, TimedStatus::kTimeout};
        }
        TxWrite result;
        {
            const std::lock_guard<std::mutex> lock(write_mutex_);
            result =
                tx_path_->write_at(src, samples, tx_.format, end_of_burst,
                                   static_cast<std::uint64_t>(position), slice_until(deadline));
        }
        if (result.late) {
            return TimedWrite{0, TimedStatus::kLate};
        }
        if (result.taken > 0) {
            return TimedWrite{result.taken, TimedStatus::kOk};
        }
        if (Clock::now() >= deadline || !active(Direction::kTx)) {
            return TimedWrite{0, TimedStatus::kTimeout};
        }
    }
}

bool Pair::take_tx_underflow() {
    poll_boards();
    if (tx_.shortfall_pending.exchange(false)) {
        return true;
    }
    return tx_path_ ? tx_path_->take_underflow() : false;
}

bool Pair::take_time_error() {
    poll_boards();
    if (time_error_.exchange(false)) {
        return true;
    }
    return tx_path_ ? tx_path_->take_late() : false;
}

std::uint64_t Pair::rx_received() const { return rx_path_ ? rx_path_->received() : 0; }
std::uint64_t Pair::rx_dropped() const { return rx_path_ ? rx_path_->dropped() : 0; }
std::uint64_t Pair::tx_sent() const { return tx_path_ ? tx_path_->sent() : 0; }
std::uint64_t Pair::tx_underflows() const { return tx_path_ ? tx_path_->underflows() : 0; }

}  // namespace hackrfduplex
