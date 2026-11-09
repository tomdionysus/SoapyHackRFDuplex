// SPDX-License-Identifier: GPL-3.0-or-later
#include "soapy_device.hpp"

#include <SoapySDR/Constants.h>

#include <SoapySDR/Errors.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <thread>

#include "bandwidth.hpp"
#include "convert.hpp"
#include "hackrf_board.hpp"
#include "hackrfduplex/version.hpp"
#include "selection.hpp"

namespace hackrfduplex {

namespace {

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
    const auto origin = args.find("time_origin");
    if (origin != args.end()) {
        pair_->set_time_origin(parse_time_origin(origin->second));
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

// Identification

std::string SoapyDevice::getDriverKey() const { return "hackrfduplex"; }

std::string SoapyDevice::getHardwareKey() const { return "HackRF Duplex"; }

SoapySDR::Kwargs SoapyDevice::getHardwareInfo() const {
    SoapySDR::Kwargs info;
    for (Direction direction : {Direction::kRx, Direction::kTx}) {
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

bool SoapyDevice::getFullDuplex(const int, const size_t) const { return true; }

// Streams

std::vector<std::string> SoapyDevice::getStreamFormats(const int, const size_t) const {
    return {SOAPY_SDR_CS8, SOAPY_SDR_CS16, SOAPY_SDR_CF32, SOAPY_SDR_CF64};
}

std::string SoapyDevice::getNativeStreamFormat(const int, const size_t, double& fullScale) const {
    fullScale = kFullScale;
    return SOAPY_SDR_CS8;
}

SoapySDR::ArgInfoList SoapyDevice::getStreamArgsInfo(const int, const size_t) const {
    SoapySDR::ArgInfo buffers;
    buffers.key = "buffers";
    buffers.value = std::to_string(kDefaultBuffers);
    buffers.name = "Buffer count";
    buffers.description = "USB transfers the stream can hold between the board and the caller.";
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
    std::size_t buffers = kDefaultBuffers;
    const auto it = args.find("buffers");
    if (it != args.end()) {
        const int wanted = std::stoi(it->second);
        if (wanted <= 0) {
            throw std::invalid_argument("setupStream: buffers must be positive");
        }
        buffers = static_cast<std::size_t>(wanted);
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

int SoapyDevice::activateStream(SoapySDR::Stream* stream, const int, const long long,
                                const size_t) {
    try {
        pair_->activate(direction_of(stream));
    } catch (const BoardError& e) {
        soapy_log(LogLevel::kError, e.what());
        return SOAPY_SDR_STREAM_ERROR;
    }
    return 0;
}

int SoapyDevice::deactivateStream(SoapySDR::Stream* stream, const int, const long long) {
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
    const RxRead read = pair_->read(buffs[0], numElems, timeout_of(timeoutUs));
    flags = 0;
    if (read.overflow) {
        flags |= SOAPY_SDR_END_ABRUPT;
        return SOAPY_SDR_OVERFLOW;
    }
    if (read.samples == 0) {
        return pair_->failed(Direction::kRx) ? SOAPY_SDR_STREAM_ERROR : SOAPY_SDR_TIMEOUT;
    }
    flags |= SOAPY_SDR_HAS_TIME;
    timeNs = SoapySDR::ticksToTimeNs(static_cast<long long>(read.position) + pair_->time_offset(),
                                     pair_->sample_rate(Direction::kRx));
    return static_cast<int>(read.samples);
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

std::vector<std::string> SoapyDevice::listAntennas(const int, const size_t) const {
    return {"TX/RX"};
}

void SoapyDevice::setAntenna(const int, const size_t, const std::string& name) {
    if (name != "TX/RX") {
        throw std::invalid_argument("setAntenna: the HackRF has one port, TX/RX");
    }
}

std::string SoapyDevice::getAntenna(const int, const size_t) const { return "TX/RX"; }

// Gain

std::vector<std::string> SoapyDevice::listGains(const int direction, const size_t) const {
    // gr-osmosdr expects the IF gain first and the baseband gain last.
    if (direction_of(direction) == Direction::kRx) {
        return {"LNA", "AMP", "VGA"};
    }
    return {"VGA", "AMP"};
}

bool SoapyDevice::hasGainMode(const int, const size_t) const { return false; }

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

SoapySDR::Range SoapyDevice::getGainRange(const int direction, const size_t) const {
    if (direction_of(direction) == Direction::kRx) {
        return SoapySDR::Range(0, gain::kRxMaxDb, 1);
    }
    return SoapySDR::Range(0, gain::kTxMaxDb, 1);
}

SoapySDR::Range SoapyDevice::getGainRange(const int direction, const size_t,
                                          const std::string& name) const {
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
                               const double frequency, const SoapySDR::Kwargs&) {
    check_channel(channel);
    if (name != "RF") {
        throw std::invalid_argument("setFrequency: unknown component " + name);
    }
    if (!(frequency >= 0)) {
        throw std::invalid_argument("setFrequency: frequency must be positive");
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

std::vector<std::string> SoapyDevice::listFrequencies(const int, const size_t) const {
    return {"RF"};
}

SoapySDR::RangeList SoapyDevice::getFrequencyRange(const int direction,
                                                   const size_t channel) const {
    return getFrequencyRange(direction, channel, "RF");
}

SoapySDR::RangeList SoapyDevice::getFrequencyRange(const int, const size_t,
                                                   const std::string& name) const {
    if (name != "RF") {
        throw std::invalid_argument("getFrequencyRange: unknown component " + name);
    }
    return {SoapySDR::Range(kMinFrequency, kMaxFrequency)};
}

// Sample rate and bandwidth

void SoapyDevice::setSampleRate(const int direction, const size_t channel, const double rate) {
    check_channel(channel);
    pair_->set_sample_rate(direction_of(direction), rate);
}

double SoapyDevice::getSampleRate(const int direction, const size_t channel) const {
    check_channel(channel);
    return pair_->sample_rate(direction_of(direction));
}

std::vector<double> SoapyDevice::listSampleRates(const int direction, const size_t) const {
    if (direction_of(direction) == Direction::kRx) {
        return {250e3, 500e3, 1e6, 1.92e6, 2e6, 4e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
    }
    return {1.92e6, 2e6, 4e6, 8e6, 10e6, 12.5e6, 16e6, 20e6};
}

SoapySDR::RangeList SoapyDevice::getSampleRateRange(const int direction, const size_t) const {
    const double minimum =
        direction_of(direction) == Direction::kRx ? kMinRxSampleRate : kMinTxSampleRate;
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

std::vector<double> SoapyDevice::listBandwidths(const int, const size_t) const {
    std::vector<double> list;
    for (const std::uint32_t hz : filter_bandwidths()) {
        list.push_back(hz);
    }
    return list;
}

SoapySDR::RangeList SoapyDevice::getBandwidthRange(const int, const size_t) const {
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

// Sensors

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
    return {bias, clkout, origin, calibrate};
}

void SoapyDevice::writeSetting(const std::string& key, const std::string& value) {
    if (key == "bias_tx") {
        pair_->set_bias_tee(value == "true" || value == "1");
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
    return {};
}

}  // namespace hackrfduplex
