#pragma once

#include <vector>
#include <string>
#include <memory>
#include <cstdint>
#include <mutex>

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
    
    // Parameters
    void setVolume(float vol) { volume_ = vol; }
    float getVolume() const { return volume_; }
    void setRootNote(int note) { if (sample_) sample_->rootNote = note; }
    int getRootNote() const { return sample_ ? sample_->rootNote : 60; }

private:
    double sampleRate_;
    std::unique_ptr<Sample> sample_;
    float volume_ = 1.0f;
    
    // Voice state (simple single-voice sampler)
    struct Voice {
        bool active = false;
        double position = 0.0;       // Current playback position
        double increment = 1.0;      // Playback speed (for pitch shifting)
        float velocity = 1.0f;
        uint8_t note = 0;
    };
    Voice voice_;
    
    std::mutex mutex_;
    
    // File loading helpers
    bool parseWavHeader(const std::vector<uint8_t>& data, int& channels, int& sampleRate, int& bitsPerSample, size_t& dataOffset, size_t& dataSize);
    bool loadMp3(const std::string& path);
};

} // namespace pan

