// SPDX-License-Identifier: GPL-3.0-or-later
//
// hackrfduplex-check: opens a pair through SoapySDR, receives for a
// while, optionally transmits a tone meanwhile, and reports rates,
// drops and what the receiver heard. The transmitter stays off unless
// --tx-tone is given.

#include <SoapySDR/Constants.h>

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string rx_serial;
    std::string tx_serial;
    double rate_hz = 8e6;
    double rx_frequency_hz = 868.3e6;
    double tx_frequency_hz = 868.3e6;
    double rx_gain_db = 32;
    double tx_gain_db = 0;
    // 0: never transmit. Otherwise a tone this far from the TX centre.
    double tx_tone_hz = 0;
    double seconds = 5;
    bool help = false;
};

const char* const kUsage =
    "usage: hackrfduplex-check [options]\n"
    "\n"
    "Receives through the hackrfduplex SoapySDR module and reports. Nothing is\n"
    "transmitted unless --tx-tone is given.\n"
    "\n"
    "  --rx-serial S     board that receives (suffix of its serial)\n"
    "  --tx-serial S     board that transmits\n"
    "  --rate HZ         sample rate, both directions (default 8e6)\n"
    "  --rx-freq HZ      receive centre frequency (default 868.3e6)\n"
    "  --tx-freq HZ      transmit centre frequency (default 868.3e6)\n"
    "  --rx-gain DB      overall receive gain (default 32)\n"
    "  --tx-gain DB      overall transmit gain (default 0)\n"
    "  --tx-tone HZ      transmit a tone this far from the TX centre (default off)\n"
    "  --seconds S       how long to run (default 5)\n"
    "  --help\n";

bool parse(int argc, char** argv, Options& options) {
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
        } else {
            throw std::invalid_argument("unknown option " + arg);
        }
    }
    return true;
}

// Power at one frequency, the Goertzel way, so a tone shows above the
// broadband floor without an FFT library.
class ToneMeter {
public:
    ToneMeter(double tone_hz, double rate_hz)
        : rotation_(std::polar(1.0, -2.0 * std::numbers::pi * tone_hz / rate_hz)) {}

    void add(const std::complex<float>* samples, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            const std::complex<double> s(samples[i].real(), samples[i].imag());
            sum_ += s * phase_;
            phase_ *= rotation_;
            power_ += std::norm(s);
            peak_ = std::max({peak_, std::fabs(s.real()), std::fabs(s.imag())});
            ++count_;
        }
        // Keep the phasor on the unit circle.
        phase_ /= std::abs(phase_);
    }

    double tone_dbfs() const {
        if (count_ == 0) {
            return -200;
        }
        return 20 * std::log10(std::abs(sum_) / static_cast<double>(count_) + 1e-12);
    }
    double mean_dbfs() const {
        if (count_ == 0) {
            return -200;
        }
        return 10 * std::log10(power_ / static_cast<double>(count_) + 1e-24);
    }
    double peak() const { return peak_; }

private:
    std::complex<double> rotation_;
    std::complex<double> phase_{1.0, 0.0};
    std::complex<double> sum_{0.0, 0.0};
    double power_ = 0;
    double peak_ = 0;
    std::uint64_t count_ = 0;
};

struct RxResult {
    std::uint64_t samples = 0;
    unsigned overflows = 0;
    unsigned timeouts = 0;
    int error = 0;
    long long first_time_ns = -1;
    long long last_time_ns = -1;
};

