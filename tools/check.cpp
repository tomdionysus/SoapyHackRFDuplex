// SPDX-License-Identifier: GPL-3.0-or-later
//
// hackrfduplex-check: opens a pair through SoapySDR, receives for a
// while, optionally transmits a tone meanwhile, and reports rates,
// drops and what the receiver heard. The transmitter stays off unless
// --tx-tone or --timed-bursts is given. --timed-bursts places bursts at
// receive timestamps plus a delay and reports where each one arrived.

#include <SoapySDR/Constants.h>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "burst.hpp"

namespace {

struct Options {
    std::string rx_serial;
    std::string tx_serial;
    double rate_hz = 8e6;
    // 0: the same as rate_hz.
    double tx_rate_hz = 0;
    double rx_frequency_hz = 868.3e6;
    double tx_frequency_hz = 868.3e6;
    double rx_gain_db = 32;
    double tx_gain_db = 0;
    // 0: never transmit. Otherwise a tone this far from the TX centre.
    double tx_tone_hz = 0;
    double seconds = 5;
    // Empty: leave the boards' CLKOUT as they are.
    std::string clkout;
    std::string tx_format = SOAPY_SDR_CF32;
    // Empty: the module's default.
    std::string time_origin;
    // Extra calibrations once both streams run, each reporting the origin.
    int calibrations = 0;
    // Timed mode when set: the delay from a receive timestamp to its burst.
    std::optional<double> timed_delay_us;
    // Window mode when set: the delay from the time now to each burst, which
    // a timed receive window is opened to catch.
    std::optional<double> window_delay_us;
    // 0: the module's default.
    int buffers = 0;
    bool help = false;
};

// The module reports the origin it found in its log; keep the last one.
std::mutex g_log_mutex;
std::optional<long long> g_tx_of_rx;
std::string g_last_warning;

void log_handler(const SoapySDRLogLevel level, const char* message) {
    const std::string text = message;
    const char* name = level <= SOAPY_SDR_ERROR     ? "ERROR"
                       : level <= SOAPY_SDR_WARNING ? "WARNING"
                       : level <= SOAPY_SDR_INFO    ? "INFO"
                                                    : "DEBUG";
    if (level == SOAPY_SDR_SSI) {
        std::fputs(message, stderr);
        return;
    }
    std::fprintf(stderr, "[%s] %s\n", name, message);
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    const std::string marker = "transmit position = receive position ";
    const auto at = text.find(marker);
    if (at != std::string::npos) {
        const std::string rest = text.substr(at + marker.size());
        const long long magnitude = std::stoll(rest.substr(2));
        g_tx_of_rx = rest[0] == '-' ? -magnitude : magnitude;
    }
    if (level <= SOAPY_SDR_WARNING) {
        g_last_warning = text;
    }
}

std::optional<long long> take_tx_of_rx() {
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    const auto value = g_tx_of_rx;
    g_tx_of_rx.reset();
    return value;
}

std::size_t format_bytes(const std::string& format) {
    if (format == SOAPY_SDR_CS8) {
        return 2;
    }
    if (format == SOAPY_SDR_CS16) {
        return 4;
    }
    if (format == SOAPY_SDR_CF32) {
        return 8;
    }
    if (format == SOAPY_SDR_CF64) {
        return 16;
    }
    throw std::invalid_argument("--tx-format must be CS8, CS16, CF32 or CF64, not " + format);
}

// Writes `samples` into `out` in `format`, full scale 1.0 for the floats
// and the type's maximum for the integers.
void pack(const std::vector<std::complex<float>>& samples, const std::string& format,
          std::vector<std::uint8_t>& out) {
    out.resize(samples.size() * format_bytes(format));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float re = samples[i].real();
        const float im = samples[i].imag();
        if (format == SOAPY_SDR_CS8) {
            auto* p = reinterpret_cast<std::int8_t*>(out.data()) + i * 2;
            p[0] = static_cast<std::int8_t>(std::lround(re * 127.0f));
            p[1] = static_cast<std::int8_t>(std::lround(im * 127.0f));
        } else if (format == SOAPY_SDR_CS16) {
            auto* p = reinterpret_cast<std::int16_t*>(out.data()) + i * 2;
            p[0] = static_cast<std::int16_t>(std::lround(re * 32767.0f));
            p[1] = static_cast<std::int16_t>(std::lround(im * 32767.0f));
        } else if (format == SOAPY_SDR_CF32) {
            auto* p = reinterpret_cast<float*>(out.data()) + i * 2;
            p[0] = re;
            p[1] = im;
        } else {
            auto* p = reinterpret_cast<double*>(out.data()) + i * 2;
            p[0] = re;
            p[1] = im;
        }
    }
}

