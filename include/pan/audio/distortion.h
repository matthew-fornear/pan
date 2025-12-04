#pragma once

#include "pan/audio/effect.h"
#include "pan/audio/audio_buffer.h"
#include <cmath>

namespace pan {

/**
 * Distortion effect - adds harmonic content through non-linear waveshaping
 * 
 * Algorithm:
 * 1. Apply input gain (drive)
 * 2. Apply waveshaping function (soft clip using tanh, or hard clip)
 * 3. Apply tone control (simple low-pass filter)
 * 4. Mix with dry signal
 */
class Distortion : public Effect {
public:
    enum class Type {
        SoftClip,    // tanh - warm, tube-like
        HardClip,    // digital clipping - harsh, aggressive
        Overdrive,   // asymmetric soft clip - amp-like
        Fuzz         // extreme, square-wave-ish
    };
    
    // Presets
    enum class Preset {
        Warm,        // Light tube saturation
        Crunch,      // Medium overdrive
        Heavy,       // High gain distortion
        Fuzz_Preset, // Fuzzy, vintage
        Screamer,    // TS808-style
        Custom
    };
    
    Distortion(double sampleRate);
    virtual ~Distortion() = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Distortion"; }
    void reset() override;
    
    void loadPreset(Preset preset);
    Preset getCurrentPreset() const { return currentPreset_; }
    void setCurrentPreset(Preset p) { currentPreset_ = p; }
    static const char* getPresetName(Preset preset);
    
    // Parameters
    void setDrive(float drive) { drive_ = std::max(1.0f, std::min(100.0f, drive)); }  // 1-100
    void setTone(float tone) { tone_ = std::max(0.0f, std::min(1.0f, tone)); }        // 0-1 (dark to bright)
    void setMix(float mix) { mix_ = std::max(0.0f, std::min(1.0f, mix)); }            // 0-1
    void setType(Type type) { type_ = type; }
    
    float getDrive() const { return drive_; }
    float getTone() const { return tone_; }
    float getMix() const { return mix_; }
    Type getType() const { return type_; }

private:
    double sampleRate_;
    
    // Parameters
    float drive_ = 10.0f;    // Input gain
    float tone_ = 0.5f;      // Tone control (low-pass cutoff)
    float mix_ = 0.7f;       // Wet/dry mix
    Type type_ = Type::SoftClip;
    Preset currentPreset_ = Preset::Warm;
    
    // Simple one-pole low-pass filter state
    float filterStateL_ = 0.0f;
    float filterStateR_ = 0.0f;
    
    // Apply waveshaping based on type
    float waveshape(float input);
};

} // namespace pan