struct TxResult {
    std::uint64_t samples = 0;
    unsigned underflows = 0;
    unsigned timeouts = 0;
    int error = 0;
};

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

    SoapySDR::Kwargs args;
    args["driver"] = "hackrfduplex";
    if (!options.rx_serial.empty()) {
        args["rx_serial"] = options.rx_serial;
    }
    if (!options.tx_serial.empty()) {
        args["tx_serial"] = options.tx_serial;
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

        const bool transmit = options.tx_tone_hz != 0;
        if (transmit) {
            device->setSampleRate(SOAPY_SDR_TX, 0, options.rate_hz);
            device->setFrequency(SOAPY_SDR_TX, 0, options.tx_frequency_hz);
            device->setGain(SOAPY_SDR_TX, 0, options.tx_gain_db);
            std::cout << "tx: " << device->getSampleRate(SOAPY_SDR_TX, 0) / 1e6 << " Msps, "
                      << device->getFrequency(SOAPY_SDR_TX, 0) / 1e6 << " MHz, filter "
                      << device->getBandwidth(SOAPY_SDR_TX, 0) / 1e6 << " MHz, gain "
                      << device->getGain(SOAPY_SDR_TX, 0) << " dB (VGA "
                      << device->getGain(SOAPY_SDR_TX, 0, "VGA") << ", AMP "
                      << device->getGain(SOAPY_SDR_TX, 0, "AMP") << "), tone at "
                      << options.tx_tone_hz / 1e3 << " kHz\n";
        } else {
            std::cout << "tx: off (give --tx-tone to transmit)\n";
        }

        SoapySDR::Stream* rx = device->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
        SoapySDR::Stream* tx =
            transmit ? device->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32) : nullptr;
        const std::size_t rx_mtu = device->getStreamMTU(rx);

        const double tone_offset =
            options.tx_tone_hz + (options.tx_frequency_hz - options.rx_frequency_hz);
        ToneMeter meter(tone_offset, options.rate_hz);
        RxResult rx_result;
        TxResult tx_result;
        std::atomic<bool> stop{false};

        std::thread tx_thread;
        if (transmit) {
            if (device->activateStream(tx) != 0) {
                throw std::runtime_error("TX stream did not activate");
            }
            tx_thread = std::thread([&] {
                const std::size_t mtu = device->getStreamMTU(tx);
                std::vector<std::complex<float>> buffer(mtu);
                const double step = 2.0 * std::numbers::pi * options.tx_tone_hz / options.rate_hz;
                double phase = 0;
                while (!stop) {
                    for (auto& s : buffer) {
                        s = std::polar(0.9f, static_cast<float>(phase));
                        phase += step;
                        if (phase > 2.0 * std::numbers::pi) {
                            phase -= 2.0 * std::numbers::pi;
                        }
                    }
                    std::size_t sent = 0;
                    while (sent < buffer.size() && !stop) {
                        int flags = 0;
                        const void* buffs[] = {buffer.data() + sent};
                        const int n =
                            device->writeStream(tx, buffs, buffer.size() - sent, flags, 0, 200000);
                        if (n == SOAPY_SDR_TIMEOUT) {
                            ++tx_result.timeouts;
                            continue;
                        }
                        if (n < 0) {
                            tx_result.error = n;
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
        std::vector<std::complex<float>> buffer(rx_mtu);
        const auto start = std::chrono::steady_clock::now();
        const auto deadline =
            start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(options.seconds));
        while (std::chrono::steady_clock::now() < deadline) {
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
                break;
            }
            if (rx_result.first_time_ns < 0) {
                rx_result.first_time_ns = time_ns;
            }
            rx_result.last_time_ns = time_ns;
            rx_result.samples += static_cast<std::uint64_t>(n);
            meter.add(buffer.data(), static_cast<std::size_t>(n));
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
        std::printf("rx: mean %.1f dBFS, peak %.3f of full scale, tone at %+.1f kHz: %.1f dBFS\n",
                    meter.mean_dbfs(), meter.peak(), tone_offset / 1e3, meter.tone_dbfs());
        if (transmit) {
            std::printf("tx: %llu samples = %.3f Msps, %u underflows, %u timeouts\n",
                        static_cast<unsigned long long>(tx_result.samples),
                        static_cast<double>(tx_result.samples) / elapsed / 1e6,
                        tx_result.underflows, tx_result.timeouts);
        }
        if (rx_result.error != 0) {
            std::printf("rx: stream error %s\n", SoapySDR::errToStr(rx_result.error));
            status = 1;
        }
        if (tx_result.error != 0) {
            std::printf("tx: stream error %s\n", SoapySDR::errToStr(tx_result.error));
            status = 1;
        }
        if (meter.peak() >= 0.99) {
            std::printf("rx: clipping, lower --rx-gain\n");
        }
    } catch (const std::exception& e) {
        std::cerr << "hackrfduplex-check: " << e.what() << "\n";
        status = 1;
    }
    SoapySDR::Device::unmake(device);
    return status;
}
