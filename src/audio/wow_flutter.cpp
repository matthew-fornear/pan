#include "pan/audio/wow_flutter.h"
#include <algorithm>

namespace pan {

WowFlutter::WowFlutter(double sampleRate)
    : sampleRate_(sampleRate)
{
    maxDelaySamples_ = static_cast<size_t>(sampleRate_ * 0.05); // 50ms max
    delayL_.assign(maxDelaySamples_, 0.0f);
    delayR_.assign(maxDelaySamples_, 0.0f);
}

void WowFlutter::reset() {
    std::fill(delayL_.begin(), delayL_.end(), 0.0f);
    std::fill(delayR_.begin(), delayR_.end(), 0.0f);
    writePos_ = 0;
    wowPhase_ = flutterPhase_ = 0.0;
}

float WowFlutter::readDelayInterp(const std::vector<float>& buf, float delaySamples) const {
    float rp = static_cast<float>(writePos_) - delaySamples;
    while (rp < 0) rp += static_cast<float>(maxDelaySamples_);
    size_t i0 = static_cast<size_t>(rp) % maxDelaySamples_;
    size_t i1 = (i0 + 1) % maxDelaySamples_;
    float frac = rp - std::floor(rp);
    return buf[i0] * (1.0f - frac) + buf[i1] * frac;
}

void WowFlutter::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    double wowInc = (2.0 * M_PI * wowRate_) / sampleRate_;
    double flutterInc = (2.0 * M_PI * flutterRate_) / sampleRate_;
    
    for (size_t i = 0; i < numFrames; ++i) {
        float wow = std::sin(wowPhase_);
        float flutter = std::sin(flutterPhase_);
        float delayMs = wowDepthMs_ * wow + flutterDepthMs_ * flutter + (wowDepthMs_ * 0.5f);
        float delaySamples = std::clamp(delayMs / 1000.0f * static_cast<float>(sampleRate_), 1.0f, static_cast<float>(maxDelaySamples_ - 2));
        
        // write dry
        delayL_[writePos_] = left[i];
        delayR_[writePos_] = right[i];
        
        // read delayed
        float dL = readDelayInterp(delayL_, delaySamples);
        float dR = readDelayInterp(delayR_, delaySamples);
        
        // soft saturation
        auto sat = [&](float x) { return std::tanh(x * (1.0f + saturation_ * 4.0f)); };
        dL = dL * (1.0f - saturation_) + sat(dL) * saturation_;
        dR = dR * (1.0f - saturation_) + sat(dR) * saturation_;
        
        left[i] = left[i] * (1.0f - mix_) + dL * mix_;
        right[i] = right[i] * (1.0f - mix_) + dR * mix_;
        
        writePos_ = (writePos_ + 1) % maxDelaySamples_;
        wowPhase_ += wowInc;
        flutterPhase_ += flutterInc;
        if (wowPhase_ >= 2.0 * M_PI) wowPhase_ -= 2.0 * M_PI;
        if (flutterPhase_ >= 2.0 * M_PI) flutterPhase_ -= 2.0 * M_PI;
    }
}

} // namespace pan


