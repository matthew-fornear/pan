#include "pan/audio/resonator_bank.h"
#include <algorithm>

namespace pan {

ResonatorBank::ResonatorBank(double sampleRate)
    : sampleRate_(sampleRate)
{
    recalcDelays();
}

void ResonatorBank::reset() {
    for (auto& c : combs_) {
        std::fill(c.buf.begin(), c.buf.end(), 0.0f);
        c.writePos = 0;
    }
}

void ResonatorBank::recalcDelays() {
    float ratios[3] = {1.0f, std::pow(2.0f, spreadSemi_ / 12.0f), std::pow(2.0f, -spreadSemi_ / 24.0f)};
    for (int i = 0; i < 3; ++i) {
        float freq = rootHz_ * ratios[i];
        float minDelay = 1.0f;
        float desired = static_cast<float>(sampleRate_ / freq);
        size_t delay = static_cast<size_t>(std::max(minDelay, desired));
        combs_[i].delay = delay;
        combs_[i].buf.assign(delay + 1, 0.0f);
        combs_[i].writePos = 0;
    }
}

void ResonatorBank::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_) return;
    auto& left = buffer.getChannel(0);
    auto& right = buffer.getChannel(1);
    
    for (size_t i = 0; i < numFrames; ++i) {
        float inputL = left[i];
        float inputR = right[i];
        
        float wetL = 0.0f;
        float wetR = 0.0f;
        
        for (int c = 0; c < 3; ++c) {
            auto& comb = combs_[c];
            size_t rp = (comb.writePos + comb.buf.size() - comb.delay) % comb.buf.size();
            float delayed = comb.buf[rp];
            float out = inputL * 0.5f + delayed * decay_;
            comb.buf[comb.writePos] = out;
            comb.writePos = (comb.writePos + 1) % comb.buf.size();
            wetL += out;
            wetR += out;
        }
        
        wetL /= 3.0f;
        wetR /= 3.0f;
        left[i] = left[i] * (1.0f - mix_) + wetL * mix_;
        right[i] = right[i] * (1.0f - mix_) + wetR * mix_;
    }
}

} // namespace pan


