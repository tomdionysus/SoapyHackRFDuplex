// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

// Two HackRF boards as one full-duplex radio: one only ever receives,
// the other only ever transmits, each tuned, filtered and gained on its
// own. Settings go to the board at once and are remembered; a stream is
// opened in a sample format, activated to start the board, and read or
// written from one thread per direction. The two directions share
// nothing but this object, so tuning one never touches the other.
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

    // Reader thread. Throws std::logic_error with no RX stream open.
    RxRead read(void* dst, std::size_t max_samples, std::chrono::microseconds timeout);
    // Writer thread. Throws std::logic_error with no TX stream open.
    std::size_t write(const void* src, std::size_t samples, bool end_of_burst,
                      std::chrono::microseconds timeout);
    bool take_tx_underflow();

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
        Format format = Format::kCs8;
    };

    Side& side(Direction direction);
    const Side& side(Direction direction) const;
    void apply_rx_gains();
    void apply_tx_gains();
    void log(LogLevel level, const std::string& message);

    Side rx_;
    Side tx_;
    RxGains rx_gains_;
    TxGains tx_gains_;
    bool bias_ = false;
    std::unique_ptr<RxPath> rx_path_;
    std::unique_ptr<TxPath> tx_path_;
    Log log_;
};

const char* direction_name(Direction direction);

}  // namespace hackrfduplex
