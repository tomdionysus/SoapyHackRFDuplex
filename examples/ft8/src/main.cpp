// SPDX-License-Identifier: GPL-3.0-or-later
//
// hackrfduplex-ft8: an example SoapySDR client for the HackRF pair that
// decodes FT8 and prints what it hears, one line per message as WSJT-X
// does. Two ways to run it:
//
//   receive only (the default): each UTC 15 s slot from the dial frequency,
//   which needs an antenna and a band with FT8 on it;
//
//   --loopback: the pair transmits a frame of --message at a time it
//   chooses and decodes it back, which needs only the pair and a shielded
//   bench, and transmits (TX gain 0 unless --tx-gain says otherwise).
//
// Both directions run at 1.92 Msps, the lowest rate both take that is a
// whole multiple of the FT8 chain's 48 ksps; the receive centre sits at
// dial + 1500 Hz so FT8's 3 kHz of audio lies either side of it.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Time.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "decimator.hpp"
#include "ft8/codec.h"
#include "ft8/decoder.h"
#include "message_text.hpp"
#include "resampler.hpp"

namespace {

using Sample = std::complex<float>;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Options {
    std::string args = "driver=hackrfduplex,clkout=rx";
    double dial_hz = 144.174e6;
    double rx_gain_db = 32;
    double tx_gain_db = 0;
    int slots = 0;  // 0: until interrupted (receive only), 3 (loopback)
    bool loopback = false;
    std::string message = "CQ K1ABC FN42";
    float audio_hz = 1000;  // where in the 3 kHz of audio the loopback frame sits
};

const char* const kUsage =
    "usage: hackrfduplex-ft8 [options]\n"
    "\n"
    "Decodes FT8 through the hackrfduplex pair and prints what it hears.\n"
    "\n"
    "  --args A        SoapySDR device arguments (default driver=hackrfduplex,clkout=rx)\n"
    "  --dial HZ       the FT8 dial frequency (default 144174000; with --loopback\n"
    "                  any frequency the bench allows, 868300000 for one)\n"
    "  --rx-gain DB    receive gain (default 32)\n"
    "  --slots N       stop after N slots (default: until interrupted; with\n"
    "                  --loopback, 3)\n"
    "  --loopback      transmit a frame each slot and decode it back (transmits)\n"
    "  --message TEXT  the loopback frame's standard FT8 text (default \"CQ K1ABC FN42\")\n"
    "  --audio HZ      the loopback frame's audio frequency, 200 to 2900 (default 1000)\n"
    "  --tx-gain DB    loopback transmit gain (default 0)\n";

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument(a + " needs a value");
            }
            return argv[++i];
        };
        if (a == "--args") {
            o.args = value();
        } else if (a == "--dial") {
            o.dial_hz = std::stod(value());
        } else if (a == "--rx-gain") {
            o.rx_gain_db = std::stod(value());
        } else if (a == "--tx-gain") {
            o.tx_gain_db = std::stod(value());
        } else if (a == "--slots") {
            o.slots = std::stoi(value());
        } else if (a == "--loopback") {
            o.loopback = true;
        } else if (a == "--message") {
            o.message = value();
        } else if (a == "--audio") {
            o.audio_hz = std::stof(value());
        } else {
            throw std::invalid_argument(a == "-h" || a == "--help" ? "" : "unknown option " + a);
        }
    }
    if (o.loopback && o.slots == 0) {
        o.slots = 3;
    }
    return o;
}

// The receive chain, 1.92 Msps down to the decoder's 3200 sps, with the
// stream position of every output sample: output k came from input
// position start + k * kTotal. A gap in the stream restarts it there.
class Chain {
public:
    static constexpr long long kTotal = ft8::kResampleFactor * ft8::Decimator::kFactor;

    void feed(long long position, std::span<const Sample> input) {
        if (!down_ || position != next_) {
            down_ = std::make_unique<ft8::DownTo48k>();
            decimator_ = std::make_unique<ft8::Decimator>();
            start_ = position;
            produced_ = 0;
        }
        next_ = position + static_cast<long long>(input.size());
        audio_.clear();
        out_.clear();
        down_->process(input, audio_);
        decimator_->process(audio_, out_);
    }
    // What the last feed() produced, and the position of its first sample.
    const std::vector<Sample>& output() const { return out_; }
    long long output_position() {
        const long long at = start_ + produced_ * kTotal;
        produced_ += static_cast<long long>(out_.size());
        return at;
    }
    // Whether the chain's output has reached stream position `position`.
    bool reached(long long position) const {
        return down_ && start_ + produced_ * kTotal >= position;
    }
    // Outputs past one capture, kept for the next.
    void hold(long long position, const Sample& s) { held_.emplace_back(position, s); }
    std::vector<std::pair<long long, Sample>> release() { return std::exchange(held_, {}); }

private:
    std::unique_ptr<ft8::DownTo48k> down_;
    std::unique_ptr<ft8::Decimator> decimator_;
    long long start_ = 0;
    long long next_ = 0;
    long long produced_ = 0;
    std::vector<Sample> audio_;
    std::vector<Sample> out_;
    std::vector<std::pair<long long, Sample>> held_;
};

