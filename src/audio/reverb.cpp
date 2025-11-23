#include "pan/audio/reverb.h"
#include "pan/audio/audio_buffer.h"
#include <cmath>
#include <algorithm>

namespace pan {

// Tuning constants (in samples at 44.1kHz)
static const int COMB_TUNING_L1 = 1116;
static const int COMB_TUNING_L2 = 1188;
static const int COMB_TUNING_L3 = 1277;
static const int COMB_TUNING_L4 = 1356;
static const int COMB_TUNING_L5 = 1422;
static const int COMB_TUNING_L6 = 1491;
static const int COMB_TUNING_L7 = 1557;
static const int COMB_TUNING_L8 = 1617;

static const int COMB_TUNING_R1 = 1116 + 23;
static const int COMB_TUNING_R2 = 1188 + 23;
static const int COMB_TUNING_R3 = 1277 + 23;
static const int COMB_TUNING_R4 = 1356 + 23;
static const int COMB_TUNING_R5 = 1422 + 23;
static const int COMB_TUNING_R6 = 1491 + 23;
static const int COMB_TUNING_R7 = 1557 + 23;
static const int COMB_TUNING_R8 = 1617 + 23;

static const int ALLPASS_TUNING_L1 = 556;
static const int ALLPASS_TUNING_L2 = 441;
static const int ALLPASS_TUNING_L3 = 341;
static const int ALLPASS_TUNING_L4 = 225;

static const int ALLPASS_TUNING_R1 = 556 + 23;
static const int ALLPASS_TUNING_R2 = 441 + 23;
static const int ALLPASS_TUNING_R3 = 341 + 23;
static const int ALLPASS_TUNING_R4 = 225 + 23;

Reverb::CombFilter::CombFilter(size_t size)
    : bufferSize(size)
    , bufferIndex(0)
    , feedback(0)
    , filterStore(0)
    , damp1(0)
    , damp2(0)
{
    buffer.resize(size, 0.0f);
}

float Reverb::CombFilter::process(float input) {
    float output = buffer[bufferIndex];
    filterStore = (output * damp2) + (filterStore * damp1);
    buffer[bufferIndex] = input + (filterStore * feedback);
    
    bufferIndex = (bufferIndex + 1) % bufferSize;
    return output;
}

Reverb::AllpassFilter::AllpassFilter(size_t size)
    : bufferSize(size)
    , bufferIndex(0)
{
    buffer.resize(size, 0.0f);
}

float Reverb::AllpassFilter::process(float input) {
    float bufout = buffer[bufferIndex];
    float output = -input + bufout;
    buffer[bufferIndex] = input + (bufout * 0.5f);
    
    bufferIndex = (bufferIndex + 1) % bufferSize;
    return output;
}

Reverb::Reverb(double sampleRate)
    : sampleRate_(sampleRate)
    , roomSize_(0.5f)
    , damping_(0.5f)
    , wetLevel_(0.3f)
    , dryLevel_(0.7f)
    , width_(1.0f)
    , currentPreset_(Preset::Room)
{
    // Scale buffer sizes based on sample rate
    float scale = sampleRate_ / 44100.0f;
    
    // Create comb filters
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L1 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L2 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L3 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L4 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L5 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L6 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L7 * scale));
    combFiltersL_.emplace_back(static_cast<size_t>(COMB_TUNING_L8 * scale));
    
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R1 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R2 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R3 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R4 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R5 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R6 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R7 * scale));
    combFiltersR_.emplace_back(static_cast<size_t>(COMB_TUNING_R8 * scale));
    
    // Create allpass filters
    allpassFiltersL_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_L1 * scale));
    allpassFiltersL_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_L2 * scale));
    allpassFiltersL_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_L3 * scale));
    allpassFiltersL_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_L4 * scale));
    
    allpassFiltersR_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_R1 * scale));
    allpassFiltersR_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_R2 * scale));
    allpassFiltersR_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_R3 * scale));
    allpassFiltersR_.emplace_back(static_cast<size_t>(ALLPASS_TUNING_R4 * scale));
    
    updateFilters();
}

