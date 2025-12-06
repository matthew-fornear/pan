#include "pan/audio/bit_noise_texture.h"
#include <algorithm>

namespace pan {

BitNoiseTexture::BitNoiseTexture(double sampleRate)
    : sampleRate_(sampleRate)
{
    std::random_device rd;
    rng_.seed(rd());
}

void BitNoiseTexture::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    float step = 1.0f / ((1 << bits_) - 1);
    float tiltCoeff = 1.0f - std::exp(-2.0f * static_cast<float>(M_PI) * (tilt_ > 0 ? 8000.0f : 1200.0f) / static_cast<float>(sampleRate_));
    
    for (size_t i = 0; i < numFrames; ++i) {
        // Downsample hold
        if (phase_ % static_cast<size_t>(downsampleFactor_) == 0) {
            heldL_ = std::round(left[i] / step) * step;
            heldR_ = std::round(right[i] / step) * step;
        }
        phase_++;
        
        float nl = noise_ * dist_(rng_);
        float nr = noise_ * dist_(rng_);
        
        float wetL = heldL_ + nl;
        float wetR = heldR_ + nr;
        
        // Simple tilt (one-pole)
        tiltStateL_ += tiltCoeff * (wetL - tiltStateL_);
        tiltStateR_ += tiltCoeff * (wetR - tiltStateR_);
        wetL = tilt_ < 0 ? tiltStateL_ : wetL - tiltStateL_;
        wetR = tilt_ < 0 ? tiltStateR_ : wetR - tiltStateR_;
        
        left[i] = left[i] * (1.0f - mix_) + wetL * mix_;
        right[i] = right[i] * (1.0f - mix_) + wetR * mix_;
    }
}

} // namespace pan