// Reads the receive stream until a whole slot, 15 s from `from` (a stream
// position), has gone through the chain; returns its 3200 sps samples.
// What the chain produced past the slot is held for the next capture,
// since one read can cross the boundary. Fewer than a slot if the stream
// fails or the user stops it.
std::vector<Sample> capture(SoapySDR::Device& device, SoapySDR::Stream* rx, Chain& chain,
                            long long from) {
    const auto slot = static_cast<long long>(ft8::kSlotTime * ft8::kPairRate);
    std::vector<Sample> out;
    // What was held from the capture before, then fresh reads.
    auto take = [&](long long at, const Sample& s) {
        if (at >= from && at < from + slot) {
            out.push_back(s);
        } else if (at >= from + slot) {
            chain.hold(at, s);
        }
    };
    for (const auto& [at, s] : chain.release()) {
        take(at, s);
    }
    std::vector<Sample> buffer(device.getStreamMTU(rx));
    while (!g_stop && !chain.reached(from + slot)) {
        void* buffs[] = {buffer.data()};
        int flags = 0;
        long long time_ns = 0;
        const int n = device.readStream(rx, buffs, buffer.size(), flags, time_ns, 1'000'000);
        if (n == SOAPY_SDR_OVERFLOW || n == SOAPY_SDR_TIMEOUT) {
            continue;
        }
        if (n < 0) {
            std::fprintf(stderr, "hackrfduplex-ft8: read error %s\n", SoapySDR::errToStr(n));
            break;
        }
        const long long position = SoapySDR::timeNsToTicks(time_ns, ft8::kPairRate);
        chain.feed(position, std::span(buffer).first(static_cast<std::size_t>(n)));
        long long at = chain.output_position();
        for (const Sample& s : chain.output()) {
            take(at, s);
            at += Chain::kTotal;
        }
    }
    return out;
}

// The frame for `text` at `audio_hz`, ramped and at half scale, taken up to
// the pair's rate.
std::vector<Sample> frame(const std::string& text, float audio_hz) {
    const auto payload = ft8::message_payload(text);
    if (!payload) {
        throw std::invalid_argument("not a standard FT8 message: " + text);
    }
    std::vector<Sample> audio =
        ft8::synthesize(ft8::encode(*payload), audio_hz - 1500.0F, ft8::kAudioRate);
    for (Sample& s : audio) {
        s *= 0.5F;
    }
    ft8::UpFrom48k up;
    std::vector<Sample> wide;
    up.process(audio, wide);
    return wide;
}

// Writes `samples` as one burst starting at stream time `at_ns`.
bool transmit(SoapySDR::Device& device, SoapySDR::Stream* tx, const std::vector<Sample>& samples,
              long long at_ns) {
    std::size_t sent = 0;
    bool first = true;
    while (sent < samples.size() && !g_stop) {
        const std::size_t n = std::min<std::size_t>(samples.size() - sent, 65536);
        int flags = (first ? SOAPY_SDR_HAS_TIME : 0) |
                    (sent + n == samples.size() ? SOAPY_SDR_END_BURST : 0);
        const void* buffs[] = {samples.data() + sent};
        const int w = device.writeStream(tx, buffs, n, flags, at_ns, 2'000'000);
        if (w == SOAPY_SDR_TIMEOUT) {
            continue;
        }
        if (w < 0) {
            std::fprintf(stderr, "hackrfduplex-ft8: write error %s\n", SoapySDR::errToStr(w));
            return false;
        }
        sent += static_cast<std::size_t>(w);
        first = false;
    }
    return sent == samples.size();
}

std::vector<ft8::Decoder::Frame> decode(const std::vector<Sample>& samples) {
    ft8::Decoder::Options o;
    o.sample_rate = ft8::Decimator::kOutputRate;
    ft8::Decoder decoder(o);
    decoder.process(samples);
    return decoder.decode();
}

void print(const char* when, const std::vector<ft8::Decoder::Frame>& frames, std::size_t samples) {
    for (const auto& f : frames) {
        const auto text = ft8::message_text(f.payload);
        std::printf("%s %4.0f %5.1f %5.0f ~  %s\n", when, f.snr_db, f.time_s - ft8::kStartOffset,
                    f.freq_hz + 1500.0F, text ? text->c_str() : "(not a standard message)");
    }
    std::printf("-- %s: %zu decoded from %zu samples\n", when, frames.size(), samples);
    std::fflush(stdout);
}

