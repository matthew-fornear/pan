#include "pan/audio/sidechain_pump.h"
#include <algorithm>

namespace pan {

SidechainPump::SidechainPump(double sampleRate)
    : sampleRate_(sampleRate) {}

void SidechainPump::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    float depthLin = std::pow(10.0f, depthDb_ / 20.0f);
    double inc = (2.0 * M_PI * rateHz_) / sampleRate_;
    float attackCoeff = 1.0f - std::exp(-1.0f / (attackMs_ * 0.001f * sampleRate_));
    float releaseCoeff = 1.0f - std::exp(-1.0f / (releaseMs_ * 0.001f * sampleRate_));
    
    for (size_t i = 0; i < numFrames; ++i) {
        // Envelope is driven by a cosine LFO shaped
        float lfo = 0.5f * (1.0f - std::cos(static_cast<float>(phase_)));
        float shaped = std::pow(lfo, shape_);
        float target = 1.0f - (1.0f - depthLin) * shaped; // 1 -> no duck, depthLin at peak
        
        if (target < env_) {
            env_ = env_ + attackCoeff * (target - env_);
        } else {
            env_ = env_ + releaseCoeff * (target - env_);
        }
        
        float wetL = left[i] * env_;
        float wetR = right[i] * env_;
        left[i] = left[i] * (1.0f - mix_) + wetL * mix_;
        right[i] = right[i] * (1.0f - mix_) + wetR * mix_;
        
        phase_ += inc;
        if (phase_ >= 2.0 * M_PI) phase_ -= 2.0 * M_PI;
    }
}

} // namespace pan


