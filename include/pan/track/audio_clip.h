#pragma once

#include <string>
#include <memory>
#include <vector>
#include "pan/audio/audio_buffer.h"

namespace pan {

/**
 * Represents an audio clip that can be placed on a track
 */
class AudioClip {
public:
    AudioClip(const std::string& name);
    ~AudioClip();

    // Clip properties
    std::string getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    
    // Timeline position (in samples)
    int64_t getStartTime() const { return startTime_; }
    void setStartTime(int64_t startTime) { startTime_ = startTime; }
    
    int64_t getEndTime() const { return endTime_; }
    int64_t getLength() const { return endTime_ - startTime_; }
    
    // Audio data
    void setAudioData(std::shared_ptr<AudioBuffer> buffer);
    std::shared_ptr<AudioBuffer> getAudioData() const { return audioData_; }
    
    bool hasAudioData() const { return audioData_ != nullptr; }
    
    // Playback state
    bool isPlaying() const { return isPlaying_; }
    void setPlaying(bool playing) { isPlaying_ = playing; }
    
    // Gain/volume for this clip
    float getGain() const { return gain_; }
    void setGain(float gain);

private:
    std::string name_;
    int64_t startTime_;  // Start position in timeline (samples)
    int64_t endTime_;    // End position in timeline (samples)
    
    std::shared_ptr<AudioBuffer> audioData_;  // The actual audio samples
    bool isPlaying_;
    float gain_;  // Clip gain (0.0 to 2.0)
};

} // namespace pan

