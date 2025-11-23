#pragma once

#include <memory>
#include <vector>
#include <atomic>
#include <functional>

namespace pan {

class AudioBuffer;
class AudioDevice;

/**
 * Core audio engine responsible for real-time audio processing
 */
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Initialization
    bool initialize();
    void shutdown();

    // Audio device management
    bool setAudioDevice(std::shared_ptr<AudioDevice> device);
    std::shared_ptr<AudioDevice> getCurrentDevice() const;

    // Engine control
    bool start();
    bool stop();
    bool isRunning() const;

    // Sample rate and buffer size
    void setSampleRate(double sampleRate);
    double getSampleRate() const;
    void setBufferSize(size_t bufferSize);
    size_t getBufferSize() const;

    // Audio processing callback
    using ProcessCallback = std::function<void(AudioBuffer& input, AudioBuffer& output, size_t numFrames)>;
    void setProcessCallback(ProcessCallback callback);

    // Internal callback handler (for PortAudio) - public for callback access
    void processAudioCallback(AudioBuffer& input, AudioBuffer& output, size_t numFrames);
    
    // Methods for PortAudio callback to access buffers
    AudioBuffer* getInputBuffer();
    AudioBuffer* getOutputBuffer();
    
    // Internal method for callback to resize buffers if needed
    void resizeBuffersIfNeeded(size_t numChannels, size_t numFrames);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pan

