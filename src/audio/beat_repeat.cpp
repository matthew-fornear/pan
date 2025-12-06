#include "pan/audio/beat_repeat.h"
#include <algorithm>

namespace pan {

BeatRepeat::BeatRepeat(double sampleRate)
    : sampleRate_(sampleRate)
{
    bufSize_ = static_cast<size_t>(sampleRate_ * 2.0); // 2 seconds max
    bufL_.assign(bufSize_, 0.0f);
    bufR_.assign(bufSize_, 0.0f);
    std::random_device rd;
    rng_.seed(rd());
    reset();
}

void BeatRepeat::reset() {
    std::fill(bufL_.begin(), bufL_.end(), 0.0f);
    std::fill(bufR_.begin(), bufR_.end(), 0.0f);
    writePos_ = playPos_ = 0;
    repeating_ = false;
    intervalCounter_ = 0;
    gateSamples_ = static_cast<size_t>(gateMs_ / 1000.0f * sampleRate_);
    intervalSamples_ = static_cast<size_t>(intervalMs_ / 1000.0f * sampleRate_);
    lpStateL_ = lpStateR_ = 0.0f;
}

void BeatRepeat::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    if (intervalSamples_ == 0) intervalSamples_ = 1;
    if (gateSamples_ == 0) gateSamples_ = 1;
    float lpCoeff = filter_ > 0 ? (1.0f - std::exp(-2.0f * static_cast<float>(M_PI) * 4000.0f / static_cast<float>(sampleRate_))) * filter_ : 0.0f;
    
    for (size_t i = 0; i < numFrames; ++i) {
        // write incoming to buffer
        bufL_[writePos_] = left[i];
        bufR_[writePos_] = right[i];
        
        float wetL = left[i];
        float wetR = right[i];
        
        if (repeating_) {
            size_t idx = (playPos_ + i) % bufSize_;
            wetL = bufL_[idx];
            wetR = bufR_[idx];
            // decay
            wetL *= decay_;
            wetR *= decay_;
            // simple lowpass
            if (lpCoeff > 0.0f) {
                lpStateL_ += lpCoeff * (wetL - lpStateL_);
                lpStateR_ += lpCoeff * (wetR - lpStateR_);
                wetL = lpStateL_;
                wetR = lpStateR_;
            }
        }
        
        left[i] = left[i] * (1.0f - mix_) + wetL * mix_;
        right[i] = right[i] * (1.0f - mix_) + wetR * mix_;
        
        writePos_ = (writePos_ + 1) % bufSize_;
        intervalCounter_++;
        
        // trigger
        if (intervalCounter_ >= intervalSamples_) {
            intervalCounter_ = 0;
            if (dist_(rng_) <= chance_) {
                repeating_ = true;
                playPos_ = (writePos_ + bufSize_ - gateSamples_) % bufSize_;
            } else {
                repeating_ = false;
            }
        }
    }
}

} // namespace pan


