#include "pan/audio/audio_device.h"
#include <iostream>
#include <vector>
#include <string>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

namespace pan {

AudioDevice::AudioDevice() {
    info_.name = "Default Device";
    info_.inputChannels = 2;
    info_.outputChannels = 2;
    info_.sampleRates = {44100.0, 48000.0, 96000.0};
    info_.defaultBufferSize = 512;
}

std::vector<AudioDevice> AudioDevice::enumerateDevices() {
    std::vector<AudioDevice> devices;
    
#ifdef PAN_USE_PORTAUDIO
    int numDevices = Pa_GetDeviceCount();
    if (numDevices < 0) {
        std::cerr << "Error getting device count: " << Pa_GetErrorText(numDevices) << std::endl;
        return devices;
    }
    
    for (int i = 0; i < numDevices; ++i) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(i);
        if (!deviceInfo) {
            continue;
        }
        
        AudioDevice device;
        DeviceInfo& info = device.info_;
        info.name = deviceInfo->name;
        info.inputChannels = deviceInfo->maxInputChannels;
        info.outputChannels = deviceInfo->maxOutputChannels;
        info.defaultBufferSize = static_cast<size_t>(deviceInfo->defaultLowOutputLatency * deviceInfo->defaultSampleRate);
        
        // Get supported sample rates
        static const double sampleRates[] = {8000.0, 11025.0, 16000.0, 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
        for (double rate : sampleRates) {
            PaStreamParameters testParams;
            testParams.device = i;
            testParams.channelCount = 1;
            testParams.sampleFormat = paFloat32;
            testParams.suggestedLatency = deviceInfo->defaultLowOutputLatency;
            testParams.hostApiSpecificStreamInfo = nullptr;
            
            if (Pa_IsFormatSupported(nullptr, &testParams, rate) == paFormatIsSupported) {
                info.sampleRates.push_back(rate);
            }
        }
        
        devices.push_back(device);
    }
#else
    // Fallback when PortAudio is not available
    AudioDevice defaultDevice;
    devices.push_back(defaultDevice);
#endif
    
    return devices;
}

AudioDevice AudioDevice::getDefaultInputDevice() {
    AudioDevice device;
    
#ifdef PAN_USE_PORTAUDIO
    PaDeviceIndex index = Pa_GetDefaultInputDevice();
    if (index != paNoDevice) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(index);
        if (deviceInfo) {
            device.info_.name = std::string("Default Input: ") + deviceInfo->name;
            device.info_.inputChannels = deviceInfo->maxInputChannels;
            device.info_.outputChannels = 0;
        }
    } else {
        device.info_.name = "No Input Device";
    }
#else
    device.info_.name = "Default Input";
#endif
    
    return device;
}

AudioDevice AudioDevice::getDefaultOutputDevice() {
    AudioDevice device;
    
#ifdef PAN_USE_PORTAUDIO
    PaDeviceIndex index = Pa_GetDefaultOutputDevice();
    if (index != paNoDevice) {
        const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(index);
        if (deviceInfo) {
            device.info_.name = std::string("Default Output: ") + deviceInfo->name;
            device.info_.inputChannels = 0;
            device.info_.outputChannels = deviceInfo->maxOutputChannels;
        }
    } else {
        device.info_.name = "No Output Device";
    }
#else
    device.info_.name = "Default Output";
#endif
    
    return device;
}

} // namespace pan

