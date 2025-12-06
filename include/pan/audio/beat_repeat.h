#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <vector>
#include <random>
#include <algorithm>

namespace pan {

class BeatRepeat : public Effect {
public:
    explicit BeatRepeat(double sampleRate);
    ~BeatRepeat() override = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Beat Repeat"; }
    void reset() override;
    
    void setIntervalMs(float ms) { intervalMs_ = std::clamp(ms, 50.0f, 2000.0f); }
    void setGateMs(float ms) { gateMs_ = std::clamp(ms, 40.0f, 800.0f); }
    void setChance(float c) { chance_ = std::clamp(c, 0.0f, 1.0f); }
    void setDecay(float d) { decay_ = std::clamp(d, 0.1f, 1.0f); }
    void setFilter(float f) { filter_ = std::clamp(f, 0.0f, 1.0f); }
    void setMix(float m) { mix_ = std::clamp(m, 0.0f, 1.0f); }
    
    float getIntervalMs() const { return intervalMs_; }
    float getGateMs() const { return gateMs_; }
    float getChance() const { return chance_; }
    float getDecay() const { return decay_; }
    float getFilter() const { return filter_; }
    float getMix() const { return mix_; }
    
private:
    double sampleRate_;
    std::vector<float> bufL_;
    std::vector<float> bufR_;
    size_t writePos_ = 0;
    size_t bufSize_ = 0;
    size_t playPos_ = 0;
    size_t gateSamples_ = 0;
    size_t intervalSamples_ = 0;
    size_t intervalCounter_ = 0;
    bool repeating_ = false;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_{0.0f, 1.0f};
    
    // Params
    float intervalMs_ = 500.0f;
    float gateMs_ = 250.0f;
    float chance_ = 0.35f;
    float decay_ = 0.9f;
    float filter_ = 0.0f; // lowpass mix
    float mix_ = 0.5f;
    
    float lpStateL_ = 0.0f;
    float lpStateR_ = 0.0f;
};

} // namespace pan