const char* const kUsage =
    "usage: hackrfduplex-check [options]\n"
    "\n"
    "Receives through the hackrfduplex SoapySDR module and reports. Nothing is\n"
    "transmitted unless --tx-tone is given.\n"
    "\n"
    "  --rx-serial S     board that receives (suffix of its serial)\n"
    "  --tx-serial S     board that transmits\n"
    "  --rate HZ         sample rate, both directions (default 8e6)\n"
    "  --tx-rate HZ      transmit sample rate, if not the same as --rate\n"
    "  --rx-freq HZ      receive centre frequency (default 868.3e6)\n"
    "  --tx-freq HZ      transmit centre frequency (default 868.3e6)\n"
    "  --rx-gain DB      overall receive gain (default 32)\n"
    "  --tx-gain DB      overall transmit gain (default 0)\n"
    "  --tx-tone HZ      transmit a tone this far from the TX centre (default off)\n"
    "  --seconds S       how long to run (default 5)\n"
    "  --clkout WHICH    off, rx or tx: which board drives 10 MHz to the other's CLKIN\n"
    "                    (default: leave as is)\n"
    "  --tx-format F     CS8, CS16, CF32 or CF64 for the transmit stream (default CF32)\n"
    "  --buffers N       the streams' ring depth in transfers (default: the module's)\n"
    "  --time-origin O   burst, trigger or none (default: the module's, burst)\n"
    "  --calibrations N  once both streams run, calibrate N more times and report\n"
    "                    the origin each time found\n"
    "  --timed-bursts US transmit a 4096-sample burst at each receive timestamp plus\n"
    "                    US microseconds, and report where each arrived (transmits;\n"
    "                    excludes --tx-tone)\n"
    "  --timed-window US transmit a burst US microseconds after the time now and\n"
    "                    open a timed receive window just around it, again and\n"
    "                    again, and report where in each window it arrived\n"
    "                    (transmits; excludes --tx-tone and --timed-bursts)\n"
    "  --help\n";

void parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(arg + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else if (arg == "--rx-serial") {
            options.rx_serial = value();
        } else if (arg == "--tx-serial") {
            options.tx_serial = value();
        } else if (arg == "--rate") {
            options.rate_hz = std::stod(value());
        } else if (arg == "--tx-rate") {
            options.tx_rate_hz = std::stod(value());
        } else if (arg == "--rx-freq") {
            options.rx_frequency_hz = std::stod(value());
        } else if (arg == "--tx-freq") {
            options.tx_frequency_hz = std::stod(value());
        } else if (arg == "--rx-gain") {
            options.rx_gain_db = std::stod(value());
        } else if (arg == "--tx-gain") {
            options.tx_gain_db = std::stod(value());
        } else if (arg == "--tx-tone") {
            options.tx_tone_hz = std::stod(value());
        } else if (arg == "--seconds") {
            options.seconds = std::stod(value());
        } else if (arg == "--clkout") {
            options.clkout = value();
        } else if (arg == "--tx-format") {
            options.tx_format = value();
            format_bytes(options.tx_format);
        } else if (arg == "--buffers") {
            options.buffers = std::stoi(value());
        } else if (arg == "--time-origin") {
            options.time_origin = value();
        } else if (arg == "--calibrations") {
            options.calibrations = std::stoi(value());
        } else if (arg == "--timed-bursts") {
            options.timed_delay_us = std::stod(value());
        } else if (arg == "--timed-window") {
            options.window_delay_us = std::stod(value());
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
}

// In-place radix-2 FFT; `data` is a power of two long.
void fft(std::vector<std::complex<double>>& data) {
    const std::size_t n = data.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> step =
            std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(len));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = data[i + k];
                const std::complex<double> v = data[i + k + len / 2] * w;
                data[i + k] = u + v;
                data[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// The average power spectrum of the whole band, in kBins bins, with the
// strongest peaks reported in dBFS per bin. A full-scale tone in one bin
// reads 0 dBFS; the noise per bin sits 10 log10(kBins) below the mean.
class Spectrum {
public:
    static constexpr std::size_t kBins = 8192;

    explicit Spectrum(double rate_hz) : rate_hz_(rate_hz), power_(kBins, 0.0), block_(kBins) {}

    void add(const std::complex<float>* samples, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::complex<double> s(samples[i].real(), samples[i].imag());
            total_power_ += std::norm(s);
            peak_ = std::max({peak_, std::fabs(s.real()), std::fabs(s.imag())});
            ++count_;
            // Hann window, so a tone does not leak across the band.
            const double w =
                0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(filled_) /
                                     static_cast<double>(kBins));
            block_[filled_] = s * w;
            if (++filled_ == kBins) {
                fft(block_);
                for (std::size_t k = 0; k < kBins; ++k) {
                    power_[k] += std::norm(block_[k]);
                }
                ++blocks_;
                filled_ = 0;
            }
        }
    }

    struct Peak {
        double offset_hz = 0;  // from the centre
        double level_dbfs = -200;
    };

    // Level of the bin holding `offset_hz`.
    double level_at(double offset_hz) const { return level(bin_of(offset_hz)); }

    // The `count` strongest bins, each the local maximum of its
    // neighbourhood, the centre's own three bins (DC) left out.
    std::vector<Peak> peaks(std::size_t count) const {
        std::vector<Peak> found;
        for (std::size_t k = 0; k < kBins; ++k) {
            const std::size_t left = (k + kBins - 1) % kBins;
            const std::size_t right = (k + 1) % kBins;
            if (power_[k] < power_[left] || power_[k] < power_[right]) {
                continue;
            }
            if (k <= 1 || k >= kBins - 1) {
                continue;
            }
            found.push_back(Peak{offset_of(k), level(k)});
        }
        std::sort(found.begin(), found.end(),
                  [](const Peak& a, const Peak& b) { return a.level_dbfs > b.level_dbfs; });
        if (found.size() > count) {
            found.resize(count);
        }
        return found;
    }

    double mean_dbfs() const {
        if (count_ == 0) {
            return -200;
        }
        return 10 * std::log10(total_power_ / static_cast<double>(count_) + 1e-24);
    }
    double median_bin_dbfs() const {
        std::vector<double> levels;
        levels.reserve(kBins);
        for (std::size_t k = 0; k < kBins; ++k) {
            levels.push_back(level(k));
        }
        std::nth_element(levels.begin(), levels.begin() + static_cast<long>(kBins / 2),
                         levels.end());
        return levels[kBins / 2];
    }
    double peak() const { return peak_; }

private:
    // Hann halves the coherent gain, so a bin's amplitude is |X| / (N / 2).
    double level(std::size_t k) const {
        if (blocks_ == 0) {
            return -200;
        }
        const double amplitude = std::sqrt(power_[k] / static_cast<double>(blocks_)) /
                                 (static_cast<double>(kBins) / 2.0);
        return 20 * std::log10(amplitude + 1e-12);
    }
    double offset_of(std::size_t k) const {
        const double signed_k = k < kBins / 2 ? static_cast<double>(k)
                                              : static_cast<double>(k) - static_cast<double>(kBins);
        return signed_k * rate_hz_ / static_cast<double>(kBins);
    }
    std::size_t bin_of(double offset_hz) const {
        const long k = std::lround(offset_hz / rate_hz_ * static_cast<double>(kBins));
        return static_cast<std::size_t>((k % static_cast<long>(kBins) + static_cast<long>(kBins)) %
                                        static_cast<long>(kBins));
    }

    double rate_hz_;
    std::vector<double> power_;
    std::vector<std::complex<double>> block_;
    std::size_t filled_ = 0;
    std::size_t blocks_ = 0;
    double total_power_ = 0;
    double peak_ = 0;
    std::uint64_t count_ = 0;
};

