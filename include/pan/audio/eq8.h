#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <cmath>
#include <array>

namespace pan {

/**
 * EQ8 - 8-band parametric equalizer (similar to Ableton's EQ Eight)
 * 
 * Features:
 * - 8 fully parametric bands
 * - Multiple filter types per band (Low Cut, Low Shelf, Peak, High Shelf, High Cut)
 * - Biquad IIR filters for each band
 */
class EQ8 : public Effect {
public:
    static constexpr int NUM_BANDS = 8;
    
    enum class FilterType {
        LowCut,     // High-pass filter (removes lows)
        LowShelf,   // Boost/cut below frequency
        Peak,       // Bell/parametric EQ
        HighShelf,  // Boost/cut above frequency
        HighCut     // Low-pass filter (removes highs)
    };
    
    struct Band {
        bool enabled = true;
        FilterType type = FilterType::Peak;
        float frequency = 1000.0f;  // Hz
        float gain = 0.0f;          // dB (-24 to +24)
        float q = 1.0f;             // Q factor (0.1 to 18)
    };
    
    // Presets
    enum class Preset {
        Flat,           // All bands at 0dB
        BassBoost,      // Enhanced low end
        Presence,       // Vocal presence boost
        Scooped,        // Mid scoop for guitars
        Bright,         // High frequency boost
        Warm,           // Roll off highs, boost lows
        LoCut,          // Remove rumble
        Custom
    };
    
    EQ8(double sampleRate);
    virtual ~EQ8() = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "EQ8"; }
    void reset() override;
    
    void loadPreset(Preset preset);
    Preset getCurrentPreset() const { return currentPreset_; }
    void setCurrentPreset(Preset p) { currentPreset_ = p; }
    static const char* getPresetName(Preset preset);
    
    // Band access
    Band& getBand(int index) { return bands_[index]; }
    const Band& getBand(int index) const { return bands_[index]; }
    void updateCoefficients(int bandIndex);
    void updateAllCoefficients();
    
    // Output gain
    void setOutputGain(float gainDb) { outputGain_ = std::pow(10.0f, gainDb / 20.0f); }
    float getOutputGainDb() const { return 20.0f * std::log10(outputGain_); }

private:
    double sampleRate_;
    Preset currentPreset_ = Preset::Flat;
    float outputGain_ = 1.0f;
    
    std::array<Band, NUM_BANDS> bands_;
    
    // Biquad filter coefficients for each band
    struct BiquadCoeffs {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
        float a1 = 0.0f, a2 = 0.0f;
    };
    std::array<BiquadCoeffs, NUM_BANDS> coeffs_;
    
    // Filter state (stereo)
    struct BiquadState {
        float x1L = 0.0f, x2L = 0.0f;  // Input history
        float y1L = 0.0f, y2L = 0.0f;  // Output history
        float x1R = 0.0f, x2R = 0.0f;
        float y1R = 0.0f, y2R = 0.0f;
    };
    std::array<BiquadState, NUM_BANDS> state_;
    
    void calculateBiquadCoeffs(int bandIndex);
    float processBiquad(int bandIndex, float input, bool isLeft);
};

} // namespace pan