int receive_only(SoapySDR::Device& device, const Options& options) {
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    device.activateStream(rx);
    // The stream's time against the wall clock, once: a slot starts on a
    // UTC multiple of 15 s.
    const auto wall = std::chrono::system_clock::now();
    const long long now = SoapySDR::timeNsToTicks(device.getHardwareTime(), ft8::kPairRate);
    const double utc = std::chrono::duration<double>(wall.time_since_epoch()).count();
    Chain chain;
    // Each slot is decoded while the next is captured: the receive ring
    // holds about a second, and decoding can take longer than that.
    std::future<void> decoding;
    for (int slot = 0; (options.slots == 0 || slot < options.slots) && !g_stop; ++slot) {
        const double next = (std::floor(utc / ft8::kSlotTime) + 1 + slot) * ft8::kSlotTime;
        const long long from = now + std::llround((next - utc) * ft8::kPairRate);
        std::vector<Sample> samples = capture(device, rx, chain, from);
        if (decoding.valid()) {
            decoding.get();
        }
        decoding = std::async(std::launch::async, [next, samples = std::move(samples)] {
            const auto start = static_cast<std::time_t>(next);
            std::tm tm{};
            gmtime_r(&start, &tm);
            char when[16];
            std::strftime(when, sizeof when, "%H%M%S", &tm);
            print(when, decode(samples), samples.size());
        });
    }
    if (decoding.valid()) {
        decoding.get();
    }
    device.deactivateStream(rx);
    device.closeStream(rx);
    return 0;
}

int loopback(SoapySDR::Device& device, const Options& options) {
    const std::vector<Sample> burst = frame(options.message, options.audio_hz);
    SoapySDR::Stream* rx = device.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    SoapySDR::Stream* tx = device.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32);
    device.activateStream(rx);
    device.activateStream(tx);
    if (!device.hasHardwareTime()) {
        std::fprintf(stderr,
                     "hackrfduplex-ft8: the streams share no time; is clkout set and the "
                     "reference cable in?\n");
        return 2;
    }
    Chain chain;
    int heard = 0;
    for (int slot = 0; slot < options.slots && !g_stop; ++slot) {
        // A slot starting a second from now, its frame 0.5 s in as FT8's is.
        const long long from = SoapySDR::timeNsToTicks(device.getHardwareTime(), ft8::kPairRate) +
                               static_cast<long long>(ft8::kPairRate);
        const long long at = from + std::llround(ft8::kStartOffset * ft8::kPairRate);
        bool sent = false;
        std::thread writer([&] {
            sent = transmit(device, tx, burst, SoapySDR::ticksToTimeNs(at, ft8::kPairRate));
        });
        const std::vector<Sample> samples = capture(device, rx, chain, from);
        writer.join();
        char when[16];
        std::snprintf(when, sizeof when, "slot %d", slot + 1);
        const auto frames = decode(samples);
        print(when, frames, samples.size());
        bool found = false;
        for (const auto& f : frames) {
            found = found || ft8::message_text(f.payload) == options.message;
        }
        heard += sent && found ? 1 : 0;
    }
    device.deactivateStream(tx);
    device.deactivateStream(rx);
    device.closeStream(tx);
    device.closeStream(rx);
    std::printf("loopback: \"%s\" sent and decoded in %d of %d slots\n", options.message.c_str(),
                heard, options.slots);
    return heard == options.slots ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse(argc, argv);
    } catch (const std::exception& e) {
        if (*e.what() != '\0') {
            std::cerr << "hackrfduplex-ft8: " << e.what() << "\n\n";
        }
        std::cerr << kUsage;
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        std::unique_ptr<SoapySDR::Device, void (*)(SoapySDR::Device*)> device(
            SoapySDR::Device::make(options.args), SoapySDR::Device::unmake);
        // The receive centre sits 1500 Hz above the dial, so FT8's audio,
        // 0 to 3000 Hz above the dial, lies either side of it.
        const double centre = options.dial_hz + 1500.0;
        for (const int direction : {SOAPY_SDR_RX, SOAPY_SDR_TX}) {
            device->setSampleRate(direction, 0, ft8::kPairRate);
            device->setFrequency(direction, 0, centre);
        }
        device->setGain(SOAPY_SDR_RX, 0, options.rx_gain_db);
        device->setGain(SOAPY_SDR_TX, 0, options.tx_gain_db);
        // The receiver's DC sits at the centre, inside FT8's band: removed
        // where the module offers it.
        if (device->hasDCOffsetMode(SOAPY_SDR_RX, 0)) {
            device->setDCOffsetMode(SOAPY_SDR_RX, 0, true);
        }
        return options.loopback ? loopback(*device, options) : receive_only(*device, options);
    } catch (const std::exception& e) {
        std::cerr << "hackrfduplex-ft8: " << e.what() << "\n";
        return 1;
    }
}