// Looks for a tone near an expected offset: the stream is mixed so the
// expected tone sits at 0 Hz, averaged down to kSearchRate, and each
// second's worth is searched bin by bin at kBinHz over +-kSpanHz. The
// peak of each second gives the tone's real offset and, across seconds,
// its drift.
class ToneFinder {
public:
    static constexpr double kSearchRate = 8000;
    static constexpr double kSpanHz = 3000;
    static constexpr double kBinHz = 1;

    ToneFinder(double expected_offset_hz, double rate_hz)
        : decimation_(static_cast<std::size_t>(std::lround(rate_hz / kSearchRate))),
          rotation_(std::polar(1.0, -2.0 * std::numbers::pi * expected_offset_hz / rate_hz)) {}

    void add(const std::complex<float>* samples, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::complex<double> s(samples[i].real(), samples[i].imag());
            accumulator_ += s * phase_;
            phase_ *= rotation_;
            if (++accumulated_ == decimation_) {
                baseband_.push_back(accumulator_ / static_cast<double>(decimation_));
                accumulator_ = 0;
                accumulated_ = 0;
                phase_ /= std::abs(phase_);
            }
        }
    }

    struct Peak {
        double offset_hz = 0;  // from the expected tone
        double level_dbfs = -200;
    };

    // One peak per whole second of input.
    std::vector<Peak> peaks() const {
        std::vector<Peak> result;
        const auto window = static_cast<std::size_t>(kSearchRate);
        for (std::size_t start = 0; start + window <= baseband_.size(); start += window) {
            Peak best;
            for (double f = -kSpanHz; f <= kSpanHz; f += kBinHz) {
                const std::complex<double> step =
                    std::polar(1.0, -2.0 * std::numbers::pi * f / kSearchRate);
                std::complex<double> phase(1.0, 0.0);
                std::complex<double> sum(0.0, 0.0);
                for (std::size_t n = 0; n < window; ++n) {
                    sum += baseband_[start + n] * phase;
                    phase *= step;
                }
                const double level =
                    20 * std::log10(std::abs(sum) / static_cast<double>(window) + 1e-12);
                if (level > best.level_dbfs) {
                    best = Peak{f, level};
                }
            }
            result.push_back(best);
        }
        return result;
    }

private:
    std::size_t decimation_;
    std::complex<double> rotation_;
    std::complex<double> phase_{1.0, 0.0};
    std::complex<double> accumulator_{0.0, 0.0};
    std::size_t accumulated_ = 0;
    std::vector<std::complex<double>> baseband_;
};

double epoch_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct RxResult {
    std::uint64_t samples = 0;
    unsigned overflows = 0;
    unsigned timeouts = 0;
    int error = 0;
    // Wall-clock seconds since the epoch when the error came, for scripts
    // that time a fault against it.
    double error_at = 0;
    long long first_time_ns = -1;
    long long last_time_ns = -1;
};

struct TxResult {
    std::uint64_t samples = 0;
    unsigned underflows = 0;
    unsigned timeouts = 0;
    int error = 0;
    // Wall-clock seconds since the epoch when the error came, for scripts
    // that time a fault against it.
    double error_at = 0;
};

SoapySDR::Kwargs stream_args(const Options& options) {
    SoapySDR::Kwargs args;
    if (options.buffers > 0) {
        args["buffers"] = std::to_string(options.buffers);
    }
    return args;
}

// The per-channel sensor that says whether each running board's synthesisers
// are locked to the reference it now runs on.
void report_lo_locked(SoapySDR::Device* device, bool transmit) {
    std::printf("sensors: rx lo_locked %s",
                device->readSensor(SOAPY_SDR_RX, 0, "lo_locked").c_str());
    if (transmit) {
        std::printf(", tx lo_locked %s", device->readSensor(SOAPY_SDR_TX, 0, "lo_locked").c_str());
    }
    std::printf("\n");
}

