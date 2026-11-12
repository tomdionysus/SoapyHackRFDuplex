// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Types.hpp>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "pair.hpp"

namespace hackrfduplex {

// The SoapySDR face of a Pair: one RX channel on one board, one TX
// channel on the other, full duplex. Everything here is translation; the
// behaviour is the pair's.
class SoapyDevice final : public SoapySDR::Device {
public:
    // Opens the boards `args` select (selection.hpp). Throws
    // std::runtime_error when they cannot be chosen or opened.
    explicit SoapyDevice(const SoapySDR::Kwargs& args);
    // Over a pair already made, which is how the tests use it; `args` are
    // the device arguments beyond the board selection: the settings but
    // calibrate.
    SoapyDevice(std::unique_ptr<Pair> pair, const SoapySDR::Kwargs& args);
    ~SoapyDevice() override;

    // Identification
    std::string getDriverKey() const override;
    std::string getHardwareKey() const override;
    SoapySDR::Kwargs getHardwareInfo() const override;

    // Channels
    size_t getNumChannels(const int direction) const override;
    bool getFullDuplex(const int direction, const size_t channel) const override;

    // Streams
    std::vector<std::string> getStreamFormats(const int direction,
                                              const size_t channel) const override;
    std::string getNativeStreamFormat(const int direction, const size_t channel,
                                      double& fullScale) const override;
    SoapySDR::ArgInfoList getStreamArgsInfo(const int direction,
                                            const size_t channel) const override;
    SoapySDR::Stream* setupStream(const int direction, const std::string& format,
                                  const std::vector<size_t>& channels,
                                  const SoapySDR::Kwargs& args) override;
    void closeStream(SoapySDR::Stream* stream) override;
    size_t getStreamMTU(SoapySDR::Stream* stream) const override;
    int activateStream(SoapySDR::Stream* stream, const int flags, const long long timeNs,
                       const size_t numElems) override;
    int deactivateStream(SoapySDR::Stream* stream, const int flags,
                         const long long timeNs) override;
    int readStream(SoapySDR::Stream* stream, void* const* buffs, const size_t numElems, int& flags,
                   long long& timeNs, const long timeoutUs) override;
    int writeStream(SoapySDR::Stream* stream, const void* const* buffs, const size_t numElems,
                    int& flags, const long long timeNs, const long timeoutUs) override;
    int readStreamStatus(SoapySDR::Stream* stream, size_t& chanMask, int& flags, long long& timeNs,
                         const long timeoutUs) override;

    // Direct access: a CS8 receive stream's ring, one buffer held at a time.
    // Nothing on a stream in another format, which converts, or on the
    // transmit stream.
    size_t getNumDirectAccessBuffers(SoapySDR::Stream* stream) override;
    int getDirectAccessBufferAddrs(SoapySDR::Stream* stream, const size_t handle,
                                   void** buffs) override;
    int acquireReadBuffer(SoapySDR::Stream* stream, size_t& handle, const void** buffs, int& flags,
                          long long& timeNs, const long timeoutUs) override;
    void releaseReadBuffer(SoapySDR::Stream* stream, const size_t handle) override;

    // Antenna
    std::vector<std::string> listAntennas(const int direction, const size_t channel) const override;
    void setAntenna(const int direction, const size_t channel, const std::string& name) override;
    std::string getAntenna(const int direction, const size_t channel) const override;

    // Gain
    std::vector<std::string> listGains(const int direction, const size_t channel) const override;
    bool hasGainMode(const int direction, const size_t channel) const override;
    void setGain(const int direction, const size_t channel, const double value) override;
    void setGain(const int direction, const size_t channel, const std::string& name,
                 const double value) override;
    double getGain(const int direction, const size_t channel) const override;
    double getGain(const int direction, const size_t channel,
                   const std::string& name) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel) const override;
    SoapySDR::Range getGainRange(const int direction, const size_t channel,
                                 const std::string& name) const override;

