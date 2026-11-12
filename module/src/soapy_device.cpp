// SPDX-License-Identifier: GPL-3.0-or-later
#include "soapy_device.hpp"

#include <SoapySDR/Constants.h>

#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <chrono>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "bandwidth.hpp"
#include "convert.hpp"
#include "hackrf_board.hpp"
#include "hackrfduplex/version.hpp"
#include "selection.hpp"

namespace hackrfduplex {

namespace {

// The transmit ring's depth unless set, and the least the receive ring's
// is (Pair::open_stream).
constexpr std::size_t kDefaultBuffers = 15;
// Measured (docs/design.md): receive is clean down to 200 ksps, about
// where the clock chip's divider runs out; the transmitter goes silent or
// loses a channel between 1.05 and 1.1 Msps, so its floor keeps a margin.
constexpr double kMinRxSampleRate = 200e3;
constexpr double kMinTxSampleRate = 1.2e6;
constexpr double kMaxSampleRate = 20e6;
constexpr double kMinFrequency = 1e6;
constexpr double kMaxFrequency = 6e9;

void soapy_log(LogLevel level, const std::string& message) {
    SoapySDRLogLevel soapy_level = SOAPY_SDR_INFO;
    switch (level) {
        case LogLevel::kDebug:
            soapy_level = SOAPY_SDR_DEBUG;
            break;
        case LogLevel::kInfo:
            soapy_level = SOAPY_SDR_INFO;
            break;
        case LogLevel::kWarning:
            soapy_level = SOAPY_SDR_WARNING;
            break;
        case LogLevel::kError:
            soapy_level = SOAPY_SDR_ERROR;
            break;
    }
    SoapySDR::log(soapy_level, "hackrfduplex: " + message);
}

// A request this device cannot honour, refused rather than ignored.
[[noreturn]] void refuse(const std::string& call, const std::string& why) {
    throw std::invalid_argument(call + ": " + why);
}

// A count of samples, as the tx_underrun_limit setting and argument take it.
std::uint32_t parse_samples(const std::string& key, const std::string& text) {
    const bool digits = !text.empty() && text.find_first_not_of("0123456789") == std::string::npos;
    if (!digits || text.size() > 10 || std::stoull(text) > UINT32_MAX) {
        refuse(key, "a count of samples from 0 to " + std::to_string(UINT32_MAX) + ", not '" +
                        text + "'");
    }
    return static_cast<std::uint32_t>(std::stoull(text));
}

std::chrono::microseconds timeout_of(long timeout_us) {
    return std::chrono::microseconds(timeout_us < 0 ? 0 : timeout_us);
}

std::unique_ptr<Pair> open_pair(const SoapySDR::Kwargs& args) {
    const Selection selection = select_pair(args, list_boards());
    return std::make_unique<Pair>(std::make_unique<HackrfBoard>(selection.rx),
                                  std::make_unique<HackrfBoard>(selection.tx), &soapy_log);
}

}  // namespace

SoapyDevice::SoapyDevice(const SoapySDR::Kwargs& args) : SoapyDevice(open_pair(args), args) {}

SoapyDevice::SoapyDevice(std::unique_ptr<Pair> pair, const SoapySDR::Kwargs& args)
    : pair_(std::move(pair)) {
    if (!pair_) {
        throw std::invalid_argument("a device needs a pair");
    }
    // The settings are device arguments too, except calibrate, which acts;
    // the rest are what discovery hands back. Anything else is logged, as an
    // unknown setting is: applications pass argument strings around.
    for (const auto& [key, value] : args) {
        if (key == "bias_tx" || key == "clkout" || key == "time_origin" ||
            key == "tx_underrun_limit") {
            writeSetting(key, value);
        } else if (key != "driver" && key != "label" && key != kRxSerialKey &&
                   key != kTxSerialKey) {
            soapy_log(LogLevel::kWarning, "device argument " + key + ": none such, ignored");
        }
    }
}

SoapyDevice::~SoapyDevice() = default;

Direction SoapyDevice::direction_of(int direction) {
    if (direction == SOAPY_SDR_RX) {
        return Direction::kRx;
    }
    if (direction == SOAPY_SDR_TX) {
        return Direction::kTx;
    }
    throw std::invalid_argument("invalid direction " + std::to_string(direction));
}

Direction SoapyDevice::direction_of(SoapySDR::Stream* stream) const {
    if (stream == token(Direction::kRx)) {
        return Direction::kRx;
    }
    if (stream == token(Direction::kTx)) {
        return Direction::kTx;
    }
    throw std::invalid_argument("invalid stream handle");
}

SoapySDR::Stream* SoapyDevice::token(Direction direction) const {
    const int* which = direction == Direction::kRx ? &rx_token_ : &tx_token_;
    return reinterpret_cast<SoapySDR::Stream*>(const_cast<int*>(which));
}

void SoapyDevice::check_channel(const size_t channel) {
    if (channel != 0) {
        throw std::invalid_argument("invalid channel " + std::to_string(channel) +
                                    ": each direction has one");
    }
}

Direction SoapyDevice::check(const int direction, const size_t channel) {
    const Direction dir = direction_of(direction);
    check_channel(channel);
    return dir;
}

// Identification

std::string SoapyDevice::getDriverKey() const { return "hackrfduplex"; }

std::string SoapyDevice::getHardwareKey() const { return "HackRF Duplex"; }

SoapySDR::Kwargs SoapyDevice::getHardwareInfo() const {
    SoapySDR::Kwargs info;
    for (const Direction direction : {Direction::kRx, Direction::kTx}) {
        const std::string prefix = direction == Direction::kRx ? "rx_" : "tx_";
        const BoardInfo board = pair_->board(direction).info();
        info[prefix + "serial"] = board.serial;
        info[prefix + "board"] = board.board;
        info[prefix + "revision"] = board.revision;
        info[prefix + "firmware"] = board.firmware;
        info[prefix + "usb_api"] = board.usb_api;
        info[prefix + "clock"] = board.external_clock ? "external" : "internal";
        info[prefix + "clkout"] = board.clock_out ? "on" : "off";
    }
    info["module_version"] = kVersion;
    info["libhackrf"] = libhackrf_version();
    return info;
}

// Channels

size_t SoapyDevice::getNumChannels(const int direction) const {
    direction_of(direction);
    return 1;
}

bool SoapyDevice::getFullDuplex(const int direction, const size_t channel) const {
    check(direction, channel);
    return true;
}

// Streams

std::vector<std::string> SoapyDevice::getStreamFormats(const int direction,
                                                       const size_t channel) const {
    check(direction, channel);
    return {SOAPY_SDR_CS8, SOAPY_SDR_CS16, SOAPY_SDR_CF32, SOAPY_SDR_CF64};
}

std::string SoapyDevice::getNativeStreamFormat(const int direction, const size_t channel,
                                               double& fullScale) const {
    check(direction, channel);
    fullScale = kFullScale;
    return SOAPY_SDR_CS8;
}

SoapySDR::ArgInfoList SoapyDevice::getStreamArgsInfo(const int direction,
                                                     const size_t channel) const {
    check(direction, channel);
    SoapySDR::ArgInfo buffers;
    buffers.key = "buffers";
    buffers.name = "Buffer count";
    buffers.description = "USB transfers the stream can hold between the board and the caller.";
    if (direction == SOAPY_SDR_RX) {
        // Left empty, the module sizes it at each activation (Pair::open_stream).
        buffers.description +=
            " Unset: enough for 1.5 s at the sample rate when the stream starts, at least " +
            std::to_string(kDefaultBuffers) + ".";
    } else {
        buffers.value = std::to_string(kDefaultBuffers);
    }
    buffers.units = "transfers";
    buffers.type = SoapySDR::ArgInfo::INT;
    return {buffers};
}

SoapySDR::Stream* SoapyDevice::setupStream(const int direction, const std::string& format,
                                           const std::vector<size_t>& channels,
                                           const SoapySDR::Kwargs& args) {
    const Direction dir = direction_of(direction);
    if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0)) {
        throw std::invalid_argument("setupStream: each direction has one channel, 0");
    }
    std::optional<std::size_t> buffers;
    const auto it = args.find("buffers");
    if (it != args.end()) {
        const int wanted = std::stoi(it->second);
        if (wanted <= 0) {
            throw std::invalid_argument("setupStream: buffers must be positive");
        }
        buffers = static_cast<std::size_t>(wanted);
    }
    for (const auto& [key, value] : args) {
        if (key != "buffers") {
            soapy_log(LogLevel::kWarning, "setupStream: no stream argument " + key + ", ignored");
        }
    }
    pair_->open_stream(dir, parse_format(format), buffers);
    return token(dir);
}