// The device's sensors: whether the boards share a reference and whether
// the streams share a time origin.
// A board gone from the bus cannot be asked, which is no reason to lose
// the rest of the report.
void report_device_sensors(SoapySDR::Device* device, const char* when) {
    try {
        const std::string ref = device->readSensor("ref_locked");
        const std::string time = device->readSensor("time_locked");
        std::printf("sensors %s: ref_locked %s, time_locked %s\n", when, ref.c_str(), time.c_str());
    } catch (const std::exception& e) {
        std::printf("sensors %s: unreadable (%s)\n", when, e.what());
    }
}

// The origin found when both streams started, then `count` more
// calibrations, each reported, and their spread.
void run_calibrations(SoapySDR::Device* device, int count) {
    std::vector<long long> found;
    const auto first = take_tx_of_rx();
    if (first) {
        std::printf("time: origin at start: transmit position = receive position %+lld\n", *first);
        found.push_back(*first);
    } else {
        std::printf("time: no common origin at start\n");
    }
    for (int i = 1; i <= count; ++i) {
        device->writeSetting("calibrate", "");
        const auto value = take_tx_of_rx();
        if (value) {
            std::printf("time: calibration %d: transmit position = receive position %+lld\n", i,
                        *value);
            found.push_back(*value);
        } else {
            std::printf("time: calibration %d: no origin found\n", i);
        }
    }
    if (found.size() > 1) {
        const auto [low, high] = std::minmax_element(found.begin(), found.end());
        std::printf("time: origin spread %lld samples over %zu findings\n", *high - *low,
                    found.size());
    }
}

struct TimedResult {
    unsigned written = 0;
    unsigned late_writes = 0;
    unsigned write_timeouts = 0;
    unsigned time_errors = 0;
    unsigned underflows = 0;
    unsigned overflows = 0;
    unsigned found = 0;
    unsigned missing = 0;
    unsigned lost = 0;
    unsigned behind = 0;
    // getHardwareTime between two reads, against their timestamps.
    unsigned clock_checks = 0;
    unsigned clock_outside = 0;
    std::map<long long, unsigned> errors;
};

