#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <thread>
#include <atomic>
#include "pan/midi/midi_message.h"

namespace pan {

/**
 * MIDI input device for receiving MIDI messages from hardware
 */
class MidiInput {
public:
    using MidiCallback = std::function<void(const MidiMessage&)>;

    MidiInput();
    ~MidiInput();

    // Device management
    static std::vector<std::string> enumerateDevices();
    bool openDevice(int deviceIndex);
    bool openDevice(const std::string& deviceName);
    void closeDevice();
    bool isOpen() const { return isOpen_; }

    // Start/stop receiving MIDI
    bool start();
    void stop();
    bool isRunning() const { return isRunning_; }

    // Set callback for received MIDI messages
    void setCallback(MidiCallback callback) { callback_ = callback; }

    // Get current device name
    std::string getDeviceName() const { return deviceName_; }

private:
    void midiThreadFunction();
    
    std::string deviceName_;
    int deviceIndex_;
    bool isOpen_;
    bool isRunning_;
    std::atomic<bool> shouldStop_;
    std::thread midiThread_;
    MidiCallback callback_;
    
    // ALSA sequencer handle (opaque pointer)
    void* sequencerHandle_;
    int portId_;
};

} // namespace pan

