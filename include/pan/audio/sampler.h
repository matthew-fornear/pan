#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <mutex>
#include <array>

namespace pan {

/**
 * Sample - holds audio data loaded from a WAV file
 */
struct Sample {
    std::vector<float> dataL;        // Left channel (or mono)
    std::vector<float> dataR;        // Right channel (empty if mono)
    double sampleRate = 44100.0;
    int rootNote = 60;               // MIDI note at which sample plays at original pitch (C4)
    std::string name;
    std::string filePath;
    bool stereo = false;
    
    // For waveform display (downsampled)
    std::vector<float> waveformDisplay;  // Normalized -1 to 1, ~512 points
    
    void generateWaveformDisplay();
};

/**
 * Sampler playback modes (like Ableton's Simpler)
 */
enum class SamplerMode {
    Classic,   // Standard sampler mode with ADSR
    OneShot,   // Plays entire sample, ignores note-off
    Slice      // Sample divided into slices triggered by notes
};

/**
 * Sampler parameters - all the knobs and controls
 */
struct SamplerParams {
    // Mode
    SamplerMode mode = SamplerMode::Classic;
    // Slice data (normalized positions 0-1, excluding 0 and 1)
    std::vector<float> sliceMarkers; 
    int sliceGridSlices = 4;       // default slices when not custom
    bool sliceCustom = false;
    
    // Sample region
    float gain = 0.0f;           // dB (-60 to +12)
    float startPos = 0.0f;       // Start position (0-1 normalized)
    float loopStart = 0.0f;      // Loop start position (0-1)
    float length = 1.0f;         // Sample length (0-1, 1 = full sample)
    float fade = 0.0f;           // Fade in/out amount (0-1)
    bool loopEnabled = false;    // Loop playback
    bool snapEnabled = false;    // Snap to zero crossings
    
    // Voices
    int voices = 6;              // Max polyphony (1-32)
    bool retrigger = false;      // Retrigger on same note
    
    // Warp/Beats
    bool warpEnabled = false;    // Enable time-warping
    float warpBeats = 1.0f;      // Number of beats the sample represents
    
    // Filter
    bool filterEnabled = false;
    int filterType = 0;          // 0=LP, 1=HP, 2=BP, 3=Notch
    float filterFreq = 22000.0f; // Hz (20-22000)
    float filterRes = 0.0f;      // Resonance (0-1)
    
    // LFO
    bool lfoEnabled = false;
    int lfoWaveform = 0;         // 0=Sine, 1=Triangle, 2=Saw, 3=Square, 4=Random
    float lfoRate = 1.0f;        // Hz
    float lfoAmount = 0.0f;      // Depth (0-1)
    int lfoTarget = 0;           // 0=Pitch, 1=Filter, 2=Pan, 3=Volume
    
    // Pitch
    int transpose = 0;           // Semitones (-48 to +48)
    float detune = 0.0f;         // Cents (-100 to +100)
    bool pitchEnvEnabled = false;
    float pitchEnvAmount = 0.0f; // Semitones
    float pitchEnvTime = 0.0f;   // Seconds
    
    // ADSR Amplitude Envelope
    float attack = 0.0f;         // Seconds (0-30)
    float decay = 0.0f;          // Seconds (0-30)
    float sustain = 1.0f;        // Level (0-1)
    float release = 0.05f;       // Seconds (0-30)
    
    // Pan/Spread
    float pan = 0.0f;            // -1 (L) to +1 (R)
    float spread = 0.0f;         // Stereo spread (0-1)
    
    // Volume output
    float volume = -12.0f;       // dB (-inf to 0)
};

/**
 * Sampler - plays back audio samples, pitch-shifted based on MIDI note
 */
class Sampler {
public:
    Sampler(double sampleRate);
    ~Sampler() = default;
    
    // Load a sample from WAV file
    bool loadSample(const std::string& path);
    
    // Get loaded sample (for display)
    const Sample* getSample() const { return sample_.get(); }
    
    // Process audio (called from audio thread)
    void process(float* outL, float* outR, size_t numFrames);
    
    // MIDI control
    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void allNotesOff();
    
    // Parameters access
    SamplerParams& getParams() { return params_; }
    const SamplerParams& getParams() const { return params_; }
    
    // Convenience setters for common parameters
    void setVolume(float vol) { params_.volume = vol; }
    float getVolume() const { return params_.volume; }
    void setRootNote(int note) { if (sample_) sample_->rootNote = note; }
    int getRootNote() const { return sample_ ? sample_->rootNote : 60; }
    void setMode(SamplerMode mode) { params_.mode = mode; }
    SamplerMode getMode() const { return params_.mode; }
    
    // Sample metadata
    double getSampleDuration() const;   // seconds
    size_t getSampleFrames() const;     // total frames
    double getSampleRate() const;       // Hz

private:
    double sampleRate_;
    std::unique_ptr<Sample> sample_;
    SamplerParams params_;
    
    // Multi-voice support
    static constexpr int MAX_VOICES = 32;
    struct Voice {
        bool active = false;
        double position = 0.0;       // Current playback position in samples
        double increment = 1.0;      // Playback speed (for pitch shifting)
        float velocity = 1.0f;
        uint8_t note = 0;
        size_t startSample = 0;      // slice start
        size_t endSample = 0;        // slice end (exclusive)
        size_t loopStartSample = 0;  // loop start within slice
        
        // ADSR envelope state
        enum class EnvStage { Attack, Decay, Sustain, Release, Off };
        EnvStage envStage = EnvStage::Off;
        float envLevel = 0.0f;       // Current envelope level
        double envTime = 0.0;        // Time in current stage
        
        // For one-shot mode
        bool releasing = false;
    };
    std::array<Voice, MAX_VOICES> voices_;
    int activeVoiceCount_ = 0;
    
    // LFO state
    double lfoPhase_ = 0.0;
    
    // Filter state (biquad)
    float filterState_[4] = {0, 0, 0, 0};  // z1L, z2L, z1R, z2R
    
    std::mutex mutex_;
    
    // Internal helpers
    void processVoice(Voice& voice, float* outL, float* outR, size_t numFrames);
    float processEnvelope(Voice& voice, double deltaTime);
    float calculateLFO();
    void updateFilter();
    int findFreeVoice();
    
    // File loading helpers
    bool parseWavHeader(const std::vector<uint8_t>& data, int& channels, int& sampleRate, int& bitsPerSample, size_t& dataOffset, size_t& dataSize);
    bool loadMp3(const std::string& path);
};

} // namespace pan

