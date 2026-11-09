// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Types.hpp>
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

    // Settings
    using SoapySDR::Device::getSettingInfo;
    using SoapySDR::Device::readSetting;
    using SoapySDR::Device::writeSetting;
    SoapySDR::ArgInfoList getSettingInfo() const override;
    void writeSetting(const std::string& key, const std::string& value) override;
    std::string readSetting(const std::string& key) const override;

private:
    static Direction direction_of(int direction);
    Direction direction_of(SoapySDR::Stream* stream) const;
    SoapySDR::Stream* token(Direction direction) const;
    static void check_channel(const size_t channel);

    std::unique_ptr<Pair> pair_;
    // Stream handles: addresses of these, one per direction.
    int rx_token_ = 0;
    int tx_token_ = 1;
};

}  // namespace hackrfduplex
