// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "board.hpp"
#include "format.hpp"
#include "gain.hpp"
#include "rx_path.hpp"
#include "tx_path.hpp"

namespace hackrfduplex {

enum class Direction { kRx, kTx };

enum class LogLevel { kDebug, kInfo, kWarning, kError };
using Log = std::function<void(LogLevel, const std::string&)>;

struct RxGains {
    unsigned lna_db = 16;
    unsigned vga_db = 16;
    bool amp = false;
};

struct TxGains {
    unsigned vga_db = 0;
    bool amp = false;
};

// Which board, if either, drives 10 MHz out of its CLKOUT for the
// other's CLKIN, so the two sample clocks and both local oscillators run
// from one reference.
enum class ClockOut { kOff, kRx, kTx };

// How the two streams get a common time origin when both have started
// (docs/design.md, Time): a calibration burst sent by the transmitter and
// found by the receiver; one trigger edge starting both boards, which
// needs a jumper between them; or none.
enum class TimeOrigin { kBurst, kTrigger, kNone };

// The outcome of a timed write.
enum class TimedStatus {
    kOk,
    // Nothing taken within the timeout, or still waiting for an origin.
    kTimeout,
    // The time asked for has already gone out.
    kLate,
    // The streams have no common origin.
    kNoTime,
};

struct TimedWrite {
    std::size_t taken = 0;
    TimedStatus status = TimedStatus::kOk;
};

// Two HackRF boards as one full-duplex radio: one only ever receives,
// the other only ever transmits, each tuned, filtered and gained on its
// own. Settings go to the board at once and are remembered; a stream is
// opened in a sample format, activated to start the board, and read or
// written from one thread per direction. The two directions share
// nothing but this object, so tuning one never touches the other.
//
// Time is counted in receive samples ("ticks"): a receive position plus an
// offset the caller may set. Once both streams have started, the time
// origin ties transmit positions to receive positions, so a write can be
// placed at a time.
class Pair {
public:
    Pair(std::unique_ptr<Board> rx, std::unique_ptr<Board> tx, Log log);
    // Stops both boards.
    ~Pair();
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;

    Board& board(Direction direction);

    void set_frequency(Direction direction, std::uint64_t hz);
    std::uint64_t frequency(Direction direction) const;

    // Also re-applies an explicit bandwidth, which the firmware resets.
    void set_sample_rate(Direction direction, double hz);
    double sample_rate(Direction direction) const;

    // 0 returns the filter to the firmware's choice for the rate; any
    // other value is rounded down to a filter width.
    void set_bandwidth(Direction direction, std::uint32_t hz);
    // The filter in effect: the explicit width, or the automatic one.
    std::uint32_t bandwidth(Direction direction) const;
    bool bandwidth_is_auto(Direction direction) const;

    void set_rx_gains(RxGains gains);
    RxGains rx_gains() const;
    void set_tx_gains(TxGains gains);
    TxGains tx_gains() const;
    // One figure spread over the stages (gain::split_rx, gain::split_tx).
    void set_overall_gain(Direction direction, double db);
    double overall_gain(Direction direction) const;

    // Antenna port power on the transmitting board; re-applied whenever
    // transmission starts, since the firmware drops it when idle.
    void set_bias_tee(bool on);
    bool bias_tee() const { return bias_; }

    // The other board's CLKOUT is switched off before the chosen one's
    // goes on. Read back from the boards, since CLKOUT survives this
    // object and dies with the board's power.
    void set_clock_out(ClockOut which);
    ClockOut clock_out();

    // Streams. `slots` is the ring depth in transfers.
    void open_stream(Direction direction, Format format, std::size_t slots);
    void close_stream(Direction direction);
    bool stream_open(Direction direction) const;
    // Starts the board; a no-op when already active. Throws std::logic_error
    // with no stream open.
    void activate(Direction direction);
    void deactivate(Direction direction);
    bool active(Direction direction) const;
    // Samples per transfer, the unit a stream moves in.
    std::size_t mtu(Direction direction) const;

    // Whether the direction's synthesisers are tuned on the reference the
    // board runs from. libhackrf cannot report lock, so this is the pair's
    // own account: false only while activate() handles a reference switch.
    // Lock-free, so it can be read from any thread during activate().
    bool lo_locked(Direction direction) const;