// Timed mode: after each read stamped t, the burst is written for time
// t + delay; each is then looked for in the received samples within a
// window around where it was asked to land, and the difference counted.
int run_timed(SoapySDR::Device* device, const Options& options) {
    const double rate = options.rate_hz;
    const SoapySDR::Kwargs args = stream_args(options);
    SoapySDR::Stream* rx = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, {}, args);
    SoapySDR::Stream* tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8, {}, args);
    if (device->activateStream(tx) != 0 || device->activateStream(rx) != 0) {
        throw std::runtime_error("the streams did not activate");
    }
    report_lo_locked(device, true);
    run_calibrations(device, options.calibrations);
    if (!device->hasHardwareTime()) {
        std::printf("timed: no common time origin, which timed bursts need\n");
        device->deactivateStream(rx);
        device->deactivateStream(tx);
        device->closeStream(rx);
        device->closeStream(tx);
        return 1;
    }

    const std::vector<std::int8_t> burst = hackrfduplex::burst::samples();
    const auto chips = static_cast<long long>(hackrfduplex::burst::kChips);
    const long long margin = 2048;
    const long long delay = std::llround(*options.timed_delay_us * 1e-6 * rate);
    const std::size_t mtu = device->getStreamMTU(rx);
    std::vector<std::int8_t> buffer(2 * mtu);
    std::vector<std::int8_t> history;
    long long history_start = 0;
    std::deque<long long> pending;
    long long last_scheduled = -(1LL << 62);
    TimedResult result;

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(options.seconds));
    std::optional<long long> previous_read_ns;
    while (std::chrono::steady_clock::now() < deadline) {
        int flags = 0;
        long long time_ns = 0;
        void* buffs[] = {buffer.data()};
        const long long clock_ns = device->getHardwareTime();
        const int n = device->readStream(rx, buffs, mtu, flags, time_ns, 200000);
        if (n == SOAPY_SDR_OVERFLOW) {
            ++result.overflows;
            previous_read_ns.reset();
            continue;
        }

        if (n == SOAPY_SDR_TIMEOUT) {
            continue;
        }
        if (n < 0) {
            std::printf("timed: read error %s\n", SoapySDR::errToStr(n));
            break;
        }
        const long long tick = SoapySDR::timeNsToTicks(time_ns, rate);
        const auto held = static_cast<long long>(history.size() / 2);
        if (history.empty() || tick != history_start + held) {
            // A gap: bursts whose window ran into it cannot be judged.
            while (!pending.empty() && pending.front() - margin < tick) {
                pending.pop_front();
                ++result.lost;
            }
            history.clear();
            history_start = tick;
        }
        history.insert(history.end(), buffer.begin(), buffer.begin() + 2 * n);

        // A read from a backlog (the ring fills while activation calibrates)
        // is not live; stamping from it measures the backlog, not the lead.
        const long long newest = SoapySDR::timeNsToTicks(device->getHardwareTime(), rate);
        const bool live = newest - (tick + n) < static_cast<long long>(mtu);
        if (!live) {
            ++result.behind;
            previous_read_ns.reset();
        } else {
            // Between two live reads the time now lies between the first
            // one's first sample and the second one's last.
            const long long end_ns = time_ns + SoapySDR::ticksToTimeNs(n, rate);
            if (previous_read_ns) {
                ++result.clock_checks;
                if (clock_ns < *previous_read_ns || clock_ns > end_ns) {
                    ++result.clock_outside;
                }
            }
            previous_read_ns = time_ns;
        }
        const long long at = tick + delay;
        if (live && at >= last_scheduled + 4 * chips) {
            int write_flags = SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST;
            const void* out[] = {burst.data()};
            const int w = device->writeStream(tx, out, burst.size() / 2, write_flags,
                                              SoapySDR::ticksToTimeNs(at, rate), 0);
            if (w == SOAPY_SDR_TIME_ERROR) {
                ++result.late_writes;
            } else if (w == SOAPY_SDR_TIMEOUT) {
                ++result.write_timeouts;
            } else if (w < 0) {
                std::printf("timed: write error %s\n", SoapySDR::errToStr(w));
                break;
            } else {
                long long done = w;
                while (done < chips) {
                    int more_flags = SOAPY_SDR_END_BURST;
                    const void* rest[] = {burst.data() + 2 * done};
                    const int m = device->writeStream(
                        tx, rest, static_cast<std::size_t>(chips - done), more_flags, 0, 100000);
                    if (m <= 0) {
                        break;
                    }
                    done += m;
                }
                ++result.written;
                pending.push_back(at);
                last_scheduled = at;
            }
        }

        while (true) {
            std::size_t mask = 0;
            int status_flags = 0;
            long long status_time = 0;
            const int r = device->readStreamStatus(tx, mask, status_flags, status_time, 0);
            if (r == SOAPY_SDR_UNDERFLOW) {
                ++result.underflows;
            } else if (r == SOAPY_SDR_TIME_ERROR) {
                ++result.time_errors;
            } else {
                break;
            }
        }

        const long long end = history_start + static_cast<long long>(history.size() / 2);
        while (!pending.empty() && pending.front() + chips + margin <= end) {
            const long long asked = pending.front();
            pending.pop_front();
            const long long low = std::max(asked - margin, history_start);
            const auto from = static_cast<std::size_t>(2 * (low - history_start));
            const auto length = static_cast<std::size_t>(2 * (asked + chips + margin - low));
            const hackrfduplex::burst::Found found = hackrfduplex::burst::find(
                std::span<const std::int8_t>(history.data() + from, length));
            if (found.found) {
                ++result.found;
                ++result.errors[low + static_cast<long long>(found.offset) - asked];
            } else {
                ++result.missing;
            }
        }
        const long long keep = std::max(
            history_start, (pending.empty() ? end : std::min(pending.front(), end)) - margin);
        if (keep - history_start > 4 * static_cast<long long>(mtu)) {
            history.erase(history.begin(), history.begin() + 2 * (keep - history_start));
            history_start = keep;
        }
    }
    device->deactivateStream(rx);
    device->deactivateStream(tx);
    device->closeStream(rx);
    device->closeStream(tx);

    std::printf("timed: delay %.0f us = %lld samples at %.3f Msps\n", *options.timed_delay_us,
                delay, rate / 1e6);
    std::printf(
        "timed: %u bursts written, %u late writes, %u write timeouts, %u reads from a backlog "
        "not stamped\n",
        result.written, result.late_writes, result.write_timeouts, result.behind);
    std::printf("timed: %u found, %u not found, %u lost to receive overflows (%u overflows)\n",
                result.found, result.missing, result.lost, result.overflows);
    std::printf("timed: stream status: %u time errors, %u underflows\n", result.time_errors,
                result.underflows);
    std::printf("timed: hardware time between two reads' timestamps: %u of %u\n",
                result.clock_checks - result.clock_outside, result.clock_checks);
    std::printf("timed: arrival against the time asked, in samples:");
    for (const auto& [error, count] : result.errors) {
        std::printf(" %+lld (x%u)", error, count);
    }
    std::printf("\n");
    const bool clean = result.late_writes == 0 && result.time_errors == 0 && result.missing == 0 &&
                       result.errors.size() <= 1 && result.clock_outside == 0;
    std::printf("timed: %s\n", clean ? "every burst on time and in place" : "NOT CLEAN");
    return clean ? 0 : 1;
}

