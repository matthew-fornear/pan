#pragma once

#include <vector>
#include <map>
#include <set>
#include <memory>
#include <cstdint>
#include <mutex>
#include <atomic>
#include "pan/audio/audio_buffer.h"
#include "pan/midi/midi_message.h"

namespace pan {

/**
 * Waveform types
 */
enum class Waveform {
    Sine,
    Square,
    Sawtooth,
    Triangle
};

/**
 * Oscillator configuration for multi-oscillator synthesis
 */
struct Oscillator {
    Waveform waveform;
    float frequencyMultiplier;  // Relative to base note frequency (1.0 = same, 2.0 = octave up, 0.5 = octave down)
    float amplitude;            // 0.0 to 1.0
    
    Oscillator() : waveform(Waveform::Sine), frequencyMultiplier(1.0f), amplitude(1.0f) {}
    Oscillator(Waveform wf, float freqMult, float amp) 
        : waveform(wf), frequencyMultiplier(freqMult), amplitude(amp) {}
};

/**
 * Simple synthesizer for playing MIDI notes
 */
class Synthesizer {
public:
    Synthesizer(double sampleRate);
    ~Synthesizer();

    // MIDI input
    void processMidiMessage(const MidiMessage& message);
    void processMidiMessages(const std::vector<MidiMessage>& messages);
    
    // Audio generation
    void generateAudio(AudioBuffer& buffer, size_t numFrames);
    
    // Voice management
    void noteOn(uint8_t note, uint8_t velocity);
    void noteOff(uint8_t note);
    void allNotesOff();
    
    // Synthesis parameters
    void setVolume(float volume);
    float getVolume() const { return volume_; }
    void setWaveform(Waveform waveform);  // Deprecated: use oscillators instead
    Waveform getWaveform() const;  // Deprecated: returns first oscillator's waveform
    void setReleaseTime(float seconds) { releaseTime_ = std::max(0.001f, seconds); }
    float getReleaseTime() const { return releaseTime_; }
    
    // Oscillator management
    void setOscillators(const std::vector<Oscillator>& oscillators) { oscillators_ = oscillators; }
    const std::vector<Oscillator>& getOscillators() const { return oscillators_; }
    std::vector<Oscillator>& getOscillators() { return oscillators_; }

private:
    struct Voice {
        uint8_t note;
        float phase;
        float phaseIncrement;
        float amplitude;
        float envelope;
        bool active;
        bool inRelease;  // True when note is released and fading out
        float releaseStartEnvelope;  // Envelope value when release started
        int releaseSamplesRemaining;  // Samples remaining in release phase
        
        Voice() : note(0), phase(0.0f), phaseIncrement(0.0f), 
                  amplitude(0.0f), envelope(0.0f), active(false),
                  inRelease(false), releaseStartEnvelope(0.0f), releaseSamplesRemaining(0) {}
    };
    
    double sampleRate_;
    float volume_;
    Waveform waveform_;  // Deprecated: kept for backward compatibility
    std::vector<Oscillator> oscillators_;  // Multi-oscillator support
    std::vector<Voice> voices_;
    static constexpr size_t MAX_VOICES = 16;
    
    // Envelope parameters (in seconds)
    float releaseTime_;  // Release phase duration (default 0.025s = 25ms)
    
    // Sustain pedal state
    bool sustainPedalDown_;
    std::set<uint8_t> sustainedNotes_;  // Notes that are released but held by sustain pedal
    
    // Thread safety
    std::mutex midiMutex_;
    std::vector<MidiMessage> pendingMessages_;
    std::atomic<bool> hasPendingMessages_;
    
    float noteToFrequency(uint8_t note) const;
    void updateVoice(Voice& voice, size_t numFrames);
    void handleSustainPedal(uint8_t value);
    float generateWaveform(float phase, Waveform waveform) const;
};

} // namespace pan

