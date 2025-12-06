#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <cmath>
#include <algorithm>

namespace pan {

// Tempo-free pump/ducking envelope (can be rate-driven)
class SidechainPump : public Effect {
public:
    explicit SidechainPump(double sampleRate);
    ~SidechainPump() override = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Sidechain Pump"; }
    void reset() override { phase_ = 0.0; env_ = 0.0f; }
    
    // Parameters
    void setRateHz(float r) { rateHz_ = std::clamp(r, 0.1f, 8.0f); }
    void setDepth(float dB) { depthDb_ = std::clamp(dB, -48.0f, 0.0f); }  // negative gain applied at peak
    void setMix(float m) { mix_ = std::clamp(m, 0.0f, 1.0f); }
    void setShape(float s) { shape_ = std::clamp(s, 0.2f, 3.0f); } // curve steepness
    void setAttackMs(float a) { attackMs_ = std::clamp(a, 1.0f, 400.0f); }
    void setReleaseMs(float r) { releaseMs_ = std::clamp(r, 10.0f, 800.0f); }
    
    float getRateHz() const { return rateHz_; }
    float getDepthDb() const { return depthDb_; }
    float getMix() const { return mix_; }
    float getShape() const { return shape_; }
    float getAttackMs() const { return attackMs_; }
    float getReleaseMs() const { return releaseMs_; }
    
private:
    double sampleRate_;
    double phase_ = 0.0;
    float env_ = 0.0f;
    
    // Parameters
    float rateHz_ = 2.0f;
    float depthDb_ = -12.0f;
    float mix_ = 0.6f;
    float shape_ = 1.5f;
    float attackMs_ = 10.0f;
    float releaseMs_ = 200.0f;
};

} // namespace pan


