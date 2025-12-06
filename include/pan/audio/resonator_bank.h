#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace pan {

class ResonatorBank : public Effect {
public:
    explicit ResonatorBank(double sampleRate);
    ~ResonatorBank() override = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Resonator Bank"; }
    void reset() override;
    
    void setRootHz(float hz) { rootHz_ = std::clamp(hz, 40.0f, 2000.0f); recalcDelays(); }
    void setSpread(float s) { spreadSemi_ = std::clamp(s, -12.0f, 24.0f); recalcDelays(); }
    void setDecay(float d) { decay_ = std::clamp(d, 0.1f, 0.999f); }
    void setMix(float m) { mix_ = std::clamp(m, 0.0f, 1.0f); }
    
    float getRootHz() const { return rootHz_; }
    float getSpread() const { return spreadSemi_; }
    float getDecay() const { return decay_; }
    float getMix() const { return mix_; }
    
private:
    struct Comb {
        std::vector<float> buf;
        size_t writePos = 0;
        size_t delay = 1;
    };
    
    double sampleRate_;
    float rootHz_ = 220.0f;
    float spreadSemi_ = 7.0f;
    float decay_ = 0.85f;
    float mix_ = 0.5f;
    
    Comb combs_[3];
    
    void recalcDelays();
};

} // namespace pan


