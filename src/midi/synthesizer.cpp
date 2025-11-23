#include "pan/midi/synthesizer.h"
#include <algorithm>
#include <cmath>
#include <cmath>

namespace pan {

Synthesizer::Synthesizer(double sampleRate)
    : sampleRate_(sampleRate)
    , volume_(0.5f)
    , waveform_(Waveform::Sine)
    , voices_(MAX_VOICES)
    , releaseTime_(0.025f)  // 25ms default release time
    , sustainPedalDown_(false)
    , hasPendingMessages_(false)
{
    // Initialize with a default sine oscillator
    oscillators_.push_back(Oscillator(Waveform::Sine, 1.0f, 1.0f));
}

void Synthesizer::setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
}

Synthesizer::~Synthesizer() = default;

float Synthesizer::noteToFrequency(uint8_t note) const {
    // A4 (MIDI note 69) = 440 Hz
    // Use double precision for accuracy
    double noteValue = static_cast<double>(note);
    double freq = 440.0 * std::pow(2.0, (noteValue - 69.0) / 12.0);
    return static_cast<float>(freq);
}

void Synthesizer::noteOn(uint8_t note, uint8_t velocity) {
    // Remove from sustained notes if it was there
    sustainedNotes_.erase(note);
    
    // First, check if this note is already playing (including sustained)
    // If so, we can reuse that voice or stop it
    for (auto& voice : voices_) {
        if (voice.note == note && (voice.active || voice.envelope > 0.0001f)) {
            // Same note is already playing - start release phase, then we'll retrigger
            voice.active = false;
            if (voice.envelope > 0.0f) {
                voice.inRelease = true;
                voice.releaseStartEnvelope = voice.envelope;
                voice.releaseSamplesRemaining = static_cast<int>(sampleRate_ * releaseTime_);
            }
        }
    }
    
    // Find an available voice or steal the oldest one
    size_t voiceIndex = 0;
    float oldestPhase = voices_[0].phase;
    
    for (size_t i = 0; i < MAX_VOICES; ++i) {
        if (!voices_[i].active && voices_[i].envelope <= 0.0001f) {
            voiceIndex = i;
            break;
        }
        if (voices_[i].phase > oldestPhase) {
            oldestPhase = voices_[i].phase;
            voiceIndex = i;
        }
    }
    
    Voice& voice = voices_[voiceIndex];
    voice.note = note;
    voice.phase = 0.0f;
    voice.phaseIncrement = noteToFrequency(note) / static_cast<float>(sampleRate_);
    voice.amplitude = (velocity / 127.0f);  // Store velocity-based amplitude, volume applied dynamically
    voice.envelope = 1.0f;
    voice.active = true;
    voice.inRelease = false;
    voice.releaseSamplesRemaining = 0;
}

void Synthesizer::noteOff(uint8_t note) {
    for (auto& voice : voices_) {
        if (voice.active && voice.note == note) {
            if (sustainPedalDown_) {
                // Sustain pedal is down - don't release, just mark as sustained
                sustainedNotes_.insert(note);
                voice.active = false;  // Voice is no longer "active" (key released) but held by pedal
                // Keep envelope at current value (don't fade out yet)
            } else {
                // Start release phase - fade out over releaseTime_ seconds
                voice.active = false;
                voice.inRelease = true;
                voice.releaseStartEnvelope = voice.envelope;
                voice.releaseSamplesRemaining = static_cast<int>(sampleRate_ * releaseTime_);
            }
        }
    }
}

void Synthesizer::allNotesOff() {
    sustainedNotes_.clear();
    sustainPedalDown_ = false;
    for (auto& voice : voices_) {
        voice.active = false;
        if (voice.envelope > 0.0f) {
            voice.inRelease = true;
            voice.releaseStartEnvelope = voice.envelope;
            voice.releaseSamplesRemaining = static_cast<int>(sampleRate_ * releaseTime_);
        }
    }
}

void Synthesizer::processMidiMessage(const MidiMessage& message) {
    // Queue message for processing in audio thread (thread-safe)
    std::lock_guard<std::mutex> lock(midiMutex_);
    pendingMessages_.push_back(message);
    hasPendingMessages_ = true;
}

void Synthesizer::processMidiMessages(const std::vector<MidiMessage>& messages) {
    // Queue messages for processing in audio thread (thread-safe)
    std::lock_guard<std::mutex> lock(midiMutex_);
    pendingMessages_.insert(pendingMessages_.end(), messages.begin(), messages.end());
    hasPendingMessages_ = true;
}

void Synthesizer::updateVoice(Voice& voice, size_t numFrames) {
    // Not used - envelope smoothing happens per-sample in generateAudio
}

void Synthesizer::handleSustainPedal(uint8_t value) {
    // Sustain pedal: value 0-63 = off, 64-127 = on
    bool newState = (value >= 64);
    
    if (newState != sustainPedalDown_) {
        sustainPedalDown_ = newState;
        
        if (!sustainPedalDown_) {
            // Pedal was released - start release phase for all sustained notes
            for (uint8_t note : sustainedNotes_) {
                for (auto& voice : voices_) {
                    if (voice.note == note && !voice.active && voice.envelope > 0.0f) {
                        // This voice was being held by sustain, now start release
                        voice.inRelease = true;
                        voice.releaseStartEnvelope = voice.envelope;
                        voice.releaseSamplesRemaining = static_cast<int>(sampleRate_ * releaseTime_);
                    }
                }
            }
            sustainedNotes_.clear();
        }
    }
}

