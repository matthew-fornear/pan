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
    Triangle,
    Noise  // For atmospheric textures
};

/**
 * ADSR Envelope parameters
 */
struct ADSREnvelope {
    float attack = 0.01f;    // Attack time in seconds (0.001 to 5.0)
    float decay = 0.1f;      // Decay time in seconds (0.001 to 5.0)
    float sustain = 0.7f;    // Sustain level (0.0 to 1.0)
    float release = 0.3f;    // Release time in seconds (0.001 to 10.0)
    
    ADSREnvelope() = default;
    ADSREnvelope(float a, float d, float s, float r) 
        : attack(a), decay(d), sustain(s), release(r) {}
};

/**
 * Pitch envelope for 808-style sounds
 */
struct PitchEnvelope {
    bool enabled = false;
    float startMultiplier = 2.0f;  // Starting pitch multiplier (e.g., 2.0 = octave up)
    float decayTime = 0.05f;       // Time to reach target pitch in seconds
    
    PitchEnvelope() = default;
    PitchEnvelope(float start, float decay) 
        : enabled(true), startMultiplier(start), decayTime(decay) {}
};

/**
 * LFO for modulation (used in atmospheric pads)
 */
struct LFO {
    bool enabled = false;
    float rate = 0.5f;           // LFO rate in Hz
    float depth = 0.0f;          // Modulation depth (0.0 to 1.0)
    float phaseOffset = 0.0f;    // Starting phase (0.0 to 1.0)
    enum class Target { Pitch, Amplitude, Filter } target = Target::Pitch;
    
    LFO() = default;
    LFO(float r, float d, Target t = Target::Pitch) 
        : enabled(true), rate(r), depth(d), target(t) {}
};

/**
 * Oscillator configuration for multi-oscillator synthesis
 */
struct Oscillator {
    Waveform waveform;
    float frequencyMultiplier;  // Relative to base note frequency (1.0 = same, 2.0 = octave up, 0.5 = octave down)
    float amplitude;            // 0.0 to 1.0
    float detune = 0.0f;        // Detune in cents (-100 to +100)
    float pan = 0.0f;           // Stereo pan (-1.0 left, 0 center, +1.0 right)
    
    Oscillator() : waveform(Waveform::Sine), frequencyMultiplier(1.0f), amplitude(1.0f) {}
    Oscillator(Waveform wf, float freqMult, float amp) 
        : waveform(wf), frequencyMultiplier(freqMult), amplitude(amp) {}
    Oscillator(Waveform wf, float freqMult, float amp, float det) 
        : waveform(wf), frequencyMultiplier(freqMult), amplitude(amp), detune(det) {}
};

/**
 * Low-pass filter with resonance
 */
struct FilterSettings {
    bool enabled = false;
    float cutoff = 1.0f;        // Normalized cutoff (0.0 to 1.0, maps to ~20Hz to ~20kHz)
    float resonance = 0.0f;     // Resonance/Q (0.0 to 1.0)
    float envAmount = 0.0f;     // How much filter envelope affects cutoff (-1.0 to 1.0)
    ADSREnvelope envelope;      // Filter envelope
    
    FilterSettings() : envelope(0.001f, 0.3f, 0.0f, 0.1f) {}
};

/**
 * Saturation/Soft clipping for warmth
 */
struct SaturationSettings {
    bool enabled = false;
    float drive = 1.0f;         // Drive amount (1.0 = clean, 5.0 = heavy)
    float mix = 0.5f;           // Wet/dry mix
    
    SaturationSettings() = default;
};

/**
 * Portamento/Glide settings
 */
struct PortamentoSettings {
    bool enabled = false;
    float time = 0.1f;          // Glide time in seconds
    bool legato = true;         // Only glide when notes overlap
    
    PortamentoSettings() = default;
};

/**
 * Unison/Voice doubling
 */
struct UnisonSettings {
    bool enabled = false;
    int voices = 1;             // Number of unison voices (1-8)
    float detune = 0.0f;        // Detune spread in cents (0-100)
    float spread = 0.5f;        // Stereo spread (0.0 = mono, 1.0 = full stereo)
    
    UnisonSettings() = default;
};

/**
 * Full instrument preset with envelope and modulation
 */
struct InstrumentEnvelope {
    ADSREnvelope ampEnvelope;         // Amplitude envelope
    PitchEnvelope pitchEnvelope;      // Pitch envelope (for 808, etc.)
    LFO lfo1;                         // Primary LFO
    LFO lfo2;                         // Secondary LFO
    FilterSettings filter;            // Low-pass filter
    SaturationSettings saturation;    // Soft clipping/warmth
    PortamentoSettings portamento;    // Glide between notes
    UnisonSettings unison;            // Voice doubling
    float masterVolume = 1.0f;        // Master volume (0.0 to 2.0)
    float pan = 0.0f;                 // Master pan (-1.0 to 1.0)
    
