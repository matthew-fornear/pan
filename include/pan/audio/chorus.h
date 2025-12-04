#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <vector>
#include <cmath>

namespace pan {

/**
 * Chorus effect - creates a richer sound by mixing delayed, pitch-modulated copies
 * 
 * Algorithm:
 * 1. Delay the input signal by a base delay time (typically 20-30ms)
 * 2. Modulate the delay time with an LFO (sine wave)
 * 3. Mix the modulated signal with the dry signal
 */
class Chorus : public Effect {
public:
    Chorus(double sampleRate);
    virtual ~Chorus() = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Chorus"; }
    void reset() override;
    
    // Parameters
    // Presets
    enum class Preset {
        Subtle,      // Light chorus
        Classic,     // Standard chorus
        Deep,        // Heavy modulation
        Detune,      // Subtle pitch variation
        Vibrato,     // Fast, shallow
        Custom
    };
    
    void loadPreset(Preset preset);
    Preset getCurrentPreset() const { return currentPreset_; }
    void setCurrentPreset(Preset p) { currentPreset_ = p; }
    static const char* getPresetName(Preset preset);
    
    void setRate(float hz) { rate_ = std::max(0.1f, std::min(5.0f, hz)); }       // LFO rate (0.1-5 Hz)
    void setDepth(float ms) { depth_ = std::max(0.0f, std::min(10.0f, ms)); }    // Modulation depth (0-10ms)
    void setDelay(float ms) { baseDelay_ = std::max(5.0f, std::min(50.0f, ms)); } // Base delay (5-50ms)
    void setMix(float mix) { mix_ = std::max(0.0f, std::min(1.0f, mix)); }       // Wet/dry (0-1)
    
    float getRate() const { return rate_; }
    float getDepth() const { return depth_; }
    float getDelay() const { return baseDelay_; }
    float getMix() const { return mix_; }

private:
    double sampleRate_;
    
    // Parameters
    float rate_ = 1.5f;       // LFO frequency in Hz
    float depth_ = 3.0f;      // Modulation depth in ms
    float baseDelay_ = 25.0f; // Base delay time in ms
    float mix_ = 0.5f;        // Wet/dry mix
    Preset currentPreset_ = Preset::Classic;
    
    // LFO state
    double lfoPhase_ = 0.0;
    
    // Delay buffer (circular)
    std::vector<float> delayBufferL_;
    std::vector<float> delayBufferR_;
    size_t writePos_ = 0;
    size_t maxDelaySamples_;
    
    // Helper to read from delay buffer with interpolation
    float readDelayInterpolated(const std::vector<float>& buffer, float delaySamples);
};

} // namespace pan

