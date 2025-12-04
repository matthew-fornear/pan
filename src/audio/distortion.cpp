#include "pan/audio/distortion.h"
#include <algorithm>

namespace pan {

const char* Distortion::getPresetName(Preset preset) {
    switch (preset) {
        case Preset::Warm: return "Warm";
        case Preset::Crunch: return "Crunch";
        case Preset::Heavy: return "Heavy";
        case Preset::Fuzz_Preset: return "Fuzz";
        case Preset::Screamer: return "Screamer";
        case Preset::Custom: return "Custom";
        default: return "Unknown";
    }
}

void Distortion::loadPreset(Preset preset) {
    currentPreset_ = preset;
    switch (preset) {
        case Preset::Warm:
            drive_ = 5.0f; tone_ = 0.6f; mix_ = 0.5f; type_ = Type::SoftClip;
            break;
        case Preset::Crunch:
            drive_ = 15.0f; tone_ = 0.5f; mix_ = 0.6f; type_ = Type::Overdrive;
            break;
        case Preset::Heavy:
            drive_ = 40.0f; tone_ = 0.4f; mix_ = 0.8f; type_ = Type::HardClip;
            break;
        case Preset::Fuzz_Preset:
            drive_ = 60.0f; tone_ = 0.35f; mix_ = 0.9f; type_ = Type::Fuzz;
            break;
        case Preset::Screamer:
            drive_ = 20.0f; tone_ = 0.7f; mix_ = 0.65f; type_ = Type::Overdrive;
            break;
        case Preset::Custom:
            // Don't change parameters
            break;
    }
}

Distortion::Distortion(double sampleRate)
    : sampleRate_(sampleRate)
    , filterStateL_(0.0f)
    , filterStateR_(0.0f)
{
}

void Distortion::reset() {
    filterStateL_ = 0.0f;
    filterStateR_ = 0.0f;
}

float Distortion::waveshape(float input) {
    switch (type_) {
        case Type::SoftClip:
            // Tanh soft clipping - smooth, tube-like saturation
            return std::tanh(input);
            
        case Type::HardClip:
            // Digital hard clipping - harsh, aggressive
            return std::max(-1.0f, std::min(1.0f, input));
            
        case Type::Overdrive:
            // Asymmetric soft clipping - amp-like character
            // Positive side clips harder than negative
            if (input > 0) {
                return 1.0f - std::exp(-input);
            } else {
                return -1.0f + std::exp(input);
            }
            
        case Type::Fuzz:
            // Extreme distortion - almost square wave
            // Uses a steeper sigmoid
            return std::tanh(input * 3.0f) * 0.9f + std::tanh(input) * 0.1f;
            
        default:
            return std::tanh(input);
    }
}

void Distortion::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    // Calculate low-pass filter coefficient from tone parameter
    // tone_ = 0 -> very dark (low cutoff), tone_ = 1 -> bright (high cutoff)
    float cutoffHz = 500.0f + tone_ * 15000.0f;  // 500Hz to 15.5kHz
    float filterCoeff = 1.0f - std::exp(-2.0f * M_PI * cutoffHz / static_cast<float>(sampleRate_));
    
    // Normalize output based on drive to maintain consistent volume
    float outputGain = 1.0f / std::sqrt(drive_ * 0.5f);
    outputGain = std::max(0.1f, std::min(1.0f, outputGain));
    
    for (size_t i = 0; i < numFrames; ++i) {
        // Store dry signal
        float dryL = left[i];
        float dryR = right[i];
        
        // Apply drive (input gain)
        float wetL = dryL * drive_;
        float wetR = dryR * drive_;
        
        // Apply waveshaping
        wetL = waveshape(wetL);
        wetR = waveshape(wetR);
        
        // Apply tone control (one-pole low-pass filter)
        filterStateL_ += filterCoeff * (wetL - filterStateL_);
        filterStateR_ += filterCoeff * (wetR - filterStateR_);
        wetL = filterStateL_;
        wetR = filterStateR_;
        
        // Apply output gain normalization
        wetL *= outputGain;
        wetR *= outputGain;
        
        // Mix dry and wet
        left[i] = dryL * (1.0f - mix_) + wetL * mix_;
        right[i] = dryR * (1.0f - mix_) + wetR * mix_;
    }
}

} // namespace pan