    InstrumentEnvelope() = default;
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
    void setReleaseTime(float seconds) { envelope_.ampEnvelope.release = std::max(0.001f, seconds); }
    float getReleaseTime() const { return envelope_.ampEnvelope.release; }
    
    // Oscillator management
    void setOscillators(const std::vector<Oscillator>& oscillators) { oscillators_ = oscillators; }
    const std::vector<Oscillator>& getOscillators() const { return oscillators_; }
    std::vector<Oscillator>& getOscillators() { return oscillators_; }
    
    // Envelope management
    void setEnvelope(const InstrumentEnvelope& env) { envelope_ = env; }
    const InstrumentEnvelope& getEnvelope() const { return envelope_; }
    InstrumentEnvelope& getEnvelope() { return envelope_; }
    
    // ADSR shortcuts
    void setADSR(float attack, float decay, float sustain, float release) {
        envelope_.ampEnvelope = ADSREnvelope(attack, decay, sustain, release);
    }
    
    // Pitch envelope for 808
    void setPitchEnvelope(float startMult, float decayTime) {
        envelope_.pitchEnvelope = PitchEnvelope(startMult, decayTime);
    }
    void disablePitchEnvelope() { envelope_.pitchEnvelope.enabled = false; }

private:
    enum class EnvelopePhase { Attack, Decay, Sustain, Release, Off };
    
    struct Voice {
        uint8_t note;
        float phase;
        float phaseIncrement;
        float basePhaseIncrement;   // Original phase increment (for pitch envelope)
        float targetPhaseIncrement; // Target for portamento
        float amplitude;            // Velocity-based amplitude
        float envelope;             // Current envelope value
        bool active;
        EnvelopePhase envPhase;     // Current envelope phase
        float envTime;              // Time in current phase
        float releaseStartEnvelope;
        
        // Pitch envelope state
        float pitchEnvValue;        // Current pitch multiplier from envelope
        float pitchEnvTime;         // Time elapsed in pitch envelope
        
        // Portamento state
        float portamentoProgress;   // 0.0 to 1.0
        float portamentoStartFreq;  // Starting frequency for glide
        
        // LFO state
        float lfo1Phase;
        float lfo2Phase;
        
        // Filter state (2-pole state variable filter)
        float filterEnvelope;       // Filter envelope value
        EnvelopePhase filterEnvPhase;
        float filterEnvTime;
        float filterReleaseStart;
        float filterLowL, filterBandL;  // Left channel filter state
        float filterLowR, filterBandR;  // Right channel filter state
        
        Voice() : note(0), phase(0.0f), phaseIncrement(0.0f), basePhaseIncrement(0.0f),
                  targetPhaseIncrement(0.0f), amplitude(0.0f), envelope(0.0f), active(false),
                  envPhase(EnvelopePhase::Off), envTime(0.0f),
                  releaseStartEnvelope(0.0f), pitchEnvValue(1.0f), pitchEnvTime(0.0f),
                  portamentoProgress(1.0f), portamentoStartFreq(0.0f),
                  lfo1Phase(0.0f), lfo2Phase(0.0f),
                  filterEnvelope(0.0f), filterEnvPhase(EnvelopePhase::Off), filterEnvTime(0.0f),
                  filterReleaseStart(0.0f),
                  filterLowL(0.0f), filterBandL(0.0f), filterLowR(0.0f), filterBandR(0.0f) {}
    };
    
    double sampleRate_;
    float volume_;
    Waveform waveform_;  // Deprecated: kept for backward compatibility
    std::vector<Oscillator> oscillators_;
    std::vector<Voice> voices_;
    static constexpr size_t MAX_VOICES = 16;
    
    // Envelope parameters
    InstrumentEnvelope envelope_;
    float releaseTime_;  // Deprecated: use envelope_.ampEnvelope.release
    
    // Sustain pedal state
    bool sustainPedalDown_;
    std::set<uint8_t> sustainedNotes_;
    
    // Thread safety
    std::mutex midiMutex_;
    std::vector<MidiMessage> pendingMessages_;
    std::atomic<bool> hasPendingMessages_;
    
    float noteToFrequency(uint8_t note) const;
    void updateVoice(Voice& voice, size_t numFrames);
    void handleSustainPedal(uint8_t value);
    float generateWaveform(float phase, Waveform waveform) const;
    float calculateEnvelope(Voice& voice, float deltaTime);
    float calculateFilterEnvelope(Voice& voice, float deltaTime);
    float calculatePitchEnvelope(Voice& voice, float deltaTime);
    float calculateLFO(float& phase, const LFO& lfo, float deltaTime);
    void applyFilter(Voice& voice, float& sampleL, float& sampleR, float cutoffMod);
    void applySaturation(float& sampleL, float& sampleR);
    void calculatePortamento(Voice& voice, float deltaTime);
    
    // Track last played note for portamento
    uint8_t lastNote_ = 60;
    float lastPhaseIncrement_ = 0.0f;
};

} // namespace pan
