#pragma once

#include <string>
#include <vector>
#include <cstddef>

namespace pan {

/**
 * Represents an audio input/output device
 */
class AudioDevice {
public:
    struct DeviceInfo {
        std::string name;
        size_t inputChannels;
        size_t outputChannels;
        std::vector<double> sampleRates;
        size_t defaultBufferSize;
    };

    AudioDevice();
    ~AudioDevice() = default;

    // Device information
    DeviceInfo getInfo() const { return info_; }
    void setInfo(const DeviceInfo& info) { info_ = info; }

    // Device properties
    std::string getName() const { return info_.name; }
    size_t getInputChannels() const { return info_.inputChannels; }
    size_t getOutputChannels() const { return info_.outputChannels; }

    // Static methods for device enumeration
    static std::vector<AudioDevice> enumerateDevices();
    static AudioDevice getDefaultInputDevice();
    static AudioDevice getDefaultOutputDevice();

private:
    DeviceInfo info_;
};

} // namespace pan

