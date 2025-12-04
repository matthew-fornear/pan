#include "pan/audio/eq8.h"
#include <algorithm>

namespace pan {

const char* EQ8::getPresetName(Preset preset) {
    switch (preset) {
        case Preset::Flat: return "Flat";
        case Preset::BassBoost: return "Bass Boost";
        case Preset::Presence: return "Presence";
        case Preset::Scooped: return "Scooped";
        case Preset::Bright: return "Bright";
        case Preset::Warm: return "Warm";
        case Preset::LoCut: return "Lo Cut";
        case Preset::Custom: return "Custom";
        default: return "Unknown";
    }
}

EQ8::EQ8(double sampleRate)
    : sampleRate_(sampleRate)
{
    // Initialize default band frequencies (logarithmically spaced)
    bands_[0] = {true, FilterType::LowCut, 30.0f, 0.0f, 0.7f};
    bands_[1] = {true, FilterType::LowShelf, 100.0f, 0.0f, 0.7f};
    bands_[2] = {true, FilterType::Peak, 200.0f, 0.0f, 1.0f};
    bands_[3] = {true, FilterType::Peak, 500.0f, 0.0f, 1.0f};
    bands_[4] = {true, FilterType::Peak, 1000.0f, 0.0f, 1.0f};
    bands_[5] = {true, FilterType::Peak, 2500.0f, 0.0f, 1.0f};
    bands_[6] = {true, FilterType::HighShelf, 6000.0f, 0.0f, 0.7f};
    bands_[7] = {true, FilterType::HighCut, 18000.0f, 0.0f, 0.7f};
    
    updateAllCoefficients();
}

void EQ8::reset() {
    for (auto& s : state_) {
        s = BiquadState{};
    }
}

void EQ8::loadPreset(Preset preset) {
    currentPreset_ = preset;
    
    // Reset to flat first
    for (auto& band : bands_) {
        band.gain = 0.0f;
        band.enabled = true;
    }
    
    switch (preset) {
        case Preset::Flat:
            // Already flat
            break;
            
        case Preset::BassBoost:
            bands_[0].type = FilterType::LowCut;
            bands_[0].frequency = 25.0f;
            bands_[1].type = FilterType::LowShelf;
            bands_[1].frequency = 80.0f;
            bands_[1].gain = 4.0f;
            bands_[2].type = FilterType::Peak;
            bands_[2].frequency = 120.0f;
            bands_[2].gain = 3.0f;
            bands_[2].q = 1.5f;
            break;
            
        case Preset::Presence:
            bands_[4].frequency = 2000.0f;
            bands_[4].gain = 3.0f;
            bands_[4].q = 1.2f;
            bands_[5].frequency = 4000.0f;
            bands_[5].gain = 2.5f;
            bands_[5].q = 1.0f;
            break;
            
        case Preset::Scooped:
            bands_[1].type = FilterType::LowShelf;
            bands_[1].frequency = 100.0f;
            bands_[1].gain = 4.0f;
            bands_[3].type = FilterType::Peak;
            bands_[3].frequency = 500.0f;
            bands_[3].gain = -5.0f;
            bands_[3].q = 0.8f;
            bands_[4].type = FilterType::Peak;
            bands_[4].frequency = 1000.0f;
            bands_[4].gain = -4.0f;
            bands_[4].q = 0.8f;
            bands_[6].type = FilterType::HighShelf;
            bands_[6].frequency = 4000.0f;
            bands_[6].gain = 4.0f;
            break;
            
        case Preset::Bright:
            bands_[5].frequency = 3000.0f;
            bands_[5].gain = 2.0f;
            bands_[6].type = FilterType::HighShelf;
            bands_[6].frequency = 8000.0f;
            bands_[6].gain = 4.0f;
            bands_[7].type = FilterType::Peak;
            bands_[7].frequency = 12000.0f;
            bands_[7].gain = 3.0f;
            bands_[7].q = 1.0f;
            break;
            
        case Preset::Warm:
            bands_[1].type = FilterType::LowShelf;
            bands_[1].frequency = 150.0f;
            bands_[1].gain = 3.0f;
            bands_[6].type = FilterType::HighShelf;
            bands_[6].frequency = 6000.0f;
            bands_[6].gain = -4.0f;
            bands_[7].type = FilterType::HighCut;
            bands_[7].frequency = 12000.0f;
            break;
            
        case Preset::LoCut:
            bands_[0].type = FilterType::LowCut;
            bands_[0].frequency = 80.0f;
            bands_[0].q = 0.7f;
            bands_[1].type = FilterType::LowCut;
            bands_[1].frequency = 40.0f;
            bands_[1].q = 0.7f;
            break;
            
        case Preset::Custom:
            // Don't change parameters
            break;
    }
    
    updateAllCoefficients();
}

void EQ8::updateCoefficients(int bandIndex) {
    calculateBiquadCoeffs(bandIndex);
}

void EQ8::updateAllCoefficients() {
    for (int i = 0; i < NUM_BANDS; ++i) {
        calculateBiquadCoeffs(i);
    }
}

void EQ8::calculateBiquadCoeffs(int bandIndex) {
    const Band& band = bands_[bandIndex];
    BiquadCoeffs& c = coeffs_[bandIndex];
    
    float omega = 2.0f * M_PI * band.frequency / static_cast<float>(sampleRate_);
    float sinOmega = std::sin(omega);
    float cosOmega = std::cos(omega);
    float alpha = sinOmega / (2.0f * band.q);
    float A = std::pow(10.0f, band.gain / 40.0f);  // sqrt of linear gain
    
    float a0 = 1.0f;
    
    switch (band.type) {
        case FilterType::LowCut: {
            // High-pass filter (2nd order Butterworth-ish)
            a0 = 1.0f + alpha;
            c.b0 = (1.0f + cosOmega) / 2.0f / a0;
            c.b1 = -(1.0f + cosOmega) / a0;
            c.b2 = (1.0f + cosOmega) / 2.0f / a0;
            c.a1 = -2.0f * cosOmega / a0;
            c.a2 = (1.0f - alpha) / a0;
            break;
        }
        case FilterType::LowShelf: {
            float sqrtA = std::sqrt(A);
            float beta = 2.0f * sqrtA * alpha;
            a0 = (A + 1.0f) + (A - 1.0f) * cosOmega + beta;
            c.b0 = A * ((A + 1.0f) - (A - 1.0f) * cosOmega + beta) / a0;
            c.b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosOmega) / a0;
            c.b2 = A * ((A + 1.0f) - (A - 1.0f) * cosOmega - beta) / a0;
            c.a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosOmega) / a0;
            c.a2 = ((A + 1.0f) + (A - 1.0f) * cosOmega - beta) / a0;
            break;
        }
        case FilterType::Peak: {
            a0 = 1.0f + alpha / A;
            c.b0 = (1.0f + alpha * A) / a0;
            c.b1 = -2.0f * cosOmega / a0;
            c.b2 = (1.0f - alpha * A) / a0;
            c.a1 = -2.0f * cosOmega / a0;
            c.a2 = (1.0f - alpha / A) / a0;
            break;
        }
        case FilterType::HighShelf: {
            float sqrtA = std::sqrt(A);
            float beta = 2.0f * sqrtA * alpha;
            a0 = (A + 1.0f) - (A - 1.0f) * cosOmega + beta;
            c.b0 = A * ((A + 1.0f) + (A - 1.0f) * cosOmega + beta) / a0;
            c.b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosOmega) / a0;
            c.b2 = A * ((A + 1.0f) + (A - 1.0f) * cosOmega - beta) / a0;
            c.a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosOmega) / a0;
            c.a2 = ((A + 1.0f) - (A - 1.0f) * cosOmega - beta) / a0;
            break;
        }
        case FilterType::HighCut: {
            // Low-pass filter (2nd order Butterworth-ish)
            a0 = 1.0f + alpha;
            c.b0 = (1.0f - cosOmega) / 2.0f / a0;
            c.b1 = (1.0f - cosOmega) / a0;
            c.b2 = (1.0f - cosOmega) / 2.0f / a0;
            c.a1 = -2.0f * cosOmega / a0;
            c.a2 = (1.0f - alpha) / a0;
            break;
        }
    }
}

float EQ8::processBiquad(int bandIndex, float input, bool isLeft) {
    const BiquadCoeffs& c = coeffs_[bandIndex];
    BiquadState& s = state_[bandIndex];
    
    float& x1 = isLeft ? s.x1L : s.x1R;
    float& x2 = isLeft ? s.x2L : s.x2R;
    float& y1 = isLeft ? s.y1L : s.y1R;
    float& y2 = isLeft ? s.y2L : s.y2R;
    
    // Direct Form II Transposed
    float output = c.b0 * input + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
    
    // Update state
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    
    return output;
}

void EQ8::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    for (size_t i = 0; i < numFrames; ++i) {
        float sampleL = left[i];
        float sampleR = right[i];
        
        // Process through each enabled band
        for (int b = 0; b < NUM_BANDS; ++b) {
            if (bands_[b].enabled) {
                sampleL = processBiquad(b, sampleL, true);
                sampleR = processBiquad(b, sampleR, false);
            }
        }
        
        // Apply output gain
        left[i] = sampleL * outputGain_;
        right[i] = sampleR * outputGain_;
    }
}

} // namespace pan