void Reverb::process(AudioBuffer& buffer, size_t numFrames) {
    if (!enabled_ || buffer.getNumChannels() == 0) {
        return;
    }
    
    float* leftChannel = buffer.getWritePointer(0);
    float* rightChannel = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : leftChannel;
    
    for (size_t i = 0; i < numFrames; ++i) {
        float inputL = leftChannel[i];
        float inputR = rightChannel[i];
        
        // Mix to mono for reverb input
        float input = (inputL + inputR) * 0.5f;
        
        // Process through comb filters
        float outputL = 0.0f;
        float outputR = 0.0f;
        
        for (auto& filter : combFiltersL_) {
            outputL += filter.process(input);
        }
        for (auto& filter : combFiltersR_) {
            outputR += filter.process(input);
        }
        
        // Process through allpass filters
        for (auto& filter : allpassFiltersL_) {
            outputL = filter.process(outputL);
        }
        for (auto& filter : allpassFiltersR_) {
            outputR = filter.process(outputR);
        }
        
        // Apply wet/dry mix and width
        float wet1 = wetLevel_ * (width_ * 0.5f + 0.5f);
        float wet2 = wetLevel_ * ((1.0f - width_) * 0.5f);
        
        // Scale reverb output to prevent buildup
        outputL *= 0.015f;
        outputR *= 0.015f;
        
        // Mix with proper gain compensation to prevent clipping
        float outL = inputL * dryLevel_ + outputL * wet1 + outputR * wet2;
        float outR = inputR * dryLevel_ + outputR * wet1 + outputL * wet2;
        
        // Soft clipping to prevent harsh distortion
        leftChannel[i] = std::tanh(outL);
        rightChannel[i] = std::tanh(outR);
    }
}

void Reverb::reset() {
    for (auto& filter : combFiltersL_) {
        std::fill(filter.buffer.begin(), filter.buffer.end(), 0.0f);
        filter.bufferIndex = 0;
        filter.filterStore = 0.0f;
    }
    for (auto& filter : combFiltersR_) {
        std::fill(filter.buffer.begin(), filter.buffer.end(), 0.0f);
        filter.bufferIndex = 0;
        filter.filterStore = 0.0f;
    }
    for (auto& filter : allpassFiltersL_) {
        std::fill(filter.buffer.begin(), filter.buffer.end(), 0.0f);
        filter.bufferIndex = 0;
    }
    for (auto& filter : allpassFiltersR_) {
        std::fill(filter.buffer.begin(), filter.buffer.end(), 0.0f);
        filter.bufferIndex = 0;
    }
}

void Reverb::setRoomSize(float size) {
    roomSize_ = std::clamp(size, 0.0f, 1.0f);
    updateFilters();
    // Note: Don't automatically set to Custom here - let the GUI handle it
}

void Reverb::setDamping(float damping) {
    damping_ = std::clamp(damping, 0.0f, 1.0f);
    updateFilters();
}

void Reverb::setWetLevel(float wet) {
    wetLevel_ = std::clamp(wet, 0.0f, 1.0f);
}

void Reverb::setDryLevel(float dry) {
    dryLevel_ = std::clamp(dry, 0.0f, 1.0f);
}

void Reverb::setWidth(float width) {
    width_ = std::clamp(width, 0.0f, 1.0f);
}

void Reverb::updateFilters() {
    float roomSize = roomSize_ * 0.28f + 0.7f;
    float damp = damping_ * 0.4f;
    
    for (auto& filter : combFiltersL_) {
        filter.feedback = roomSize;
        filter.damp1 = damp;
        filter.damp2 = 1.0f - damp;
    }
    for (auto& filter : combFiltersR_) {
        filter.feedback = roomSize;
        filter.damp1 = damp;
        filter.damp2 = 1.0f - damp;
    }
}

void Reverb::loadPreset(Preset preset) {
    currentPreset_ = preset;
    
    switch (preset) {
        case Preset::Room:
            setRoomSize(0.3f);
            setDamping(0.5f);
            setWetLevel(0.25f);
            setDryLevel(0.75f);
            setWidth(0.7f);
            break;
            
        case Preset::Hall:
            setRoomSize(0.7f);
            setDamping(0.3f);
            setWetLevel(0.4f);
            setDryLevel(0.6f);
            setWidth(1.0f);
            break;
            
        case Preset::Plate:
            setRoomSize(0.4f);
            setDamping(0.7f);
            setWetLevel(0.35f);
            setDryLevel(0.65f);
            setWidth(0.5f);
            break;
            
        case Preset::Chamber:
            setRoomSize(0.5f);
            setDamping(0.4f);
            setWetLevel(0.3f);
            setDryLevel(0.7f);
            setWidth(0.8f);
            break;
            
        case Preset::Cathedral:
            setRoomSize(0.9f);
            setDamping(0.2f);
            setWetLevel(0.5f);
            setDryLevel(0.5f);
            setWidth(1.0f);
            break;
            
        case Preset::Spring:
            setRoomSize(0.2f);
            setDamping(0.8f);
            setWetLevel(0.4f);
            setDryLevel(0.6f);
            setWidth(0.3f);
            break;
            
        case Preset::Custom:
            // Don't change anything for custom
            break;
    }
}

const char* Reverb::getPresetName(Preset preset) {
    switch (preset) {
        case Preset::Room: return "Room";
        case Preset::Hall: return "Hall";
        case Preset::Plate: return "Plate";
        case Preset::Chamber: return "Chamber";
        case Preset::Cathedral: return "Cathedral";
        case Preset::Spring: return "Spring";
        case Preset::Custom: return "Custom";
        default: return "Unknown";
    }
}

} // namespace pan