float Synthesizer::generateWaveform(float phase, Waveform waveform) const {
    const float twoPi = 2.0f * 3.14159265f;
    
    switch (waveform) {
        case Waveform::Sine:
            return std::sin(phase * twoPi);
            
        case Waveform::Square:
            // Square wave: +1 for first half, -1 for second half
            return (phase < 0.5f) ? 1.0f : -1.0f;
            
        case Waveform::Sawtooth:
            // Sawtooth: linear ramp from -1 to 1
            return 2.0f * phase - 1.0f;
            
        case Waveform::Triangle:
            // Triangle: linear rise to 1, then linear fall to -1
            if (phase < 0.5f) {
                return 4.0f * phase - 1.0f;  // Rise from -1 to 1
            } else {
                return 3.0f - 4.0f * phase;  // Fall from 1 to -1
            }
            
        default:
            return std::sin(phase * twoPi);
    }
}

void Synthesizer::setWaveform(Waveform waveform) {
    waveform_ = waveform;
    // Update first oscillator for backward compatibility
    if (oscillators_.empty()) {
        oscillators_.push_back(Oscillator(waveform, 1.0f, 1.0f));
    } else {
        oscillators_[0].waveform = waveform;
    }
}

Waveform Synthesizer::getWaveform() const {
    if (oscillators_.empty()) {
        return waveform_;
    }
    return oscillators_[0].waveform;
}

void Synthesizer::generateAudio(AudioBuffer& buffer, size_t numFrames) {
    // Process pending MIDI messages (called from audio thread)
    if (hasPendingMessages_.load()) {
        std::vector<MidiMessage> messages;
        {
            std::lock_guard<std::mutex> lock(midiMutex_);
            messages.swap(pendingMessages_);
            hasPendingMessages_ = false;
        }
        
        // Process all queued messages
        for (const auto& msg : messages) {
            if (msg.isNoteOn()) {
                noteOn(msg.getNoteNumber(), msg.getVelocity());
            } else if (msg.isNoteOff()) {
                noteOff(msg.getNoteNumber());
            } else if (msg.getType() == MidiMessageType::ControlChange) {
                // Handle control change messages
                uint8_t controller = msg.getData1();
                uint8_t value = msg.getData2();
                
                if (controller == 64) {  // Sustain pedal (CC 64)
                    handleSustainPedal(value);
                }
            }
        }
    }
    
    // Clear buffer first - ensure it's really zeroed
    buffer.clear();
    
    size_t numChannels = buffer.getNumChannels();
    
    // Limit number of active voices to prevent overload
    size_t activeVoiceCount = 0;
    for (const auto& voice : voices_) {
        if (voice.active || voice.envelope > 0.0f) {
            activeVoiceCount++;
        }
    }
    
    // Normalize by active voice count to prevent overload
    float voiceScale = activeVoiceCount > 0 ? (1.0f / sqrtf(static_cast<float>(activeVoiceCount))) : 1.0f;
    
    for (auto& voice : voices_) {
        if (!voice.active && voice.envelope <= 0.0f) {
            continue;
        }
        
        // Generate samples for this voice (same for all channels)
        for (size_t i = 0; i < numFrames; ++i) {
            // Handle release phase - linear fade out over releaseTime_
            if (voice.inRelease) {
                if (voice.releaseSamplesRemaining > 0) {
                    // Linear fade: envelope goes from releaseStartEnvelope to 0 over releaseSamplesRemaining
                    float progress = 1.0f - (static_cast<float>(voice.releaseSamplesRemaining) / 
                                            (static_cast<float>(sampleRate_) * releaseTime_));
                    voice.envelope = voice.releaseStartEnvelope * (1.0f - progress);
                    voice.releaseSamplesRemaining--;
                } else {
                    // Release phase complete - envelope is now 0
                    voice.envelope = 0.0f;
                    voice.inRelease = false;
                }
            }
            
            // If envelope is 0, voice is done
            if (voice.envelope <= 0.0001f && !voice.inRelease) {
                voice.envelope = 0.0f;
                break;
            }
            
            // Generate sample by summing all oscillators
            float sample = 0.0f;
            if (oscillators_.empty()) {
                // Fallback to old single-oscillator behavior
                sample = generateWaveform(voice.phase, waveform_);
            } else {
                // Sum all oscillators
                for (const auto& osc : oscillators_) {
                    // Calculate phase for this oscillator (with frequency multiplier)
                    float oscPhase = voice.phase * osc.frequencyMultiplier;
                    while (oscPhase >= 1.0f) oscPhase -= 1.0f;
                    while (oscPhase < 0.0f) oscPhase += 1.0f;
                    
                    // Generate waveform and add to sample
                    sample += generateWaveform(oscPhase, osc.waveform) * osc.amplitude;
                }
            }
            // Apply volume dynamically (so volume changes affect current notes)
            // voice.amplitude stores velocity-based amplitude, volume_ is applied here
            sample *= voice.amplitude * volume_ * voice.envelope * voiceScale;
            
            // Write the same sample to all channels
            for (size_t ch = 0; ch < numChannels; ++ch) {
                float* output = buffer.getWritePointer(ch);
                output[i] += sample;
            }
            
            // Update phase ONCE per frame (not per channel!)
            voice.phase += voice.phaseIncrement;
            while (voice.phase >= 1.0f) {
                voice.phase -= 1.0f;
            }
            while (voice.phase < 0.0f) {
                voice.phase += 1.0f;
            }
        }
    }
    
    // Final pass: soft limit to prevent any clipping
    for (size_t ch = 0; ch < numChannels; ++ch) {
        float* output = buffer.getWritePointer(ch);
        for (size_t i = 0; i < numFrames; ++i) {
            // Soft limiter
            if (output[i] > 0.95f) output[i] = 0.95f;
            if (output[i] < -0.95f) output[i] = -0.95f;
        }
    }
}

} // namespace pan