// Window mode: each round sends the burst at the time now plus a delay
// and opens a receive window from kMargin samples before it, for the burst
// and kMargin either side; the burst must be found kMargin samples in.
int run_window(SoapySDR::Device* device, const Options& options) {
    constexpr long long kMargin = 1024;
    const double rate = options.rate_hz;
    const SoapySDR::Kwargs args = stream_args(options);
    SoapySDR::Stream* rx = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS8, {}, args);
    SoapySDR::Stream* tx = device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS8, {}, args);
    const auto close = [&] {
        device->deactivateStream(rx);
        device->deactivateStream(tx);
        device->closeStream(rx);
        device->closeStream(tx);
    };
    if (device->activateStream(tx) != 0 || device->activateStream(rx) != 0) {
        throw std::runtime_error("the streams did not activate");
    }
    report_lo_locked(device, true);
    run_calibrations(device, options.calibrations);
    if (!device->hasHardwareTime()) {
        std::printf("window: no common time origin, which timed windows need\n");
        close();
        return 1;
    }

    const std::vector<std::int8_t> burst = hackrfduplex::burst::samples();
    const auto chips = static_cast<long long>(hackrfduplex::burst::kChips);
    const long long delay = std::llround(*options.window_delay_us * 1e-6 * rate);
    const auto length = static_cast<std::size_t>(chips + 2 * kMargin);
    const std::size_t mtu = device->getStreamMTU(rx);
    std::vector<std::int8_t> window(2 * length);
    unsigned rounds = 0;
    unsigned refused = 0;
    unsigned short_windows = 0;
    unsigned unflagged = 0;
    unsigned missing = 0;
    unsigned stamped_wrong = 0;
    std::map<long long, unsigned> errors;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(options.seconds));
    while (std::chrono::steady_clock::now() < deadline) {
        ++rounds;
        const long long at = SoapySDR::timeNsToTicks(device->getHardwareTime(), rate) + delay;
        int flags = SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST;
        const void* out[] = {burst.data()};
        long long done = device->writeStream(tx, out, static_cast<std::size_t>(chips), flags,
                                             SoapySDR::ticksToTimeNs(at, rate), 100000);
        while (done > 0 && done < chips) {
            int more = SOAPY_SDR_END_BURST;
            const void* rest[] = {burst.data() + 2 * done};
            const int m = device->writeStream(tx, rest, static_cast<std::size_t>(chips - done),
                                              more, 0, 100000);
            if (m <= 0) {
                break;
            }
            done += m;
        }
        const long long from = at - kMargin;
        if (done != chips ||
            device->activateStream(rx, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST,
                                   SoapySDR::ticksToTimeNs(from, rate), length) != 0) {
            ++refused;
            continue;
        }
        std::size_t got = 0;
        bool ended = false;
        bool first = true;
        while (got < length && !ended) {
            int read_flags = 0;
            long long time_ns = 0;
            void* buffs[] = {window.data() + 2 * got};
            const int n = device->readStream(rx, buffs, std::min(mtu, length - got), read_flags,
                                             time_ns, 1000000);
            if (n <= 0) {
                break;
            }
            if (first && SoapySDR::timeNsToTicks(time_ns, rate) != from) {
                ++stamped_wrong;
            }
            first = false;
            got += static_cast<std::size_t>(n);
            ended = (read_flags & SOAPY_SDR_END_BURST) != 0;
        }
        if (got != length) {
            ++short_windows;
            continue;
        }
        if (!ended) {
            ++unflagged;
        }
        const hackrfduplex::burst::Found found =
            hackrfduplex::burst::find(std::span<const std::int8_t>(window.data(), 2 * length));
        if (found.found) {
            ++errors[static_cast<long long>(found.offset) - kMargin];
        } else {
            ++missing;
        }
    }
    close();

    std::printf(
        "window: lead %.0f us = %lld samples at %.3f Msps; windows of %zu samples from "
        "%lld before each burst\n",
        *options.window_delay_us, delay, rate / 1e6, length, kMargin);
    std::printf(
        "window: %u rounds, %u refused, %u short, %u without END_BURST, %u stamped "
        "other than asked\n",
        rounds, refused, short_windows, unflagged, stamped_wrong);
    std::printf("window: burst not found in %u; found against where it was sent, in samples:",
                missing);
    for (const auto& [error, count] : errors) {
        std::printf(" %+lld (x%u)", error, count);
    }
    std::printf("\n");
    const bool clean = refused == 0 && short_windows == 0 && unflagged == 0 && stamped_wrong == 0 &&
                       missing == 0 && errors.size() == 1 && errors.begin()->first == 0;
    std::printf("window: %s\n",
                clean ? "every burst at the start of its window, to the sample" : "NOT CLEAN");
    return clean ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        parse(argc, argv, options);
    } catch (const std::exception& e) {
        std::cerr << "hackrfduplex-check: " << e.what() << "\n" << kUsage;
        return 2;
    }
    if (options.help) {
        std::cout << kUsage;
        return 0;
    }
    if (options.tx_rate_hz == 0) {
        options.tx_rate_hz = options.rate_hz;
    }
    if ((options.timed_delay_us ? 1 : 0) + (options.window_delay_us ? 1 : 0) +
            (options.tx_tone_hz != 0 ? 1 : 0) >
        1) {
        std::cerr << "hackrfduplex-check: --tx-tone, --timed-bursts and --timed-window exclude "
                     "each other\n";
        return 2;
    }
    SoapySDR::registerLogHandler(&log_handler);

    SoapySDR::Kwargs args;
    args["driver"] = "hackrfduplex";
    if (!options.rx_serial.empty()) {
        args["rx_serial"] = options.rx_serial;
    }
    if (!options.tx_serial.empty()) {
        args["tx_serial"] = options.tx_serial;
    }
    if (!options.time_origin.empty()) {
        args["time_origin"] = options.time_origin;
    }

    SoapySDR::Device* device = nullptr;
    try {
        device = SoapySDR::Device::make(args);
    } catch (const std::exception& e) {
        std::cerr << "hackrfduplex-check: cannot open a pair: " << e.what() << "\n";
        return 1;
    }

    int status = 0;
    try {
        if (!options.clkout.empty()) {
            device->writeSetting("clkout", options.clkout);
        }
        std::cout << "clkout: " << device->readSetting("clkout") << "\n";
        std::cout << "hardware:\n";
        for (const auto& [key, value] : device->getHardwareInfo()) {
            std::cout << "  " << key << ": " << value << "\n";
        }

        device->setSampleRate(SOAPY_SDR_RX, 0, options.rate_hz);
        device->setFrequency(SOAPY_SDR_RX, 0, options.rx_frequency_hz);
        device->setGain(SOAPY_SDR_RX, 0, options.rx_gain_db);
        std::cout << "rx: " << device->getSampleRate(SOAPY_SDR_RX, 0) / 1e6 << " Msps, "
                  << device->getFrequency(SOAPY_SDR_RX, 0) / 1e6 << " MHz, filter "
                  << device->getBandwidth(SOAPY_SDR_RX, 0) / 1e6 << " MHz, gain "
                  << device->getGain(SOAPY_SDR_RX, 0) << " dB (LNA "
                  << device->getGain(SOAPY_SDR_RX, 0, "LNA") << ", VGA "
                  << device->getGain(SOAPY_SDR_RX, 0, "VGA") << ", AMP "
                  << device->getGain(SOAPY_SDR_RX, 0, "AMP") << ")\n";

        const bool transmit = options.tx_tone_hz != 0 || options.timed_delay_us.has_value() ||
                              options.window_delay_us.has_value();
        if (transmit) {
            device->setSampleRate(SOAPY_SDR_TX, 0, options.tx_rate_hz);
            device->setFrequency(SOAPY_SDR_TX, 0, options.tx_frequency_hz);
            device->setGain(SOAPY_SDR_TX, 0, options.tx_gain_db);
            std::cout << "tx: " << device->getSampleRate(SOAPY_SDR_TX, 0) / 1e6 << " Msps, "
                      << device->getFrequency(SOAPY_SDR_TX, 0) / 1e6 << " MHz, filter "
                      << device->getBandwidth(SOAPY_SDR_TX, 0) / 1e6 << " MHz, gain "
                      << device->getGain(SOAPY_SDR_TX, 0) << " dB (VGA "
                      << device->getGain(SOAPY_SDR_TX, 0, "VGA") << ", AMP "
                      << device->getGain(SOAPY_SDR_TX, 0, "AMP") << ")";
            if (options.timed_delay_us) {
                std::cout << ", timed bursts " << *options.timed_delay_us << " us after receive\n";
            } else if (options.window_delay_us) {
                std::cout << ", bursts " << *options.window_delay_us
                          << " us ahead, each caught by a timed receive window\n";
            } else {
                std::cout << ", tone at " << options.tx_tone_hz / 1e3 << " kHz, "
                          << options.tx_format << "\n";
            }
        } else {
            std::cout << "tx: off (give --tx-tone or --timed-bursts to transmit)\n";
        }
        if (options.timed_delay_us) {
            status = run_timed(device, options);
            SoapySDR::Device::unmake(device);
            return status;
        }
        if (options.window_delay_us) {
            status = run_window(device, options);
            SoapySDR::Device::unmake(device);
            return status;
        }

        const SoapySDR::Kwargs stream = stream_args(options);
        SoapySDR::Stream* rx = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {}, stream);
        SoapySDR::Stream* tx =
            transmit ? device->setupStream(SOAPY_SDR_TX, options.tx_format, {}, stream) : nullptr;
        const std::size_t rx_mtu = device->getStreamMTU(rx);

        const double expected_offset =
            options.tx_tone_hz + (options.tx_frequency_hz - options.rx_frequency_hz);
        Spectrum spectrum(options.rate_hz);
        ToneFinder finder(expected_offset, options.rate_hz);
        RxResult rx_result;
        TxResult tx_result;
        std::atomic<bool> stop{false};

        report_device_sensors(device, "before the start");
        std::thread tx_thread;
        if (transmit) {
            if (device->activateStream(tx) != 0) {
                throw std::runtime_error("TX stream did not activate");
            }
            tx_thread = std::thread([&] {
                const std::size_t mtu = device->getStreamMTU(tx);
                std::vector<std::complex<float>> buffer(mtu);
                std::vector<std::uint8_t> packed;
                const std::size_t stride = format_bytes(options.tx_format);
                const double step =
                    2.0 * std::numbers::pi * options.tx_tone_hz / options.tx_rate_hz;
                double phase = 0;
                while (!stop) {
                    for (auto& s : buffer) {
                        s = std::polar(0.9f, static_cast<float>(phase));
                        phase += step;
                        if (phase > 2.0 * std::numbers::pi) {
                            phase -= 2.0 * std::numbers::pi;
                        }
                    }
                    pack(buffer, options.tx_format, packed);
                    std::size_t sent = 0;
                    while (sent < buffer.size() && !stop) {
                        int flags = 0;
                        const void* buffs[] = {packed.data() + sent * stride};
                        const int n =
                            device->writeStream(tx, buffs, buffer.size() - sent, flags, 0, 200000);
                        if (n == SOAPY_SDR_TIMEOUT) {
                            ++tx_result.timeouts;
                            continue;
                        }
                        if (n < 0) {
                            tx_result.error = n;
                            tx_result.error_at = epoch_seconds();
                            stop = true;
                            break;
                        }
                        sent += static_cast<std::size_t>(n);
                        tx_result.samples += static_cast<std::uint64_t>(n);
                    }
                    std::size_t mask = 0;
                    int flags = 0;
                    long long t = 0;
                    if (device->readStreamStatus(tx, mask, flags, t, 0) == SOAPY_SDR_UNDERFLOW) {
                        ++tx_result.underflows;
                    }
                }
            });
        }

        if (device->activateStream(rx) != 0) {
            throw std::runtime_error("RX stream did not activate");
        }
        report_lo_locked(device, transmit);
        report_device_sensors(device, "running");
        if (transmit) {
            run_calibrations(device, options.calibrations);
        }
        std::vector<std::complex<float>> buffer(rx_mtu);
        const auto start = std::chrono::steady_clock::now();
        const auto deadline =
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(options.seconds));
        // A stream error on either side ends the run.
        while (std::chrono::steady_clock::now() < deadline && !stop) {
            int flags = 0;
            long long time_ns = 0;
            void* buffs[] = {buffer.data()};
            const int n = device->readStream(rx, buffs, buffer.size(), flags, time_ns, 200000);
            if (n == SOAPY_SDR_OVERFLOW) {
                ++rx_result.overflows;
                continue;
            }
            if (n == SOAPY_SDR_TIMEOUT) {
                ++rx_result.timeouts;
                continue;
            }
            if (n < 0) {
                rx_result.error = n;
                rx_result.error_at = epoch_seconds();
                break;
            }
            if (rx_result.first_time_ns < 0) {
                rx_result.first_time_ns = time_ns;
            }
            rx_result.last_time_ns = time_ns;
            rx_result.samples += static_cast<std::uint64_t>(n);
            spectrum.add(buffer.data(), static_cast<std::size_t>(n));
            if (transmit) {
                finder.add(buffer.data(), static_cast<std::size_t>(n));
            }
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stop = true;
        device->deactivateStream(rx);
        device->closeStream(rx);
        if (transmit) {
            tx_thread.join();
            device->deactivateStream(tx);
            device->closeStream(tx);
        }
        report_device_sensors(device, "stopped");

        std::printf(
            "rx: %llu samples in %.2f s = %.3f Msps (%.1f%% of nominal), %u overflows, %u "
            "timeouts\n",
            static_cast<unsigned long long>(rx_result.samples), elapsed,
            static_cast<double>(rx_result.samples) / elapsed / 1e6,
            100.0 * static_cast<double>(rx_result.samples) / elapsed / options.rate_hz,
            rx_result.overflows, rx_result.timeouts);
        if (rx_result.first_time_ns >= 0) {
            std::printf("rx: stream time %.6f s to %.6f s\n",
                        static_cast<double>(rx_result.first_time_ns) / 1e9,
                        static_cast<double>(rx_result.last_time_ns) / 1e9);
        }
        std::printf(
            "rx: mean %.1f dBFS, peak %.3f of full scale, median bin %.1f dBFS (%zu bins)\n",
            spectrum.mean_dbfs(), spectrum.peak(), spectrum.median_bin_dbfs(), Spectrum::kBins);
        std::printf("rx: strongest bins across the band (offset from %.4f MHz):\n",
                    options.rx_frequency_hz / 1e6);
        for (const Spectrum::Peak& p : spectrum.peaks(6)) {
            std::printf("  %+10.1f kHz  %6.1f dBFS\n", p.offset_hz / 1e3, p.level_dbfs);
        }
        if (transmit) {
            std::printf("tx: %llu samples = %.3f Msps, %u underflows, %u timeouts\n",
                        static_cast<unsigned long long>(tx_result.samples),
                        static_cast<double>(tx_result.samples) / elapsed / 1e6,
                        tx_result.underflows, tx_result.timeouts);
            std::printf("tone: expected at %+.1f kHz: %.1f dBFS in its %.0f Hz bin\n",
                        expected_offset / 1e3, spectrum.level_at(expected_offset),
                        options.rate_hz / static_cast<double>(Spectrum::kBins));
            // The transmitter's I/Q imbalance mirrors the tone about the TX
            // centre; a healthy pair keeps it about 20 dB down.
            const double image_offset =
                (options.tx_frequency_hz - options.rx_frequency_hz) - options.tx_tone_hz;
            std::printf("tone: image at %+.1f kHz: %.1f dBFS, %.1f dB below the tone\n",
                        image_offset / 1e3, spectrum.level_at(image_offset),
                        spectrum.level_at(expected_offset) - spectrum.level_at(image_offset));
            std::printf("tone: strongest within %.0f Hz of it, per second:\n", ToneFinder::kSpanHz);
            const auto peaks = finder.peaks();
            double lowest = 1e9;
            double highest = -1e9;
            for (std::size_t i = 0; i < peaks.size(); ++i) {
                std::printf("  %2zu s: %+8.1f Hz  %6.1f dBFS\n", i + 1, peaks[i].offset_hz,
                            peaks[i].level_dbfs);
                lowest = std::min(lowest, peaks[i].offset_hz);
                highest = std::max(highest, peaks[i].offset_hz);
            }
            if (!peaks.empty()) {
                std::printf("tone: offset spread %.1f Hz over %zu s\n", highest - lowest,
                            peaks.size());
            }
        }
        if (rx_result.error != 0) {
            std::printf("rx: stream error %s at epoch %.3f\n", SoapySDR::errToStr(rx_result.error),
                        rx_result.error_at);
            status = 1;
        }
        if (tx_result.error != 0) {
            std::printf("tx: stream error %s at epoch %.3f\n", SoapySDR::errToStr(tx_result.error),
                        tx_result.error_at);
            status = 1;
        }
        if (spectrum.peak() >= 0.99) {
            std::printf("rx: clipping, lower --rx-gain\n");
        }
    } catch (const std::exception& e) {
        std::cerr << "hackrfduplex-check: " << e.what() << "\n";
        status = 1;
    }
    SoapySDR::Device::unmake(device);
    return status;
}
