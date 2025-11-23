#pragma once

#include "effect.h"
#include <vector>

namespace pan {

/**
 * Simple reverb effect (Freeverb-style algorithm)
 */
class Reverb : public Effect {
public:
    Reverb(double sampleRate);
    virtual ~Reverb() = default;
    
    void process(AudioBuffer& buffer, size_t numFrames) override;
    std::string getName() const override { return "Reverb"; }
    void reset() override;
    
    // Preset types
    enum class Preset {
        Room,
        Hall,
        Plate,
        Chamber,
        Cathedral,
        Spring,
        Custom
    };
    
    // Parameters
    void setRoomSize(float size);     // 0.0 - 1.0
    void setDamping(float damping);   // 0.0 - 1.0
    void setWetLevel(float wet);      // 0.0 - 1.0
    void setDryLevel(float dry);      // 0.0 - 1.0
    void setWidth(float width);       // 0.0 - 1.0
    
    float getRoomSize() const { return roomSize_; }
    float getDamping() const { return damping_; }
    float getWetLevel() const { return wetLevel_; }
    float getDryLevel() const { return dryLevel_; }
    float getWidth() const { return width_; }
    
    // Presets
    void loadPreset(Preset preset);
    Preset getCurrentPreset() const { return currentPreset_; }
    void setCurrentPreset(Preset preset) { currentPreset_ = preset; }
    static const char* getPresetName(Preset preset);
    
private:
    // Simple comb filter for reverb
    struct CombFilter {
        std::vector<float> buffer;
        size_t bufferSize;
        size_t bufferIndex;
        float feedback;
        float filterStore;
        float damp1;
        float damp2;
        
        CombFilter(size_t size);
        float process(float input);
    };
    
    // Simple allpass filter
    struct AllpassFilter {
        std::vector<float> buffer;
        size_t bufferSize;
        size_t bufferIndex;
        
        AllpassFilter(size_t size);
        float process(float input);
    };
    
    double sampleRate_;
    float roomSize_;
    float damping_;
    float wetLevel_;
    float dryLevel_;
    float width_;
    Preset currentPreset_;
    
    // Filters for stereo reverb
    std::vector<CombFilter> combFiltersL_;
    std::vector<CombFilter> combFiltersR_;
    std::vector<AllpassFilter> allpassFiltersL_;
    std::vector<AllpassFilter> allpassFiltersR_;
    
    void updateFilters();
};

} // namespace pan