void SoapyDevice::closeStream(SoapySDR::Stream* stream) {
    pair_->close_stream(direction_of(stream));
}

size_t SoapyDevice::getStreamMTU(SoapySDR::Stream* stream) const {
    return pair_->mtu(direction_of(stream));
}

int SoapyDevice::activateStream(SoapySDR::Stream* stream, const int flags, const long long timeNs,
                                const size_t numElems) {
    const Direction dir = direction_of(stream);
    if (dir == Direction::kTx && (flags != 0 || numElems != 0)) {
        soapy_log(LogLevel::kError,
                  "activateStream: the TX stream starts at once and runs until deactivated; no "
                  "flags or count (time the writes instead)");
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    // Receive: a start time, a burst of numElems samples, or both.
    const bool timed = (flags & SOAPY_SDR_HAS_TIME) != 0;
    const bool burst = (flags & SOAPY_SDR_END_BURST) != 0;
    if ((flags & ~(SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST)) != 0 || burst != (numElems != 0)) {
        soapy_log(LogLevel::kError,
                  "activateStream: the RX stream takes SOAPY_SDR_HAS_TIME, and "
                  "SOAPY_SDR_END_BURST with a count of samples, and nothing else");
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    std::optional<std::uint64_t> start;
    if (timed) {
        const double rate = pair_->sample_rate(Direction::kRx);
        if (!(rate > 0)) {
            soapy_log(LogLevel::kError, "activateStream: set the receive sample rate first");
            return SOAPY_SDR_TIME_ERROR;
        }
        // Time is the receive position plus the offset; a stream that is not
        // running starts again from position 0.
        const long long ticks = SoapySDR::timeNsToTicks(timeNs, rate);
        const long long now =
            pair_->active(Direction::kRx) ? pair_->time_ticks() : pair_->time_offset();
        if (ticks < now) {
            soapy_log(LogLevel::kError, "activateStream: the receive start time has passed");
            return SOAPY_SDR_TIME_ERROR;
        }
        start = static_cast<std::uint64_t>(ticks - pair_->time_offset());
    }
    std::optional<std::uint64_t> count;
    if (burst) {
        count = numElems;
    }
    try {
        pair_->activate(dir);
        if (dir == Direction::kRx) {
            pair_->set_rx_window(start, count);
        }
    } catch (const BoardError& e) {
        soapy_log(LogLevel::kError, e.what());
        return SOAPY_SDR_STREAM_ERROR;
    }
    return 0;
}

int SoapyDevice::deactivateStream(SoapySDR::Stream* stream, const int flags, const long long) {
    if (flags != 0) {
        soapy_log(LogLevel::kError, "deactivateStream: a stream stops at once; no flags");
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    try {
        pair_->deactivate(direction_of(stream));
    } catch (const BoardError& e) {
        soapy_log(LogLevel::kError, e.what());
        return SOAPY_SDR_STREAM_ERROR;
    }
    return 0;
}

int SoapyDevice::readStream(SoapySDR::Stream* stream, void* const* buffs, const size_t numElems,
                            int& flags, long long& timeNs, const long timeoutUs) {
    if (direction_of(stream) != Direction::kRx) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    if (!pair_->active(Direction::kRx)) {
        return SOAPY_SDR_STREAM_ERROR;
    }
    if (numElems == 0) {
        flags = 0;
        return 0;
    }
    RxRead read;
    try {
        read = pair_->read(buffs[0], numElems, timeout_of(timeoutUs));
    } catch (const std::logic_error& e) {
        soapy_log(LogLevel::kError, std::string("readStream: ") + e.what());
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    return report_read(read, flags, timeNs);
}

int SoapyDevice::report_read(const RxRead& read, int& flags, long long& timeNs) const {
    flags = 0;
    if (read.overflow) {
        flags |= SOAPY_SDR_END_ABRUPT;
        return SOAPY_SDR_OVERFLOW;
    }
    if (read.end_burst) {
        flags |= SOAPY_SDR_END_BURST;
    }
    if (read.samples == 0) {
        return pair_->failed(Direction::kRx) ? SOAPY_SDR_STREAM_ERROR : SOAPY_SDR_TIMEOUT;
    }
    flags |= SOAPY_SDR_HAS_TIME;
    timeNs = SoapySDR::ticksToTimeNs(static_cast<long long>(read.position) + pair_->time_offset(),
                                     pair_->sample_rate(Direction::kRx));
    return static_cast<int>(read.samples);
}

size_t SoapyDevice::getNumDirectAccessBuffers(SoapySDR::Stream* stream) {
    if (direction_of(stream) != Direction::kRx ||
        pair_->stream_format(Direction::kRx) != Format::kCs8) {
        return 0;
    }
    return pair_->rx_slots();
}

int SoapyDevice::getDirectAccessBufferAddrs(SoapySDR::Stream* stream, const size_t handle,
                                            void** buffs) {
    if (handle >= getNumDirectAccessBuffers(stream)) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    buffs[0] = pair_->rx_slot_bytes(handle);
    return 0;
}

int SoapyDevice::acquireReadBuffer(SoapySDR::Stream* stream, size_t& handle, const void** buffs,
                                   int& flags, long long& timeNs, const long timeoutUs) {
    if (getNumDirectAccessBuffers(stream) == 0) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    if (!pair_->active(Direction::kRx)) {
        return SOAPY_SDR_STREAM_ERROR;
    }
    RxView view;
    try {
        view = pair_->acquire_read(pair_->mtu(Direction::kRx), timeout_of(timeoutUs));
    } catch (const std::logic_error& e) {
        soapy_log(LogLevel::kError, std::string("acquireReadBuffer: ") + e.what());
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    const int result = report_read(view.read, flags, timeNs);
    if (result > 0) {
        handle = view.slot;
        buffs[0] = view.data;
    }
    return result;
}

void SoapyDevice::releaseReadBuffer(SoapySDR::Stream* stream, const size_t handle) {
    if (getNumDirectAccessBuffers(stream) == 0) {
        refuse("releaseReadBuffer", "no direct access on this stream");
    }
    pair_->release_read(handle);
}

int SoapyDevice::writeStream(SoapySDR::Stream* stream, const void* const* buffs,
                             const size_t numElems, int& flags, const long long timeNs,
                             const long timeoutUs) {
    if (direction_of(stream) != Direction::kTx) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    if (!pair_->active(Direction::kTx)) {
        return SOAPY_SDR_STREAM_ERROR;
    }
    if ((flags & ~(SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST | SOAPY_SDR_ONE_PACKET)) != 0) {
        flags = 0;
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    const bool end_of_burst = (flags & (SOAPY_SDR_END_BURST | SOAPY_SDR_ONE_PACKET)) != 0;
    if ((flags & SOAPY_SDR_HAS_TIME) != 0) {
        flags = 0;
        const double rate = pair_->sample_rate(Direction::kRx);
        if (!(rate > 0)) {
            return SOAPY_SDR_TIME_ERROR;
        }
        const TimedWrite written =
            pair_->write_at(buffs[0], numElems, end_of_burst, SoapySDR::timeNsToTicks(timeNs, rate),
                            timeout_of(timeoutUs));
        switch (written.status) {
            case TimedStatus::kOk:
                return static_cast<int>(written.taken);
            case TimedStatus::kTimeout:
                return pair_->failed(Direction::kTx) ? SOAPY_SDR_STREAM_ERROR : SOAPY_SDR_TIMEOUT;
            case TimedStatus::kLate:
            case TimedStatus::kNoTime:
                return pair_->failed(Direction::kTx) ? SOAPY_SDR_STREAM_ERROR
                                                     : SOAPY_SDR_TIME_ERROR;
        }
        return SOAPY_SDR_TIME_ERROR;
    }
    const std::size_t taken = pair_->write(buffs[0], numElems, end_of_burst, timeout_of(timeoutUs));
    flags = 0;
    if (numElems == 0) {
        // Nothing asked, nothing taken: an end of burst, or nothing at all.
        return pair_->failed(Direction::kTx) ? SOAPY_SDR_STREAM_ERROR : 0;
    }
    if (taken == 0) {
        return pair_->failed(Direction::kTx) ? SOAPY_SDR_STREAM_ERROR : SOAPY_SDR_TIMEOUT;
    }
    return static_cast<int>(taken);
}

int SoapyDevice::readStreamStatus(SoapySDR::Stream* stream, size_t& chanMask, int& flags,
                                  long long& timeNs, const long timeoutUs) {
    if (direction_of(stream) != Direction::kTx) {
        return SOAPY_SDR_NOT_SUPPORTED;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout_of(timeoutUs);
    while (true) {
        if (pair_->failed(Direction::kTx)) {
            return SOAPY_SDR_STREAM_ERROR;
        }
        if (pair_->take_tx_underflow()) {
            chanMask = 1;
            flags = 0;
            timeNs = 0;
            SoapySDR::log(SOAPY_SDR_SSI, "U");
            return SOAPY_SDR_UNDERFLOW;
        }
        if (pair_->take_time_error()) {
            chanMask = 1;
            flags = 0;
            timeNs = 0;
            SoapySDR::log(SOAPY_SDR_SSI, "L");
            return SOAPY_SDR_TIME_ERROR;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return SOAPY_SDR_TIMEOUT;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Antenna

std::vector<std::string> SoapyDevice::listAntennas(const int direction,
                                                   const size_t channel) const {
    check(direction, channel);
    return {"TX/RX"};
}

void SoapyDevice::setAntenna(const int direction, const size_t channel, const std::string& name) {
    check(direction, channel);
    if (name != "TX/RX") {
        throw std::invalid_argument("setAntenna: the HackRF has one port, TX/RX");
    }
}

std::string SoapyDevice::getAntenna(const int direction, const size_t channel) const {
    check(direction, channel);
    return "TX/RX";
}

// Gain

std::vector<std::string> SoapyDevice::listGains(const int direction, const size_t channel) const {
    // gr-osmosdr expects the IF gain first and the baseband gain last.
    if (check(direction, channel) == Direction::kRx) {
        return {"LNA", "AMP", "VGA"};
    }
    return {"VGA", "AMP"};
}

bool SoapyDevice::hasGainMode(const int direction, const size_t channel) const {
    check(direction, channel);
    return false;
}

void SoapyDevice::setGainMode(const int direction, const size_t channel, const bool automatic) {
    check(direction, channel);
    if (automatic) {
        refuse("setGainMode", "the HackRF has no automatic gain control");
    }
}

bool SoapyDevice::getGainMode(const int direction, const size_t channel) const {
    check(direction, channel);
    return false;
}

void SoapyDevice::setGain(const int direction, const size_t channel, const double value) {
    check_channel(channel);
    pair_->set_overall_gain(direction_of(direction), value);
}

void SoapyDevice::setGain(const int direction, const size_t channel, const std::string& name,
                          const double value) {
    check_channel(channel);
    const Direction dir = direction_of(direction);
    if (dir == Direction::kRx) {
        RxGains gains = pair_->rx_gains();
        if (name == "LNA") {
            gains.lna_db = gain::quantise(value, gain::kRxLnaMaxDb, gain::kRxLnaStepDb);
        } else if (name == "VGA") {
            gains.vga_db = gain::quantise(value, gain::kRxVgaMaxDb, gain::kRxVgaStepDb);
        } else if (name == "AMP") {
            gains.amp = value > 0;
        } else {
            throw std::invalid_argument("setGain: unknown RX gain " + name);
        }
        pair_->set_rx_gains(gains);
        return;
    }
    TxGains gains = pair_->tx_gains();
    if (name == "VGA") {
        gains.vga_db = gain::quantise(value, gain::kTxVgaMaxDb, gain::kTxVgaStepDb);
    } else if (name == "AMP") {
        gains.amp = value > 0;
    } else {
        throw std::invalid_argument("setGain: unknown TX gain " + name);
    }
    pair_->set_tx_gains(gains);
}

double SoapyDevice::getGain(const int direction, const size_t channel) const {
    check_channel(channel);
    return pair_->overall_gain(direction_of(direction));
}

double SoapyDevice::getGain(const int direction, const size_t channel,
                            const std::string& name) const {
    check_channel(channel);
    if (direction_of(direction) == Direction::kRx) {
        const RxGains gains = pair_->rx_gains();
        if (name == "LNA") {
            return gains.lna_db;
        }
        if (name == "VGA") {
            return gains.vga_db;
        }
        if (name == "AMP") {
            return gains.amp ? gain::kAmpDb : 0;
        }
        throw std::invalid_argument("getGain: unknown RX gain " + name);
    }
    const TxGains gains = pair_->tx_gains();
    if (name == "VGA") {
        return gains.vga_db;
    }
    if (name == "AMP") {
        return gains.amp ? gain::kAmpDb : 0;
    }
    throw std::invalid_argument("getGain: unknown TX gain " + name);
}

SoapySDR::Range SoapyDevice::getGainRange(const int direction, const size_t channel) const {
    if (check(direction, channel) == Direction::kRx) {
        return SoapySDR::Range(0, gain::kRxMaxDb, 1);
    }
    return SoapySDR::Range(0, gain::kTxMaxDb, 1);
}

SoapySDR::Range SoapyDevice::getGainRange(const int direction, const size_t channel,
                                          const std::string& name) const {
    check(direction, channel);
    if (name == "AMP") {
        return SoapySDR::Range(0, gain::kAmpDb, gain::kAmpDb);
    }
    if (direction_of(direction) == Direction::kRx) {
        if (name == "LNA") {
            return SoapySDR::Range(0, gain::kRxLnaMaxDb, gain::kRxLnaStepDb);
        }
        if (name == "VGA") {
            return SoapySDR::Range(0, gain::kRxVgaMaxDb, gain::kRxVgaStepDb);
        }
    } else if (name == "VGA") {
        return SoapySDR::Range(0, gain::kTxVgaMaxDb, gain::kTxVgaStepDb);
    }
    throw std::invalid_argument("getGainRange: unknown gain " + name);
}

// Frequency

void SoapyDevice::setFrequency(const int direction, const size_t channel, const double frequency,
                               const SoapySDR::Kwargs& args) {
    setFrequency(direction, channel, "RF", frequency, args);
}

void SoapyDevice::setFrequency(const int direction, const size_t channel, const std::string& name,
                               const double frequency, const SoapySDR::Kwargs& args) {
    check_channel(channel);
    if (!args.empty()) {
        refuse("setFrequency", "no tuning arguments (" + args.begin()->first +
                                   "); the frequency is set on the RF synthesiser alone");
    }
    if (name != "RF") {
        throw std::invalid_argument("setFrequency: unknown component " + name);
    }
    if (!(frequency >= 0)) {
        throw std::invalid_argument("setFrequency: a frequency cannot be negative");
    }
    pair_->set_frequency(direction_of(direction),
                         static_cast<std::uint64_t>(std::llround(frequency)));
}

double SoapyDevice::getFrequency(const int direction, const size_t channel) const {
    return getFrequency(direction, channel, "RF");
}

double SoapyDevice::getFrequency(const int direction, const size_t channel,
                                 const std::string& name) const {
    check_channel(channel);
    if (name != "RF") {
        throw std::invalid_argument("getFrequency: unknown component " + name);
    }
    return static_cast<double>(pair_->frequency(direction_of(direction)));
}

std::vector<std::string> SoapyDevice::listFrequencies(const int direction,
                                                      const size_t channel) const {
    check(direction, channel);
    return {"RF"};
}

SoapySDR::RangeList SoapyDevice::getFrequencyRange(const int direction,
                                                   const size_t channel) const {
    return getFrequencyRange(direction, channel, "RF");
}

SoapySDR::RangeList SoapyDevice::getFrequencyRange(const int direction, const size_t channel,
                                                   const std::string& name) const {
    check(direction, channel);
    if (name != "RF") {
        throw std::invalid_argument("getFrequencyRange: unknown component " + name);
    }
    return {SoapySDR::Range(kMinFrequency, kMaxFrequency)};
}

// Corrections and modes. With HACKRFDUPLEX_DSP the receive channel has DC
// removal and I/Q balance correction (rx_correction.hpp); without it, and
// on the transmit channel always, they are refused.

#if defined(HACKRFDUPLEX_DSP)
#define HACKRFDUPLEX_CORRECTS(direction, channel) (check(direction, channel) == Direction::kRx)
#else
#define HACKRFDUPLEX_CORRECTS(direction, channel) (check(direction, channel), false)
#endif

bool SoapyDevice::hasDCOffsetMode(const int direction, const size_t channel) const {
    return HACKRFDUPLEX_CORRECTS(direction, channel);
}

void SoapyDevice::setDCOffsetMode(const int direction, const size_t channel, const bool automatic) {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        pair_->rx_correction().set_dc_automatic(automatic);
#endif
        return;
    }
    if (automatic) {
        refuse("setDCOffsetMode", "no DC offset removal on this channel in this build");
    }
}

bool SoapyDevice::getDCOffsetMode(const int direction, const size_t channel) const {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        return pair_->rx_correction().dc_automatic();
#endif
    }
    return false;
}

bool SoapyDevice::hasDCOffset(const int direction, const size_t channel) const {
    return HACKRFDUPLEX_CORRECTS(direction, channel);
}

void SoapyDevice::setDCOffset(const int direction, const size_t channel,
                              [[maybe_unused]] const std::complex<double>& offset) {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        pair_->rx_correction().set_dc_offset(offset);
#endif
        return;
    }
    refuse("setDCOffset", "no DC offset correction on this channel in this build");
}

std::complex<double> SoapyDevice::getDCOffset(const int direction, const size_t channel) const {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        return pair_->rx_correction().dc_offset();
#endif
    }
    return {};
}

bool SoapyDevice::hasIQBalance(const int direction, const size_t channel) const {
    return HACKRFDUPLEX_CORRECTS(direction, channel);
}

void SoapyDevice::setIQBalance(const int direction, const size_t channel,
                               [[maybe_unused]] const std::complex<double>& balance) {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        pair_->rx_correction().set_iq_balance(balance);
#endif
        return;
    }
    refuse("setIQBalance", "no I/Q balance correction on this channel in this build");
}

std::complex<double> SoapyDevice::getIQBalance(const int direction, const size_t channel) const {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        return pair_->rx_correction().iq_balance();
#endif
    }
    return {};
}

bool SoapyDevice::hasIQBalanceMode(const int direction, const size_t channel) const {
    return HACKRFDUPLEX_CORRECTS(direction, channel);
}

void SoapyDevice::setIQBalanceMode(const int direction, const size_t channel,
                                   const bool automatic) {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        pair_->rx_correction().set_iq_automatic(automatic);
#endif
        return;
    }
    if (automatic) {
        refuse("setIQBalanceMode", "no I/Q balance correction on this channel in this build");
    }
}

bool SoapyDevice::getIQBalanceMode(const int direction, const size_t channel) const {
    if (HACKRFDUPLEX_CORRECTS(direction, channel)) {
#if defined(HACKRFDUPLEX_DSP)
        return pair_->rx_correction().iq_automatic();
#endif
    }
    return false;
}

#undef HACKRFDUPLEX_CORRECTS

bool SoapyDevice::hasFrequencyCorrection(const int direction, const size_t channel) const {
    check(direction, channel);
    return false;
}

void SoapyDevice::setFrequencyCorrection(const int direction, const size_t channel,
                                         const double value) {
    check(direction, channel);
    if (value != 0.0) {
        refuse("setFrequencyCorrection",
               "no frequency correction; share one reference between the boards (clkout)");
    }
}

double SoapyDevice::getFrequencyCorrection(const int direction, const size_t channel) const {
    check(direction, channel);
    return 0.0;
}

// Sample rate and bandwidth

void SoapyDevice::setSampleRate(const int direction, const size_t channel, const double rate) {
    const Direction dir = check(direction, channel);
    // Outside the measured range the boards misbehave rather than fail:
    // the receiver degrades, the transmitter goes silent or loses a channel.
    const double minimum = dir == Direction::kRx ? kMinRxSampleRate : kMinTxSampleRate;
    if (!(rate >= minimum && rate <= kMaxSampleRate)) {
        std::ostringstream text;
        text << "setSampleRate: " << rate / 1e6 << " Msps is outside the "
             << (dir == Direction::kRx ? "receive" : "transmit") << " range, " << minimum / 1e6
             << " to " << kMaxSampleRate / 1e6 << " Msps";
        throw std::invalid_argument(text.str());
    }
    pair_->set_sample_rate(dir, rate);
}

double SoapyDevice::getSampleRate(const int direction, const size_t channel) const {
    check_channel(channel);
    return pair_->sample_rate(direction_of(direction));
}

std::vector<double> SoapyDevice::listSampleRates(const int direction, const size_t channel) const {
    if (check(direction, channel) == Direction::kRx) {
        return {250e3, 500e3, 1e6, 1.92e6, 2e6, 4e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
    }
    return {1.92e6, 2e6, 4e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
}

SoapySDR::RangeList SoapyDevice::getSampleRateRange(const int direction,
                                                    const size_t channel) const {
    const double minimum =
        check(direction, channel) == Direction::kRx ? kMinRxSampleRate : kMinTxSampleRate;
    return {SoapySDR::Range(minimum, kMaxSampleRate)};
}

void SoapyDevice::setBandwidth(const int direction, const size_t channel, const double bw) {
    check_channel(channel);
    const std::uint32_t hz = bw > 0 ? static_cast<std::uint32_t>(std::llround(bw)) : 0;
    pair_->set_bandwidth(direction_of(direction), hz);
}

double SoapyDevice::getBandwidth(const int direction, const size_t channel) const {
    check_channel(channel);
    return pair_->bandwidth(direction_of(direction));
}

std::vector<double> SoapyDevice::listBandwidths(const int direction, const size_t channel) const {
    check(direction, channel);
    std::vector<double> list;
    for (const std::uint32_t hz : filter_bandwidths()) {
        list.push_back(hz);
    }
    return list;
}

SoapySDR::RangeList SoapyDevice::getBandwidthRange(const int direction,
                                                   const size_t channel) const {
    check(direction, channel);
    SoapySDR::RangeList list;
    for (const std::uint32_t hz : filter_bandwidths()) {
        list.emplace_back(hz, hz);
    }
    return list;
}

// Time

bool SoapyDevice::hasHardwareTime(const std::string& what) const {
    return what.empty() && pair_->tx_of_rx().has_value();
}

long long SoapyDevice::getHardwareTime(const std::string& what) const {
    if (!what.empty()) {
        throw std::invalid_argument("getHardwareTime: no time source " + what);
    }
    const double rate = pair_->sample_rate(Direction::kRx);
    if (!(rate > 0)) {
        return 0;
    }
    return SoapySDR::ticksToTimeNs(pair_->time_ticks(), rate);
}

void SoapyDevice::setHardwareTime(const long long timeNs, const std::string& what) {
    if (!what.empty()) {
        throw std::invalid_argument("setHardwareTime: no time source " + what);
    }
    const double rate = pair_->sample_rate(Direction::kRx);
    if (!(rate > 0)) {
        throw std::logic_error("setHardwareTime: set the receive sample rate first");
    }
    pair_->set_time_ticks(SoapySDR::timeNsToTicks(timeNs, rate));
}

// Clock and time sources

std::vector<std::string> SoapyDevice::listClockSources() const { return {"internal", "rx", "tx"}; }

void SoapyDevice::setClockSource(const std::string& source) {
    if (source == "internal") {
        pair_->set_clock_out(ClockOut::kOff);
    } else if (source == "rx" || source == "tx") {
        pair_->set_clock_out(parse_clock_out(source));
    } else {
        refuse("setClockSource", "the clock sources are internal, rx and tx, not " + source);
    }
}

std::string SoapyDevice::getClockSource() const {
    const ClockOut which = pair_->clock_out();
    return which == ClockOut::kOff ? "internal" : clock_out_name(which);
}

std::vector<std::string> SoapyDevice::listTimeSources() const {
    return {"burst", "trigger", "none"};
}

void SoapyDevice::setTimeSource(const std::string& source) {
    pair_->set_time_origin(parse_time_origin(source));
}

std::string SoapyDevice::getTimeSource() const { return time_origin_name(pair_->time_origin()); }

// Sensors

std::vector<std::string> SoapyDevice::listSensors() const { return {"ref_locked", "time_locked"}; }

SoapySDR::ArgInfo SoapyDevice::getSensorInfo(const std::string& key) const {
    SoapySDR::ArgInfo info;
    info.key = key;
    info.value = "false";
    info.type = SoapySDR::ArgInfo::BOOL;
    if (key == "ref_locked") {
        info.name = "Reference locked";
        info.description =
            "The two boards run from one reference: one drives its CLKOUT into the other's CLKIN "
            "(clkout), or both are on CLKIN. A streaming board is not asked: what it reported at "
            "its start stands.";
    } else if (key == "time_locked") {
        info.name = "Time locked";
        info.description =
            "The streams share a common time origin, so transmit times and receive timestamps "
            "are on one clock (hasHardwareTime).";
    } else {
        throw std::invalid_argument("getSensorInfo: no sensor " + key);
    }
    return info;
}

std::string SoapyDevice::readSensor(const std::string& key) const {
    if (key == "ref_locked") {
        return pair_->shares_reference() ? "true" : "false";
    }
    if (key == "time_locked") {
        return pair_->time_locked() ? "true" : "false";
    }
    throw std::invalid_argument("readSensor: no sensor " + key);
}

std::vector<std::string> SoapyDevice::listSensors(const int direction, const size_t channel) const {
    direction_of(direction);
    check_channel(channel);
    return {"lo_locked"};
}

SoapySDR::ArgInfo SoapyDevice::getSensorInfo(const int direction, const size_t channel,
                                             const std::string& key) const {
    direction_of(direction);
    check_channel(channel);
    if (key != "lo_locked") {
        throw std::invalid_argument("getSensorInfo: no sensor " + key);
    }
    SoapySDR::ArgInfo info;
    info.key = "lo_locked";
    info.value = "true";
    info.name = "LO locked";
    info.description =
        "The synthesisers are tuned on the reference the board runs from. False only while a "
        "reference switch at stream start is being handled.";
    info.type = SoapySDR::ArgInfo::BOOL;
    return info;
}

std::string SoapyDevice::readSensor(const int direction, const size_t channel,
                                    const std::string& key) const {
    const Direction dir = direction_of(direction);
    check_channel(channel);
    if (key != "lo_locked") {
        throw std::invalid_argument("readSensor: no sensor " + key);
    }
    return pair_->lo_locked(dir) ? "true" : "false";
}

// Settings

SoapySDR::ArgInfoList SoapyDevice::getSettingInfo() const {
    SoapySDR::ArgInfo bias;
    bias.key = "bias_tx";
    bias.value = "false";
    bias.name = "TX antenna bias";
    bias.description = "3.3 V on the transmitting board's antenna port while it transmits.";
    bias.type = SoapySDR::ArgInfo::BOOL;

    SoapySDR::ArgInfo clkout;
    clkout.key = "clkout";
    clkout.value = "off";
    clkout.name = "Reference clock out";
    clkout.description =
        "Which board drives 10 MHz from its CLKOUT for the other's CLKIN, so both run from one "
        "reference. Off at every power-up.";
    clkout.type = SoapySDR::ArgInfo::STRING;
    clkout.options = {"off", "rx", "tx"};
    clkout.optionNames = {"Off", "RX board", "TX board"};

    SoapySDR::ArgInfo origin;
    origin.key = "time_origin";
    origin.value = "burst";
    origin.name = "Time origin";
    origin.description =
        "How the two streams get a common time when both have started: a short calibration "
        "burst on the receive frequency, one trigger edge (a jumper from the RX board's P28 pin "
        "15 to the TX board's pin 16), or none. Takes effect when the streams next start.";
    origin.type = SoapySDR::ArgInfo::STRING;
    origin.options = {"burst", "trigger", "none"};
    origin.optionNames = {"Calibration burst", "Trigger edge", "None"};

    SoapySDR::ArgInfo calibrate;
    calibrate.key = "calibrate";
    calibrate.value = "";
    calibrate.name = "Calibrate";
    calibrate.description =
        "Write any value to send the calibration burst now; both streams must be active.";
    calibrate.type = SoapySDR::ArgInfo::STRING;

    SoapySDR::ArgInfo limit;
    limit.key = "tx_underrun_limit";
    limit.value = std::to_string(pair_->tx_underrun_limit());
    limit.name = "TX underrun limit";
    limit.description =
        "The transmitting board stops by itself once it goes this many samples without data in "
        "one stretch, so it cannot transmit on after its host has died. 0 turns it off. Takes "
        "effect at the next transmit start.";
    limit.units = "samples";
    limit.type = SoapySDR::ArgInfo::INT;
    return {bias, clkout, origin, calibrate, limit};
}

void SoapyDevice::writeSetting(const std::string& key, const std::string& value) {
    if (key == "bias_tx") {
        if (value == "true" || value == "1") {
            pair_->set_bias_tee(true);
        } else if (value == "false" || value == "0") {
            pair_->set_bias_tee(false);
        } else {
            refuse("writeSetting", "bias_tx is true or false, not " + value);
        }
        return;
    }
    if (key == "clkout") {
        pair_->set_clock_out(parse_clock_out(value));
        return;
    }
    if (key == "time_origin") {
        pair_->set_time_origin(parse_time_origin(value));
        return;
    }
    if (key == "calibrate") {
        pair_->calibrate();
        return;
    }
    if (key == "tx_underrun_limit") {
        pair_->set_tx_underrun_limit(parse_samples("tx_underrun_limit", value));
        return;
    }
    // Applications write vendor settings blindly; one this device lacks is
    // not the caller's error.
    soapy_log(LogLevel::kWarning, "writeSetting: no setting " + key + ", ignored");
}

std::string SoapyDevice::readSetting(const std::string& key) const {
    if (key == "bias_tx") {
        return pair_->bias_tee() ? "true" : "false";
    }
    if (key == "clkout") {
        return clock_out_name(pair_->clock_out());
    }
    if (key == "time_origin") {
        return time_origin_name(pair_->time_origin());
    }
    if (key == "tx_underrun_limit") {
        return std::to_string(pair_->tx_underrun_limit());
    }
    return {};
}

void SoapyDevice::writeSetting(const int direction, const size_t channel, const std::string& key,
                               const std::string&) {
    check(direction, channel);
    soapy_log(LogLevel::kWarning, "writeSetting: no channel setting " + key + ", ignored");
}

// Refused

void SoapyDevice::setFrontendMapping(const int direction, const std::string& mapping) {
    direction_of(direction);
    if (!mapping.empty()) {
        refuse("setFrontendMapping", "each direction has one channel on its own board");
    }
}

void SoapyDevice::setMasterClockRate(const double) {
    refuse("setMasterClockRate", "the sample rate sets the clock; use setSampleRate");
}

void SoapyDevice::setReferenceClockRate(const double) {
    refuse("setReferenceClockRate", "the reference is the boards' own 10 MHz");
}

void SoapyDevice::setCommandTime(const long long, const std::string&) {
    refuse("setCommandTime", "settings take effect at once; there are no timed commands");
}

void SoapyDevice::writeRegister(const std::string& name, const unsigned, const unsigned) {
    refuse("writeRegister", "no register interface " + name);
}

void SoapyDevice::writeRegister(const unsigned, const unsigned) {
    refuse("writeRegister", "no register interface");
}

void SoapyDevice::writeRegisters(const std::string& name, const unsigned,
                                 const std::vector<unsigned>&) {
    refuse("writeRegisters", "no register interface " + name);
}

void SoapyDevice::writeGPIO(const std::string& bank, const unsigned) {
    refuse("writeGPIO", "no GPIO bank " + bank);
}

void SoapyDevice::writeGPIO(const std::string& bank, const unsigned, const unsigned) {
    refuse("writeGPIO", "no GPIO bank " + bank);
}

void SoapyDevice::writeGPIODir(const std::string& bank, const unsigned) {
    refuse("writeGPIODir", "no GPIO bank " + bank);
}

void SoapyDevice::writeGPIODir(const std::string& bank, const unsigned, const unsigned) {
    refuse("writeGPIODir", "no GPIO bank " + bank);
}

void SoapyDevice::writeI2C(const int, const std::string&) { refuse("writeI2C", "no I2C bus"); }

unsigned SoapyDevice::transactSPI(const int, const unsigned, const size_t) {
    refuse("transactSPI", "no SPI bus");
}

void SoapyDevice::writeUART(const std::string& which, const std::string&) {
    refuse("writeUART", "no UART " + which);
}

}  // namespace hackrfduplex
