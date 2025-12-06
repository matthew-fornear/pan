#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <vector>
#include <cmath>
#include <algorithm>

namespace pan {

class WowFlutter : public Effect {
public:
    explicit WowFlutter(double sampleRate);
    ~WowFlutter() override = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Wow/Flutter Tape"; }
    void reset() override;
    
    void setWowRate(float r) { wowRate_ = std::clamp(r, 0.05f, 2.0f); }
    void setWowDepthMs(float d) { wowDepthMs_ = std::clamp(d, 0.1f, 6.0f); }
    void setFlutterRate(float r) { flutterRate_ = std::clamp(r, 3.0f, 12.0f); }
    void setFlutterDepthMs(float d) { flutterDepthMs_ = std::clamp(d, 0.05f, 1.5f); }
    void setSaturation(float s) { saturation_ = std::clamp(s, 0.0f, 1.0f); }
    void setMix(float m) { mix_ = std::clamp(m, 0.0f, 1.0f); }
    
    float getWowRate() const { return wowRate_; }
    float getWowDepthMs() const { return wowDepthMs_; }
    float getFlutterRate() const { return flutterRate_; }
    float getFlutterDepthMs() const { return flutterDepthMs_; }
    float getSaturation() const { return saturation_; }
    float getMix() const { return mix_; }
    
private:
    double sampleRate_;
    size_t maxDelaySamples_;
    std::vector<float> delayL_;
    std::vector<float> delayR_;
    size_t writePos_ = 0;
    double wowPhase_ = 0.0;
    double flutterPhase_ = 0.0;
    
    float wowRate_ = 0.3f;
    float wowDepthMs_ = 3.0f;
    float flutterRate_ = 7.0f;
    float flutterDepthMs_ = 0.4f;
    float saturation_ = 0.2f;
    float mix_ = 0.5f;
    
    float readDelayInterp(const std::vector<float>& buf, float delaySamples) const;
};

} // namespace pan


