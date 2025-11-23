#include "pan/audio/audio_buffer.h"
#include <algorithm>
#include <cstring>

namespace pan {

AudioBuffer::AudioBuffer(size_t numChannels, size_t numFrames)
    : numChannels_(numChannels)
    , numFrames_(numFrames)
    , channels_(numChannels)
{
    for (auto& channel : channels_) {
        channel.resize(numFrames, 0.0f);
    }
}

float* AudioBuffer::getWritePointer(size_t channel) {
    if (channel >= numChannels_) {
        return nullptr;
    }
    return channels_[channel].data();
}

const float* AudioBuffer::getReadPointer(size_t channel) const {
    if (channel >= numChannels_) {
        return nullptr;
    }
    return channels_[channel].data();
}

std::vector<float>& AudioBuffer::getChannel(size_t channel) {
    return channels_[channel];
}

const std::vector<float>& AudioBuffer::getChannel(size_t channel) const {
    return channels_[channel];
}

void AudioBuffer::clear() {
    for (auto& channel : channels_) {
        std::fill(channel.begin(), channel.end(), 0.0f);
    }
}

void AudioBuffer::fill(float value) {
    for (auto& channel : channels_) {
        std::fill(channel.begin(), channel.end(), value);
    }
}

void AudioBuffer::copyFrom(const AudioBuffer& other) {
    if (numChannels_ != other.numChannels_ || numFrames_ != other.numFrames_) {
        return;
    }
    
    for (size_t i = 0; i < numChannels_; ++i) {
        std::copy(other.channels_[i].begin(), other.channels_[i].end(), channels_[i].begin());
    }
}

void AudioBuffer::addFrom(const AudioBuffer& other, float gain) {
    if (numChannels_ != other.numChannels_ || numFrames_ != other.numFrames_) {
        return;
    }
    
    for (size_t i = 0; i < numChannels_; ++i) {
        for (size_t j = 0; j < numFrames_; ++j) {
            channels_[i][j] += other.channels_[i][j] * gain;
        }
    }
}

} // namespace pan