    // How the next start of both streams finds a common origin. Takes
    // effect when the streams next start; calibrate() acts at once.
    void set_time_origin(TimeOrigin origin);
    TimeOrigin time_origin() const;
    // Sends the calibration burst now. Throws std::logic_error unless both
    // streams are active. The origin is found, or left empty and logged.
    void calibrate();
    // The transmit position on the air at the instant of receive position
    // 0, while the two streams share an origin.
    std::optional<std::int64_t> tx_of_rx() const;
    // The time now: samples the receiver has delivered, plus the offset.
    std::int64_t time_ticks() const;
    // Sets the offset so that time_ticks() reads `ticks` now.
    void set_time_ticks(std::int64_t ticks);
    std::int64_t time_offset() const { return time_offset_.load(); }

    // Reader thread. Throws std::logic_error with no RX stream open.
    RxRead read(void* dst, std::size_t max_samples, std::chrono::microseconds timeout);
    // Writer thread. Throws std::logic_error with no TX stream open.
    std::size_t write(const void* src, std::size_t samples, bool end_of_burst,
                      std::chrono::microseconds timeout);
    // Writer thread: the first sample at time `ticks`. Waits up to
    // `timeout` for an origin still to come.
    TimedWrite write_at(const void* src, std::size_t samples, bool end_of_burst, std::int64_t ticks,
                        std::chrono::microseconds timeout);
    bool take_tx_underflow();
    // True once for each time error: a late write, a late block, or the
    // origin lost to a board's shortfall. Also polls the shortfalls.
    bool take_time_error();

    std::uint64_t rx_received() const;
    std::uint64_t rx_dropped() const;
    std::uint64_t tx_sent() const;
    std::uint64_t tx_underflows() const;

private:
    struct Side {
        std::unique_ptr<Board> board;
        mutable std::mutex mutex;
        std::uint64_t frequency_hz = 0;
        double sample_rate_hz = 0;
        // 0: the firmware's automatic choice.
        std::uint32_t bandwidth_hz = 0;
        bool open = false;
        bool active = false;
        // Trigger origin: activated, waiting for the other stream to start
        // both boards together.
        bool armed = false;
        Format format = Format::kCs8;
        std::atomic<bool> lo_locked{true};
    };

    enum class OriginState { kPending, kValid, kAbsent };

    Side& side(Direction direction);
    const Side& side(Direction direction) const;
    void apply_rx_gains();
    void apply_tx_gains();
    void log(LogLevel level, const std::string& message);

    void deactivate_locked(Direction direction);
    // The board's stream started, with the path reset. Side mutex held.
    void start_stream(Direction direction);
    // When the next start moves the board to another reference: starts it,
    // retunes on the new reference and stops it. Side mutex held.
    bool settle_reference(Direction direction);
    // Both boards started on one trigger edge, transmitter first.
    void start_triggered();
    // The burst sent and looked for; both streams active.
    void calibrate_locked();
    void set_origin(OriginState state, std::int64_t tx_of_rx = 0);
    // The origin lost, reported once through take_time_error().
    void lose_origin(const std::string& why);
    void note_shortfall_baseline();
    // Reads the boards' shortfall counts, at most every 100 ms.
    void poll_shortfalls();

    Side rx_;
    Side tx_;
    RxGains rx_gains_;
    TxGains tx_gains_;
    bool bias_ = false;
    std::unique_ptr<RxPath> rx_path_;
    std::unique_ptr<TxPath> tx_path_;
    Log log_;

    // Stream lifecycle: open, close, activate, deactivate, calibrate.
    std::mutex control_mutex_;
    // The transmit writer, which the calibration burst shares.
    std::mutex write_mutex_;
    // Hardware sync left on the transmitter by a trigger start.
    bool sync_on_ = false;

    mutable std::mutex time_mutex_;
    std::condition_variable time_changed_;
    TimeOrigin origin_mode_ = TimeOrigin::kBurst;
    OriginState origin_state_ = OriginState::kPending;
    std::int64_t tx_of_rx_ = 0;
    std::atomic<std::int64_t> time_offset_{0};
    std::atomic<bool> time_error_{false};
    std::mutex poll_mutex_;
    std::uint32_t rx_shortfalls_ = 0;
    std::uint32_t tx_shortfalls_ = 0;
    std::chrono::steady_clock::time_point last_poll_{};
};

const char* direction_name(Direction direction);
const char* clock_out_name(ClockOut which);
// From "off", "rx" or "tx"; throws std::invalid_argument otherwise.
ClockOut parse_clock_out(const std::string& name);
const char* time_origin_name(TimeOrigin origin);
// From "burst", "trigger" or "none"; throws std::invalid_argument otherwise.
TimeOrigin parse_time_origin(const std::string& name);

}  // namespace hackrfduplex
