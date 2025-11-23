#pragma once

#include <vector>
#include <cstddef>

namespace pan {

/**
 * Audio buffer for storing multi-channel audio data
 */
class AudioBuffer {
public:
    AudioBuffer(size_t numChannels, size_t numFrames);
    ~AudioBuffer() = default;

    // Buffer properties
    size_t getNumChannels() const { return numChannels_; }
    size_t getNumFrames() const { return numFrames_; }
    size_t getSize() const { return numChannels_ * numFrames_; }

    // Data access
    float* getWritePointer(size_t channel);
    const float* getReadPointer(size_t channel) const;
    
    // Channel access
    std::vector<float>& getChannel(size_t channel);
    const std::vector<float>& getChannel(size_t channel) const;

    // Buffer operations
    void clear();
    void fill(float value);
    void copyFrom(const AudioBuffer& other);
    void addFrom(const AudioBuffer& other, float gain = 1.0f);

private:
    size_t numChannels_;
    size_t numFrames_;
    std::vector<std::vector<float>> channels_;
};

} // namespace pan