    // Frequency
    void setFrequency(const int direction, const size_t channel, const double frequency,
                      const SoapySDR::Kwargs& args) override;
    void setFrequency(const int direction, const size_t channel, const std::string& name,
                      const double frequency, const SoapySDR::Kwargs& args) override;
    double getFrequency(const int direction, const size_t channel) const override;
    double getFrequency(const int direction, const size_t channel,
                        const std::string& name) const override;
    std::vector<std::string> listFrequencies(const int direction,
                                             const size_t channel) const override;
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel) const override;
    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t channel,
                                          const std::string& name) const override;

    // Corrections and modes: none in hardware. Setting the neutral value
    // (manual, off, zero) is honoured; anything else is refused.
    void setGainMode(const int direction, const size_t channel, const bool automatic) override;
    bool getGainMode(const int direction, const size_t channel) const override;
    bool hasDCOffsetMode(const int direction, const size_t channel) const override;
    void setDCOffsetMode(const int direction, const size_t channel, const bool automatic) override;
    bool getDCOffsetMode(const int direction, const size_t channel) const override;
    bool hasDCOffset(const int direction, const size_t channel) const override;
    void setDCOffset(const int direction, const size_t channel,
                     const std::complex<double>& offset) override;
    std::complex<double> getDCOffset(const int direction, const size_t channel) const override;
    bool hasIQBalance(const int direction, const size_t channel) const override;
    void setIQBalance(const int direction, const size_t channel,
                      const std::complex<double>& balance) override;
    std::complex<double> getIQBalance(const int direction, const size_t channel) const override;
    bool hasIQBalanceMode(const int direction, const size_t channel) const override;
    void setIQBalanceMode(const int direction, const size_t channel, const bool automatic) override;
    bool getIQBalanceMode(const int direction, const size_t channel) const override;
    bool hasFrequencyCorrection(const int direction, const size_t channel) const override;
    void setFrequencyCorrection(const int direction, const size_t channel,
                                const double value) override;
    double getFrequencyCorrection(const int direction, const size_t channel) const override;

    // Sample rate and bandwidth
    void setSampleRate(const int direction, const size_t channel, const double rate) override;
    double getSampleRate(const int direction, const size_t channel) const override;
    std::vector<double> listSampleRates(const int direction, const size_t channel) const override;
    SoapySDR::RangeList getSampleRateRange(const int direction,
                                           const size_t channel) const override;
    void setBandwidth(const int direction, const size_t channel, const double bw) override;
    double getBandwidth(const int direction, const size_t channel) const override;
    std::vector<double> listBandwidths(const int direction, const size_t channel) const override;
    SoapySDR::RangeList getBandwidthRange(const int direction, const size_t channel) const override;

    // Time: receive samples since the receive stream started, plus an
    // offset setHardwareTime sets; shared with the transmitter while the
    // streams have a common origin, which is when hasHardwareTime is true.
    bool hasHardwareTime(const std::string& what) const override;
    long long getHardwareTime(const std::string& what) const override;
    void setHardwareTime(const long long timeNs, const std::string& what) override;

    // Clock and time sources: the clkout and time_origin settings by the
    // names SoapySDR gives them. Clock sources: internal (clkout off), rx,
    // tx. Time sources: burst, trigger, none.
    std::vector<std::string> listClockSources() const override;
    void setClockSource(const std::string& source) override;
    std::string getClockSource() const override;
    std::vector<std::string> listTimeSources() const override;
    void setTimeSource(const std::string& source) override;
    std::string getTimeSource() const override;

    // Sensors
    // The device's: ref_locked (Pair::shares_reference) and time_locked
    // (an origin holds). Each channel's: lo_locked.
    std::vector<std::string> listSensors() const override;
    SoapySDR::ArgInfo getSensorInfo(const std::string& key) const override;
    std::string readSensor(const std::string& key) const override;
    std::vector<std::string> listSensors(const int direction, const size_t channel) const override;
    SoapySDR::ArgInfo getSensorInfo(const int direction, const size_t channel,
                                    const std::string& key) const override;
    std::string readSensor(const int direction, const size_t channel,
                           const std::string& key) const override;

    // Settings. There are no per-channel settings; one written is logged
    // and ignored, as an unknown device setting is.
    using SoapySDR::Device::getSettingInfo;
    using SoapySDR::Device::readSetting;
    using SoapySDR::Device::writeSetting;
    SoapySDR::ArgInfoList getSettingInfo() const override;
    void writeSetting(const std::string& key, const std::string& value) override;
    std::string readSetting(const std::string& key) const override;
    void writeSetting(const int direction, const size_t channel, const std::string& key,
                      const std::string& value) override;

    // What a HackRF pair does not offer through SoapySDR: refused, never
    // ignored. The matching listings are empty and the reads SoapySDR's.
    void setFrontendMapping(const int direction, const std::string& mapping) override;
    void setMasterClockRate(const double rate) override;
    void setReferenceClockRate(const double rate) override;
    void setCommandTime(const long long timeNs, const std::string& what) override;
    void writeRegister(const std::string& name, const unsigned addr, const unsigned value) override;
    void writeRegister(const unsigned addr, const unsigned value) override;
    void writeRegisters(const std::string& name, const unsigned addr,
                        const std::vector<unsigned>& value) override;
    void writeGPIO(const std::string& bank, const unsigned value) override;
    void writeGPIO(const std::string& bank, const unsigned value, const unsigned mask) override;
    void writeGPIODir(const std::string& bank, const unsigned dir) override;
    void writeGPIODir(const std::string& bank, const unsigned dir, const unsigned mask) override;
    void writeI2C(const int addr, const std::string& data) override;
    unsigned transactSPI(const int addr, const unsigned data, const size_t numBits) override;
    void writeUART(const std::string& which, const std::string& data) override;

private:
    static Direction direction_of(int direction);
    Direction direction_of(SoapySDR::Stream* stream) const;
    SoapySDR::Stream* token(Direction direction) const;
    static void check_channel(const size_t channel);
    // Both, throwing std::invalid_argument for either.
    static Direction check(const int direction, const size_t channel);
    // 0 when the receive stream is running; else, after waiting up to the
    // timeout for it to start, SOAPY_SDR_TIMEOUT, or SOAPY_SDR_STREAM_ERROR
    // at once when its board stopped.
    int await_rx(long timeoutUs) const;
    // A read's flags, time and return value, as readStream gives them.
    int report_read(const RxRead& read, int& flags, long long& timeNs) const;

    std::unique_ptr<Pair> pair_;
    // Stream handles: addresses of these, one per direction.
    int rx_token_ = 0;
    int tx_token_ = 1;
};

}  // namespace hackrfduplex
