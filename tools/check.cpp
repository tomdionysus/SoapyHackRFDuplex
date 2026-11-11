// SPDX-License-Identifier: GPL-3.0-or-later
//
// hackrfduplex-check: opens a pair through SoapySDR, receives for a
// while, optionally transmits a tone meanwhile, and reports rates,
// drops and what the receiver heard. The transmitter stays off unless
// --tx-tone or --timed-bursts is given. --timed-bursts places bursts at
// receive timestamps plus a delay and reports where each one arrived.

#include <SoapySDR/Constants.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <array>
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
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
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
    // Off: the received samples are counted and nothing more, so the CPU
    // reported is the module's and SoapySDR's.
    bool analyse = true;
    // The receive stream's format; any but CF32 turns the analysis off.
    std::string rx_format = SOAPY_SDR_CF32;
    // CS8 only: read through SoapySDR's direct buffer access.
    bool direct = false;
    // The receive corrections, in a build with them (HACKRFDUPLEX_DSP).
    bool dc_removal = false;
    bool iq_balance = false;
    // Timed mode: when the origin is lost, calibrate again and go on,
    // counting each loss and whether calibration brought the origin back.
    bool recover = false;
    // Run the acceptance battery instead of one run; transmit lines only
    // with `transmit`.
    bool acceptance = false;
    bool transmit = false;
    // Timed mode: a line of running totals this often, in seconds; 0 none.
    double progress_seconds = 0;
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
    "  --recover         with --timed-bursts: when the origin is lost, calibrate\n"
    "                    again and go on; the run passes if every loss was recovered\n"
    "  --progress S      with --timed-bursts: print running totals every S seconds\n"
    "  --acceptance      run the hardware acceptance battery, one run of this tool\n"
    "                    per line, and print PASS or FAIL for each against the\n"
    "                    figures in docs/design.md; receive only unless --transmit\n"
    "  --transmit        with --acceptance: the lines that transmit too (TX gain 0)\n"
    "  --timed-window US transmit a burst US microseconds after the time now and\n"
    "                    open a timed receive window just around it, again and\n"
    "                    again, and report where in each window it arrived\n"
    "                    (transmits; excludes --tx-tone and --timed-bursts)\n"
    "  --no-analysis     count the received samples and nothing more, so the CPU\n"
    "                    reported is the module's (no spectrum, no tone report)\n"
    "  --rx-format F     CS8, CS16, CF32 or CF64 for the receive stream (default\n"
    "                    CF32); any but CF32 implies --no-analysis\n"
    "  --direct          receive through direct buffer access (CS8 only)\n"
    "  --dc-removal      remove the receiver's DC offset automatically (a module\n"
    "                    built with HACKRFDUPLEX_DSP)\n"
    "  --iq-balance      correct the receiver's I/Q balance automatically (likewise)\n"
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
        } else if (arg == "--no-analysis") {
            options.analyse = false;
        } else if (arg == "--rx-format") {
            options.rx_format = value();
            format_bytes(options.rx_format);
        } else if (arg == "--direct") {
            options.direct = true;
        } else if (arg == "--dc-removal") {
            options.dc_removal = true;
        } else if (arg == "--iq-balance") {
            options.iq_balance = true;
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
        } else if (arg == "--acceptance") {
            options.acceptance = true;
        } else if (arg == "--transmit") {
            options.transmit = true;
        } else if (arg == "--recover") {
            options.recover = true;
        } else if (arg == "--progress") {
            options.progress_seconds = std::stod(value());
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

    explicit Spectrum(double rate_hz)
        : rate_hz_(rate_hz), power_(kBins, 0.0), block_(kBins), window_(kBins) {
        // Hann, so a tone does not leak across the band.
        for (std::size_t k = 0; k < kBins; ++k) {
            window_[k] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(k) /
                                              static_cast<double>(kBins));
        }
    }

    void add(const std::complex<float>* samples, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::complex<double> s(samples[i].real(), samples[i].imag());
            total_power_ += std::norm(s);
            peak_ = std::max({peak_, std::fabs(s.real()), std::fabs(s.imag())});
            ++count_;
            block_[filled_] = s * window_[filled_];
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
    std::vector<double> window_;
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
            const auto bins = static_cast<int>(kSpanHz / kBinHz);
            for (int bin = -bins; bin <= bins; ++bin) {
                const double f = bin * kBinHz;
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

// User and system time of the whole process: the module's threads, libusb's
// and this tool's.
double cpu_seconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const auto seconds = [](const timeval& tv) {
        return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
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
    // How long the writer ran: from the stream's activation, so through the
    // second activation and any calibrations, which the receive loop's
    // time leaves out.
    double seconds = 0;
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
    // --recover: origins lost, and calibrations that found one again.
    unsigned origins_lost = 0;
    unsigned recovered = 0;
    // A read or write ended the run early.
    bool stream_error = false;
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
    const double delay_us = options.timed_delay_us.value_or(0.0);
    const long long delay = std::llround(delay_us * 1e-6 * rate);
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
    bool was_caught_up = false;
    // Calibrates again when the origin has gone; true when it is back.
    const auto recover = [&]() {
        if (!options.recover || device->hasHardwareTime()) {
            return;
        }
        ++result.origins_lost;
        device->writeSetting("calibrate", "");
        const auto value = take_tx_of_rx();
        if (device->hasHardwareTime()) {
            ++result.recovered;
            std::printf(
                "timed: origin lost after %.0f s; calibrated again: transmit position = "
                "receive position %+lld\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                value.value_or(0));
        } else {
            std::printf(
                "timed: origin lost after %.0f s; calibration found none\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        std::fflush(stdout);
        // Bursts already scheduled were timed on the old origin.
        result.lost += static_cast<unsigned>(pending.size());
        pending.clear();
        history.clear();
        previous_read_ns.reset();
    };
    auto next_progress = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(options.progress_seconds));
    while (std::chrono::steady_clock::now() < deadline) {
        if (options.progress_seconds > 0 && std::chrono::steady_clock::now() >= next_progress) {
            next_progress += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(options.progress_seconds));
            std::printf(
                "timed: %.0f s: %u written, %u found, %u not found, %u lost, %u late writes, %u "
                "overflows, %u time errors, %u underflows, %u origins lost, %u recovered\n",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                result.written, result.found, result.missing, result.lost, result.late_writes,
                result.overflows, result.time_errors, result.underflows, result.origins_lost,
                result.recovered);
            std::fflush(stdout);
        }
        int flags = 0;
        long long time_ns = 0;
        void* buffs[] = {buffer.data()};
        const long long clock_ns = device->getHardwareTime();
        const int n = device->readStream(rx, buffs, mtu, flags, time_ns, 200000);
        if (n == SOAPY_SDR_OVERFLOW) {
            ++result.overflows;
            previous_read_ns.reset();
            was_caught_up = false;
            recover();
            continue;
        }

        if (n == SOAPY_SDR_TIMEOUT) {
            continue;
        }
        if (n < 0) {
            std::printf("timed: read error %s\n", SoapySDR::errToStr(n));
            result.stream_error = true;
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
        history.insert(history.end(), buffer.begin(),
                       buffer.begin() + 2 * static_cast<std::ptrdiff_t>(n));

        // A read from a backlog (the ring fills while activation calibrates)
        // is not live; stamping from it measures the backlog, not the lead.
        // Nor is the read that empties the backlog: its transfer arrived
        // while the older ones were being read, up to a transfer before now,
        // so a lead counted from it is short by that much. A read is live
        // when the one before it had caught up, so the reader waited for it.
        const long long newest = SoapySDR::timeNsToTicks(device->getHardwareTime(), rate);
        const bool caught_up = newest - (tick + n) < static_cast<long long>(mtu);
        const bool live = caught_up && was_caught_up;
        was_caught_up = caught_up;
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
        if (live && at >= last_scheduled + 4 * chips && device->hasHardwareTime()) {
            int write_flags = SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST;
            const void* out[] = {burst.data()};
            const int w = device->writeStream(tx, out, burst.size() / 2, write_flags,
                                              SoapySDR::ticksToTimeNs(at, rate), 0);
            if (w == SOAPY_SDR_TIME_ERROR) {
                ++result.late_writes;
                std::printf(
                    "timed: late write %.3f s in, burst %u, its read ending %lld samples before "
                    "the newest received, %u reads from a backlog so far\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
                    result.written + 1, newest - (tick + n), result.behind);
            } else if (w == SOAPY_SDR_TIMEOUT) {
                ++result.write_timeouts;
            } else if (w < 0) {
                std::printf("timed: write error %s\n", SoapySDR::errToStr(w));
                result.stream_error = true;
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
        recover();

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

    std::printf("timed: delay %.0f us = %lld samples at %.3f Msps\n", delay_us, delay, rate / 1e6);
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
    if (options.recover) {
        std::printf("timed: %u origins lost, %u found again by calibrating\n", result.origins_lost,
                    result.recovered);
    }
    std::printf("timed: arrival against the time asked, in samples:");
    for (const auto& [error, count] : result.errors) {
        std::printf(" %+lld (x%u)", error, count);
    }
    std::printf("\n");
    // With --recover, a lost origin is a pass when calibrating found it
    // again; the time errors that reported each loss go with it.
    const bool time_ok = options.recover ? result.recovered == result.origins_lost &&
                                               result.time_errors <= result.origins_lost
                                         : result.time_errors == 0;
    const bool clean = !result.stream_error && result.late_writes == 0 && time_ok &&
                       result.missing == 0 && result.errors.size() <= 1 &&
                       result.clock_outside == 0;
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
    const double delay_us = options.window_delay_us.value_or(0.0);
    const long long delay = std::llround(delay_us * 1e-6 * rate);
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
        delay_us, delay, rate / 1e6, length, kMargin);
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

namespace {

// The acceptance battery (TODO/ACTIVE.md, item 15): each line one run of
// this tool as a child, its report read back and judged against the
// figures in docs/design.md.
struct Child {
    int status = -1;
    std::string output;
};

Child run_child(const std::string& self, const std::vector<std::string>& args) {
    std::string command = self;
    for (const std::string& a : args) {
        command += " " + a;
    }
    command += " 2>&1";
    Child child;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return child;
    }
    std::array<char, 4096> chunk{};
    while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr) {
        child.output += chunk.data();
    }
    const int status = pclose(pipe);
    child.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return child;
}

// The first number captured by `pattern` in `text`.
std::optional<double> number(const std::string& text, const std::string& pattern) {
    std::smatch match;
    if (std::regex_search(text, match, std::regex(pattern)) && match.size() > 1) {
        return std::stod(match[1].str());
    }
    return std::nullopt;
}

bool has(const std::string& text, const std::string& what) {
    return text.find(what) != std::string::npos;
}

int run_acceptance(const std::string& self, const Options& options) {
    const std::string clkout = options.clkout.empty() ? "rx" : options.clkout;
    std::vector<std::string> common{"--clkout", clkout};
    if (!options.rx_serial.empty()) {
        common.insert(common.end(), {"--rx-serial", options.rx_serial});
    }
    if (!options.tx_serial.empty()) {
        common.insert(common.end(), {"--tx-serial", options.tx_serial});
    }
    unsigned passed = 0;
    unsigned failed = 0;
    const auto line = [&](bool ok, const std::string& name, const std::string& detail) {
        std::printf("%s  %s: %s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        std::fflush(stdout);
        ++(ok ? passed : failed);
    };
    const auto run = [&](std::vector<std::string> args) {
        std::vector<std::string> all = common;
        all.insert(all.end(), args.begin(), args.end());
        return run_child(self, all);
    };
    const auto fmt = [](const char* format, auto... values) {
        std::array<char, 256> text{};
        std::snprintf(text.data(), text.size(), format, values...);
        return std::string(text.data());
    };
    const std::string kRxRate =
        R"(rx: \d+ samples in [\d.]+ s = [\d.]+ Msps \(([\d.]+)% of nominal\))";
    const std::string kOverflows = R"((\d+) overflows, \d+ timeouts)";

    std::printf("acceptance: clkout %s, %s\n", clkout.c_str(),
                options.transmit ? "receive and transmit lines (TX gain 0)"
                                 : "receive lines only (add --transmit for the rest)");
    std::fflush(stdout);

    // Receive, at the rates the design names, and at the low end.
    for (const auto& [rate, seconds] : std::vector<std::pair<std::string, std::string>>{
             {"2e6", "10"}, {"8e6", "10"}, {"16e6", "10"}, {"0.2e6", "10"}}) {
        const Child c = run({"--rate", rate, "--seconds", seconds, "--no-analysis"});
        const auto pct = number(c.output, kRxRate);
        const auto over = number(c.output, kOverflows);
        // Samples arrive a transfer (131072) at a time: 655 ms at 200 ksps,
        // so a run's count is short by up to one transfer of its length.
        const double transfer_pct = 100.0 * 131072.0 / (std::stod(rate) * std::stod(seconds));
        line(c.status == 0 && pct && *pct >= std::min(99.0, 100.0 - transfer_pct) && over &&
                 *over == 0,
             fmt("receive %s sps %s s", rate.c_str(), seconds.c_str()),
             pct && over ? fmt("%.1f%% of nominal, %.0f overflows", *pct, *over)
                         : "no report (exit " + std::to_string(c.status) + ")");
    }
    {
        // 20 Msps: the design's limit on a host whose USB carries it; the
        // Raspberry Pi 5 target does not (docs/design.md).
        const Child c = run({"--rate", "20e6", "--seconds", "10", "--no-analysis"});
        const auto pct = number(c.output, kRxRate);
        const auto over = number(c.output, kOverflows);
        std::printf("INFO  receive 20e6 sps 10 s: %s (the host's limit, not judged)\n",
                    pct && over ? fmt("%.1f%% of nominal, %.0f overflows", *pct, *over).c_str()
                                : "no report");
    }

    // The sensors follow the reference: the boards on their own clocks,
    // then on the shared one again. Receive only, the transmitting board,
    // whose clock is the one that switches, never starts; with --transmit
    // a tone makes it start, and the switch itself is judged each way.
    {
        // No analysis: these lines judge the switch and the sensors, and
        // the analysis at 8 Msps takes more than a core on the target.
        std::vector<std::string> args{"--rate", "8e6", "--seconds", "3", "--no-analysis"};
        if (options.transmit) {
            args.insert(args.end(), {"--tx-tone", "100e3", "--tx-gain", "0"});
            // Start from the shared reference, so the next run switches.
            run(args);
        }
        std::vector<std::string> off_args{"--clkout", "off"};
        for (const std::string& key : {std::string("--rx-serial"), std::string("--tx-serial")}) {
            const std::string& value = key == "--rx-serial" ? options.rx_serial : options.tx_serial;
            if (!value.empty()) {
                off_args.insert(off_args.end(), {key, value});
            }
        }
        off_args.insert(off_args.end(), args.begin(), args.end());
        const auto judge = [&](const Child& c, bool shared) {
            // Overflows, not the rate. With an origin to find, one overflow
            // at the start is the activation backlog: this tool activates
            // transmit first, its writer fills the transmit ring, and the
            // calibration burst waits behind it for 0.8 s or so at 8 Msps,
            // longer than the receive ring holds (docs/design.md).
            const auto over = number(c.output, kOverflows);
            const double allowed = shared && options.transmit ? 1 : 0;
            bool ok = c.status == 0 && over && *over <= allowed &&
                      has(c.output, "rx lo_locked true") &&
                      has(c.output, shared ? "sensors running: ref_locked true"
                                           : "sensors running: ref_locked false");
            std::string detail = shared ? "ref_locked true" : "ref_locked false";
            if (over) {
                detail += fmt(", %.0f overflows", *over);
            }
            if (options.transmit) {
                const bool switched = has(c.output, "TX board changed reference clock");
                ok = ok && switched && has(c.output, "tx lo_locked true");
                detail += switched ? ", TX board's switch handled, tx lo_locked true"
                                   : ", no switch on the TX board";
            }
            return std::make_pair(ok, detail);
        };
        const Child off = run_child(self, off_args);
        const auto [off_ok, off_detail] = judge(off, false);
        line(off_ok,
             options.transmit ? "reference switch to the boards' own clocks"
                              : "sensors with the boards on their own clocks",
             off_detail);
        const Child on = run(args);
        const auto [on_ok, on_detail] = judge(on, true);
        line(on_ok,
             options.transmit
                 ? "reference switch back to the shared reference (clkout " + clkout + ")"
                 : "sensors with the shared reference (clkout " + clkout + ")",
             on_detail);
    }

    if (options.transmit) {
        // A tone in each format: at its sent offset every second, the four
        // levels within 0.3 dB of each other. At 2 Msps, where the tool's
        // spectrum analysis keeps up on the target (at 8 it takes more than
        // a core, drops samples, and the gaps move the estimate by a hertz
        // or two); the first second, which holds the start, is not judged.
        std::vector<double> levels;
        for (const std::string format : {"CS8", "CS16", "CF32", "CF64"}) {
            const Child c = run({"--rate", "2e6", "--seconds", "6", "--tx-tone", "100e3",
                                 "--tx-gain", "0", "--tx-format", format});
            const auto level =
                number(c.output, R"(tone: expected at [-+\d.]+ kHz: ([-\d.]+) dBFS)");
            const auto under =
                number(c.output, R"(tx: \d+ samples in [\d.]+ s = [\d.]+ Msps, (\d+) underflows)");
            const auto over = number(c.output, kOverflows);
            std::vector<double> offsets;
            const std::regex second(R"(\n +(\d+) s: +([-+\d.]+) Hz)");
            for (auto it = std::sregex_iterator(c.output.begin(), c.output.end(), second);
                 it != std::sregex_iterator(); ++it) {
                if (std::stoi((*it)[1].str()) >= 2) {
                    offsets.push_back(std::stod((*it)[2].str()));
                }
            }
            const bool on_offset =
                offsets.size() >= 3 &&
                std::all_of(offsets.begin(), offsets.end(), [](double hz) { return hz == 0; });
            if (level) {
                levels.push_back(*level);
            }
            line(c.status == 0 && level && on_offset && under && *under == 0 && over && *over == 0,
                 "tone in " + format,
                 level ? fmt("%.1f dBFS, %zu seconds judged, %s, %.0f overflows", *level,
                             offsets.size(),
                             on_offset ? "every one at +0.0 Hz" : "NOT all at +0.0 Hz",
                             over.value_or(-1))
                       : "no tone report");
        }
        if (levels.size() == 4) {
            const auto [low, high] = std::minmax_element(levels.begin(), levels.end());
            line(*high - *low <= 0.3, "tone levels across the formats",
                 fmt("within %.2f dB", *high - *low));
        } else {
            line(false, "tone levels across the formats", "not every format reported");
        }

        // The origin, repeatable, and the time sensors.
        {
            const Child c = run({"--rate", "8e6", "--seconds", "2", "--tx-tone", "100e3",
                                 "--tx-gain", "0", "--calibrations", "5"});
            const auto spread = number(c.output, R"(time: origin spread (\d+) samples)");
            line(c.status == 0 && spread && *spread == 0, "origin repeatable over 6 findings",
                 spread ? fmt("spread %.0f samples", *spread) : "no origin found");
            line(has(c.output, "sensors running: ref_locked true, time_locked true") &&
                     has(c.output, "tx lo_locked true"),
                 "time and lock sensors with both streams running",
                 "time_locked, ref_locked, tx lo_locked true");
        }

        // Timed bursts at +0, at the lead the design gives.
        for (const auto& [rate, delay, seconds] :
             std::vector<std::tuple<std::string, std::string, std::string>>{
                 {"8e6", "90112", "60"}, {"16e6", "45056", "30"}}) {
            const Child c = run({"--rate", rate, "--timed-bursts", delay, "--seconds", seconds});
            const auto written = number(c.output, R"(timed: (\d+) bursts written)");
            line(c.status == 0 && has(c.output, "every burst on time and in place"),
                 fmt("timed bursts at %s sps %s s", rate.c_str(), seconds.c_str()),
                 written ? fmt("%.0f bursts, every one at +0", *written) : "no timed report");
        }

        // The transmitter's low end.
        {
            const Child c =
                run({"--rate", "1.2e6", "--seconds", "4", "--tx-tone", "100e3", "--tx-gain", "0"});
            const auto spread = number(c.output, R"(tone: offset spread ([\d.]+) Hz)");
            const auto over = number(c.output, kOverflows);
            line(c.status == 0 && spread && *spread == 0 && over && *over == 0,
                 "transmit and receive at 1.2e6 sps",
                 spread ? fmt("tone offset spread %.1f Hz", *spread) : "no tone report");
        }
    }

    std::printf("acceptance: %u passed, %u failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
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
    if (options.acceptance) {
        try {
            return run_acceptance(argv[0], options);
        } catch (const std::exception& e) {
            std::cerr << "hackrfduplex-check: acceptance: " << e.what() << "\n";
            return 1;
        }
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
        if (options.direct && options.rx_format != SOAPY_SDR_CS8) {
            throw std::invalid_argument("--direct needs --rx-format CS8");
        }
        if (options.rx_format != SOAPY_SDR_CF32) {
            options.analyse = false;
        }
        SoapySDR::Stream* rx = device->setupStream(SOAPY_SDR_RX, options.rx_format, {}, stream);
        SoapySDR::Stream* tx =
            transmit ? device->setupStream(SOAPY_SDR_TX, options.tx_format, {}, stream) : nullptr;
        const std::size_t rx_mtu = device->getStreamMTU(rx);
        if (options.dc_removal) {
            if (!device->hasDCOffsetMode(SOAPY_SDR_RX, 0)) {
                throw std::runtime_error(
                    "--dc-removal: this module has no receive corrections (build it with "
                    "-DHACKRFDUPLEX_DSP=ON)");
            }
            device->setDCOffsetMode(SOAPY_SDR_RX, 0, true);
        }
        if (options.iq_balance) {
            if (!device->hasIQBalanceMode(SOAPY_SDR_RX, 0)) {
                throw std::runtime_error(
                    "--iq-balance: this module has no receive corrections (build it with "
                    "-DHACKRFDUPLEX_DSP=ON)");
            }
            device->setIQBalanceMode(SOAPY_SDR_RX, 0, true);
        }

        const double expected_offset =
            options.tx_tone_hz + (options.tx_frequency_hz - options.rx_frequency_hz);
        Spectrum spectrum(options.rate_hz);
        ToneFinder finder(expected_offset, options.rate_hz);
        RxResult rx_result;
        TxResult tx_result;
        std::atomic<bool> stop{false};

        report_device_sensors(device, "before the start");
        std::thread tx_thread;
        // If anything below throws, the writer is stopped and joined first,
        // rather than std::terminate with the transmitter running.
        struct JoinOnThrow {
            std::atomic<bool>& stop;
            std::thread& thread;
            ~JoinOnThrow() {
                if (thread.joinable()) {
                    stop = true;
                    thread.join();
                }
            }
        } const join_on_throw{stop, tx_thread};
        if (transmit) {
            if (device->activateStream(tx) != 0) {
                throw std::runtime_error("TX stream did not activate");
            }
            tx_thread = std::thread([&] {
                const auto began = std::chrono::steady_clock::now();
                const std::size_t mtu = device->getStreamMTU(tx);
                std::vector<std::complex<float>> buffer(mtu);
                std::vector<std::uint8_t> packed;
                const std::size_t stride = format_bytes(options.tx_format);
                // A phasor turned once per sample and set back to its
                // amplitude once per buffer: no trigonometry per sample.
                const std::complex<double> turn = std::polar(
                    1.0, 2.0 * std::numbers::pi * options.tx_tone_hz / options.tx_rate_hz);
                std::complex<double> phasor(0.9, 0.0);
                while (!stop) {
                    for (auto& s : buffer) {
                        s = std::complex<float>(phasor);
                        phasor *= turn;
                    }
                    phasor *= 0.9 / std::abs(phasor);
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
                tx_result.seconds =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            });
        }

        const auto activating = std::chrono::steady_clock::now();
        if (device->activateStream(rx) != 0) {
            throw std::runtime_error("RX stream did not activate");
        }
        // The second activation is the one that calibrates.
        std::printf(
            "activation: rx in %.3f s%s\n",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - activating).count(),
            transmit ? ", after tx, so with the calibration" : "");
        report_lo_locked(device, transmit);
        report_device_sensors(device, "running");
        if (transmit) {
            run_calibrations(device, options.calibrations);
        }
        std::vector<std::uint8_t> raw(rx_mtu * format_bytes(options.rx_format));
        const auto* samples = reinterpret_cast<const std::complex<float>*>(raw.data());
        const double cpu_start = cpu_seconds();
        const auto start = std::chrono::steady_clock::now();
        const auto deadline =
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(options.seconds));
        // A stream error on either side ends the run.
        while (std::chrono::steady_clock::now() < deadline && !stop) {
            int flags = 0;
            long long time_ns = 0;
            int n = 0;
            if (options.direct) {
                std::size_t handle = 0;
                const void* held[] = {nullptr};
                n = device->acquireReadBuffer(rx, handle, held, flags, time_ns, 200000);
                if (n > 0) {
                    device->releaseReadBuffer(rx, handle);
                }
            } else {
                void* buffs[] = {raw.data()};
                n = device->readStream(rx, buffs, rx_mtu, flags, time_ns, 200000);
            }
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
            if (options.analyse) {
                spectrum.add(samples, static_cast<std::size_t>(n));
                if (transmit) {
                    finder.add(samples, static_cast<std::size_t>(n));
                }
            }
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double cpu = cpu_seconds() - cpu_start;
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
        std::printf("cpu: %.2f s in %.2f s, %.0f%% of one core (the whole process%s)\n", cpu,
                    elapsed, 100.0 * cpu / elapsed, options.analyse ? ", analysis included" : "");
        if (options.analyse) {
            std::printf(
                "rx: mean %.1f dBFS, peak %.3f of full scale, median bin %.1f dBFS (%zu bins)\n",
                spectrum.mean_dbfs(), spectrum.peak(), spectrum.median_bin_dbfs(), Spectrum::kBins);
            std::printf("rx: centre bin (the DC offset) %.1f dBFS, DC removal %s\n",
                        spectrum.level_at(0.0), options.dc_removal ? "on" : "off");
            if (options.iq_balance) {
                const std::complex<double> b = device->getIQBalance(SOAPY_SDR_RX, 0);
                std::printf("rx: I/Q balance correction found %+.5f %+.5fj\n", b.real(), b.imag());
            }
            std::printf("rx: strongest bins across the band (offset from %.4f MHz):\n",
                        options.rx_frequency_hz / 1e6);
            for (const Spectrum::Peak& p : spectrum.peaks(6)) {
                std::printf("  %+10.1f kHz  %6.1f dBFS\n", p.offset_hz / 1e3, p.level_dbfs);
            }
        }
        if (transmit) {
            std::printf("tx: %llu samples in %.2f s = %.3f Msps, %u underflows, %u timeouts\n",
                        static_cast<unsigned long long>(tx_result.samples), tx_result.seconds,
                        tx_result.seconds > 0
                            ? static_cast<double>(tx_result.samples) / tx_result.seconds / 1e6
                            : 0.0,
                        tx_result.underflows, tx_result.timeouts);
        }
        if (transmit && options.analyse) {
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
        if (options.analyse && spectrum.peak() >= 0.99) {
            std::printf("rx: clipping, lower --rx-gain\n");
        }
    } catch (const std::exception& e) {
        std::cerr << "hackrfduplex-check: " << e.what() << "\n";
        status = 1;
    }
    SoapySDR::Device::unmake(device);
    return status;
}
