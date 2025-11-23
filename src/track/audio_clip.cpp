#include "pan/track/audio_clip.h"
#include <algorithm>

namespace pan {

AudioClip::AudioClip(const std::string& name)
    : name_(name)
    , startTime_(0)
    , endTime_(0)
    , isPlaying_(false)
    , gain_(1.0f)
{
}

AudioClip::~AudioClip() = default;

void AudioClip::setGain(float gain) {
    gain_ = std::clamp(gain, 0.0f, 2.0f);
}

void AudioClip::setAudioData(std::shared_ptr<AudioBuffer> buffer) {
    audioData_ = buffer;
    if (audioData_) {
        // Update end time based on audio data length
        endTime_ = startTime_ + static_cast<int64_t>(audioData_->getNumFrames());
    }
}

} // namespace pan

