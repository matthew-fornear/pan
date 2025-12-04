#include "pan/audio/chorus.h"
#include <algorithm>

namespace pan {

const char* Chorus::getPresetName(Preset preset) {
    switch (preset) {
        case Preset::Subtle: return "Subtle";
        case Preset::Classic: return "Classic";
        case Preset::Deep: return "Deep";
        case Preset::Detune: return "Detune";
        case Preset::Vibrato: return "Vibrato";
        case Preset::Custom: return "Custom";
        default: return "Unknown";
    }
}

void Chorus::loadPreset(Preset preset) {
    currentPreset_ = preset;
    switch (preset) {
        case Preset::Subtle:
            rate_ = 0.5f; depth_ = 1.5f; baseDelay_ = 20.0f; mix_ = 0.3f;
            break;
        case Preset::Classic:
            rate_ = 1.5f; depth_ = 3.0f; baseDelay_ = 25.0f; mix_ = 0.5f;
            break;
        case Preset::Deep:
            rate_ = 0.8f; depth_ = 6.0f; baseDelay_ = 30.0f; mix_ = 0.6f;
            break;
        case Preset::Detune:
            rate_ = 0.3f; depth_ = 2.0f; baseDelay_ = 15.0f; mix_ = 0.4f;
            break;
        case Preset::Vibrato:
            rate_ = 4.0f; depth_ = 2.5f; baseDelay_ = 10.0f; mix_ = 0.7f;
            break;
        case Preset::Custom:
            // Don't change parameters
            break;
    }
}

Chorus::Chorus(double sampleRate)
    : sampleRate_(sampleRate)
{
    // Allocate delay buffer for max delay + modulation depth (60ms should be plenty)
    maxDelaySamples_ = static_cast<size_t>(sampleRate_ * 0.06);  // 60ms max
    delayBufferL_.resize(maxDelaySamples_, 0.0f);
    delayBufferR_.resize(maxDelaySamples_, 0.0f);
    writePos_ = 0;
    lfoPhase_ = 0.0;
}

void Chorus::reset() {
    std::fill(delayBufferL_.begin(), delayBufferL_.end(), 0.0f);
    std::fill(delayBufferR_.begin(), delayBufferR_.end(), 0.0f);
    writePos_ = 0;
    lfoPhase_ = 0.0;
}

float Chorus::readDelayInterpolated(const std::vector<float>& buffer, float delaySamples) {
    // Linear interpolation for smooth delay modulation
    float readPosF = static_cast<float>(writePos_) - delaySamples;
    if (readPosF < 0) {
        readPosF += static_cast<float>(maxDelaySamples_);
    }
    
    size_t readPos0 = static_cast<size_t>(readPosF) % maxDelaySamples_;
    size_t readPos1 = (readPos0 + 1) % maxDelaySamples_;
    float frac = readPosF - std::floor(readPosF);
    
    return buffer[readPos0] * (1.0f - frac) + buffer[readPos1] * frac;
}

void Chorus::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    // Convert parameters to samples
    float baseDelaySamples = (baseDelay_ / 1000.0f) * static_cast<float>(sampleRate_);
    float depthSamples = (depth_ / 1000.0f) * static_cast<float>(sampleRate_);
    
    // LFO increment per sample
    double lfoIncrement = (2.0 * M_PI * rate_) / sampleRate_;
    
    for (size_t i = 0; i < numFrames; ++i) {
        // Calculate LFO value (sine wave, -1 to +1)
        float lfoValue = static_cast<float>(std::sin(lfoPhase_));
        lfoPhase_ += lfoIncrement;
        if (lfoPhase_ >= 2.0 * M_PI) {
            lfoPhase_ -= 2.0 * M_PI;
        }
        
        // Modulated delay time
        float currentDelaySamples = baseDelaySamples + (lfoValue * depthSamples);
        currentDelaySamples = std::max(1.0f, std::min(currentDelaySamples, static_cast<float>(maxDelaySamples_ - 1)));
        
        // Write dry signal to delay buffer
        delayBufferL_[writePos_] = left[i];
        delayBufferR_[writePos_] = right[i];
        
        // Read delayed signal with interpolation
        float delayedL = readDelayInterpolated(delayBufferL_, currentDelaySamples);
        float delayedR = readDelayInterpolated(delayBufferR_, currentDelaySamples);
        
        // Mix dry and wet
        left[i] = left[i] * (1.0f - mix_) + delayedL * mix_;
        right[i] = right[i] * (1.0f - mix_) + delayedR * mix_;
        
        // Advance write position
        writePos_ = (writePos_ + 1) % maxDelaySamples_;
    }
}

} // namespace pan

