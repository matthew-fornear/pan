#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <random>
#include <algorithm>

namespace pan {

class BitNoiseTexture : public Effect {
public:
    explicit BitNoiseTexture(double sampleRate);
    ~BitNoiseTexture() override = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Bit/Noise Texture"; }
    void reset() override { phase_ = 0; }
    
    void setBits(int b) { bits_ = std::clamp(b, 4, 16); }
    void setDownsample(int f) { downsampleFactor_ = std::clamp(f, 1, 16); }
    void setNoise(float n) { noise_ = std::clamp(n, 0.0f, 0.5f); }
    void setTilt(float t) { tilt_ = std::clamp(t, -1.0f, 1.0f); }
    void setMix(float m) { mix_ = std::clamp(m, 0.0f, 1.0f); }
    
    int getBits() const { return bits_; }
    int getDownsample() const { return downsampleFactor_; }
    float getNoise() const { return noise_; }
    float getTilt() const { return tilt_; }
    float getMix() const { return mix_; }
    
private:
    double sampleRate_;
    int bits_ = 12;
    int downsampleFactor_ = 2;
    float noise_ = 0.05f;
    float tilt_ = -0.2f; // negative darkens
    float mix_ = 0.5f;
    
    size_t phase_ = 0;
    float heldL_ = 0.0f;
    float heldR_ = 0.0f;
    float tiltStateL_ = 0.0f;
    float tiltStateR_ = 0.0f;
    std::mt19937 rng_;
    std::uniform_real_distribution<float> dist_{-1.0f, 1.0f};
};

} // namespace pan


