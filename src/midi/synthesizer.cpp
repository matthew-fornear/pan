#include "pan/midi/synthesizer.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace pan {

// Static random generator for noise waveform
static std::mt19937 noiseGen(42);
static std::uniform_real_distribution<float> noiseDist(-1.0f, 1.0f);

Synthesizer::Synthesizer(double sampleRate)
    : sampleRate_(sampleRate)
    , volume_(0.5f)
    , waveform_(Waveform::Sine)
    , voices_(MAX_VOICES)
    , releaseTime_(0.3f)
    , sustainPedalDown_(false)
    , hasPendingMessages_(false)
{
    // Initialize with default envelope
    envelope_.ampEnvelope = ADSREnvelope(0.01f, 0.1f, 0.7f, 0.3f);
    
    // Initialize with a default sine oscillator
    oscillators_.push_back(Oscillator(Waveform::Sine, 1.0f, 1.0f));
}

void Synthesizer::setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 1.0f);
}

Synthesizer::~Synthesizer() = default;

float Synthesizer::noteToFrequency(uint8_t note) const {
    // A4 (MIDI note 69) = 440 Hz
    double noteValue = static_cast<double>(note);
    double freq = 440.0 * std::pow(2.0, (noteValue - 69.0) / 12.0);
    return static_cast<float>(freq);
}

void Synthesizer::noteOn(uint8_t note, uint8_t velocity) {
    sustainedNotes_.erase(note);
    
    // Check if this note is already playing
    for (auto& voice : voices_) {
        if (voice.note == note && (voice.active || voice.envelope > 0.0001f)) {
            voice.active = false;
            if (voice.envelope > 0.0f) {
                voice.envPhase = EnvelopePhase::Release;
                voice.envTime = 0.0f;
                voice.releaseStartEnvelope = voice.envelope;
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
    
    float targetFreq = noteToFrequency(note) / static_cast<float>(sampleRate_);
    voice.targetPhaseIncrement = targetFreq;
    
    // Handle portamento
    if (envelope_.portamento.enabled && lastPhaseIncrement_ > 0.0f) {
        voice.portamentoStartFreq = lastPhaseIncrement_;
        voice.phaseIncrement = lastPhaseIncrement_;
        voice.basePhaseIncrement = lastPhaseIncrement_;
        voice.portamentoProgress = 0.0f;
    } else {
        voice.basePhaseIncrement = targetFreq;
        voice.phaseIncrement = targetFreq;
        voice.portamentoProgress = 1.0f;
    }
    
    voice.amplitude = (velocity / 127.0f);
    voice.envelope = 0.0f;  // Start from 0, attack will bring it up
    voice.active = true;
    voice.envPhase = EnvelopePhase::Attack;
    voice.envTime = 0.0f;
    voice.releaseStartEnvelope = 0.0f;
    
    // Initialize pitch envelope
    if (envelope_.pitchEnvelope.enabled) {
        voice.pitchEnvValue = envelope_.pitchEnvelope.startMultiplier;
        voice.pitchEnvTime = 0.0f;
    } else {
        voice.pitchEnvValue = 1.0f;
    }
    
    // Initialize filter envelope
    if (envelope_.filter.enabled) {
        voice.filterEnvelope = 0.0f;
        voice.filterEnvPhase = EnvelopePhase::Attack;
        voice.filterEnvTime = 0.0f;
    }
    voice.filterLowL = voice.filterBandL = 0.0f;
    voice.filterLowR = voice.filterBandR = 0.0f;
    
    // Initialize LFO phases with random offset for natural variation
    voice.lfo1Phase = envelope_.lfo1.phaseOffset;
    voice.lfo2Phase = envelope_.lfo2.phaseOffset;
    
    // Remember this note for portamento
    lastNote_ = note;
    lastPhaseIncrement_ = targetFreq;
}

void Synthesizer::noteOff(uint8_t note) {
    for (auto& voice : voices_) {
        if (voice.active && voice.note == note) {
            if (sustainPedalDown_) {
                sustainedNotes_.insert(note);
                voice.active = false;
            } else {
                voice.active = false;
                voice.envPhase = EnvelopePhase::Release;
                voice.envTime = 0.0f;
                voice.releaseStartEnvelope = voice.envelope;
                
                // Also release filter envelope
                if (envelope_.filter.enabled) {
                    voice.filterEnvPhase = EnvelopePhase::Release;
                    voice.filterEnvTime = 0.0f;
                    voice.filterReleaseStart = voice.filterEnvelope;
                }
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
            voice.envPhase = EnvelopePhase::Release;
            voice.envTime = 0.0f;
            voice.releaseStartEnvelope = voice.envelope;
        }
    }
}

void Synthesizer::processMidiMessage(const MidiMessage& message) {
    std::lock_guard<std::mutex> lock(midiMutex_);
    pendingMessages_.push_back(message);
    hasPendingMessages_ = true;
}

void Synthesizer::processMidiMessages(const std::vector<MidiMessage>& messages) {
    std::lock_guard<std::mutex> lock(midiMutex_);
    pendingMessages_.insert(pendingMessages_.end(), messages.begin(), messages.end());
    hasPendingMessages_ = true;
}

void Synthesizer::updateVoice(Voice& voice, size_t numFrames) {
    // Not used - envelope processing happens per-sample
}

void Synthesizer::handleSustainPedal(uint8_t value) {
    bool newState = (value >= 64);
    
    if (newState != sustainPedalDown_) {
        sustainPedalDown_ = newState;
        
        if (!sustainPedalDown_) {
            for (uint8_t note : sustainedNotes_) {
                for (auto& voice : voices_) {
                    if (voice.note == note && !voice.active && voice.envelope > 0.0f) {
                        voice.envPhase = EnvelopePhase::Release;
                        voice.envTime = 0.0f;
                        voice.releaseStartEnvelope = voice.envelope;
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
            return (phase < 0.5f) ? 1.0f : -1.0f;
            
        case Waveform::Sawtooth:
            return 2.0f * phase - 1.0f;
            
        case Waveform::Triangle:
            if (phase < 0.5f) {
                return 4.0f * phase - 1.0f;
            } else {
                return 3.0f - 4.0f * phase;
            }
            
        case Waveform::Noise:
            return noiseDist(noiseGen);
            
        default:
            return std::sin(phase * twoPi);
    }
}

float Synthesizer::calculateEnvelope(Voice& voice, float deltaTime) {
    const auto& env = envelope_.ampEnvelope;
    
    switch (voice.envPhase) {
        case EnvelopePhase::Attack: {
            voice.envTime += deltaTime;
            if (env.attack <= 0.001f) {
                voice.envelope = 1.0f;
                voice.envPhase = EnvelopePhase::Decay;
                voice.envTime = 0.0f;
            } else {
                // Exponential attack for smoother rise
                float progress = voice.envTime / env.attack;
                voice.envelope = 1.0f - std::exp(-5.0f * progress);
                if (progress >= 1.0f) {
                    voice.envelope = 1.0f;
                    voice.envPhase = EnvelopePhase::Decay;
                    voice.envTime = 0.0f;
                }
            }
            break;
        }
        case EnvelopePhase::Decay: {
            voice.envTime += deltaTime;
            if (env.decay <= 0.001f) {
                voice.envelope = env.sustain;
                voice.envPhase = EnvelopePhase::Sustain;
            } else {
                float progress = voice.envTime / env.decay;
                // Exponential decay from 1.0 to sustain level
                voice.envelope = env.sustain + (1.0f - env.sustain) * std::exp(-5.0f * progress);
                if (progress >= 1.0f) {
                    voice.envelope = env.sustain;
                    voice.envPhase = EnvelopePhase::Sustain;
                }
            }
            break;
        }
        case EnvelopePhase::Sustain:
            voice.envelope = env.sustain;
            break;
            
        case EnvelopePhase::Release: {
            voice.envTime += deltaTime;
            if (env.release <= 0.001f) {
                voice.envelope = 0.0f;
                voice.envPhase = EnvelopePhase::Off;
            } else {
                float progress = voice.envTime / env.release;
                // Exponential release
                voice.envelope = voice.releaseStartEnvelope * std::exp(-5.0f * progress);
                if (voice.envelope < 0.0001f || progress >= 1.0f) {
                    voice.envelope = 0.0f;
                    voice.envPhase = EnvelopePhase::Off;
                }
            }
            break;
        }
        case EnvelopePhase::Off:
            voice.envelope = 0.0f;
            break;
    }
    
    return voice.envelope;
}

float Synthesizer::calculatePitchEnvelope(Voice& voice, float deltaTime) {
    if (!envelope_.pitchEnvelope.enabled) {
        return 1.0f;
    }
    
    voice.pitchEnvTime += deltaTime;
    
    const auto& pEnv = envelope_.pitchEnvelope;
    if (pEnv.decayTime <= 0.001f) {
        voice.pitchEnvValue = 1.0f;
        return 1.0f;
    }
    
    // Exponential decay from startMultiplier to 1.0
    // Using a smoother curve for more musical pitch drop
    float progress = voice.pitchEnvTime / pEnv.decayTime;
    if (progress >= 1.0f) {
        voice.pitchEnvValue = 1.0f;
    } else {
        // Logarithmic curve feels more natural for pitch drops
        // This creates the classic 808 "boom" sound
        float curve = 1.0f - progress;
        curve = curve * curve;  // Square for faster initial drop
        voice.pitchEnvValue = 1.0f + (pEnv.startMultiplier - 1.0f) * curve;
    }
    
    return voice.pitchEnvValue;
}

float Synthesizer::calculateFilterEnvelope(Voice& voice, float deltaTime) {
    if (!envelope_.filter.enabled) {
        return 0.0f;
    }
    
    const auto& fEnv = envelope_.filter.envelope;
    
    switch (voice.filterEnvPhase) {
        case EnvelopePhase::Attack: {
            voice.filterEnvTime += deltaTime;
            if (fEnv.attack <= 0.001f) {
                voice.filterEnvelope = 1.0f;
                voice.filterEnvPhase = EnvelopePhase::Decay;
                voice.filterEnvTime = 0.0f;
            } else {
                float progress = voice.filterEnvTime / fEnv.attack;
                voice.filterEnvelope = 1.0f - std::exp(-5.0f * progress);
                if (progress >= 1.0f) {
                    voice.filterEnvelope = 1.0f;
                    voice.filterEnvPhase = EnvelopePhase::Decay;
                    voice.filterEnvTime = 0.0f;
                }
            }
            break;
        }
        case EnvelopePhase::Decay: {
            voice.filterEnvTime += deltaTime;
            if (fEnv.decay <= 0.001f) {
                voice.filterEnvelope = fEnv.sustain;
                voice.filterEnvPhase = EnvelopePhase::Sustain;
            } else {
                float progress = voice.filterEnvTime / fEnv.decay;
                voice.filterEnvelope = fEnv.sustain + (1.0f - fEnv.sustain) * std::exp(-5.0f * progress);
                if (progress >= 1.0f) {
                    voice.filterEnvelope = fEnv.sustain;
                    voice.filterEnvPhase = EnvelopePhase::Sustain;
                }
            }
            break;
        }
        case EnvelopePhase::Sustain:
            voice.filterEnvelope = fEnv.sustain;
            break;
        case EnvelopePhase::Release: {
            voice.filterEnvTime += deltaTime;
            if (fEnv.release <= 0.001f) {
                voice.filterEnvelope = 0.0f;
                voice.filterEnvPhase = EnvelopePhase::Off;
            } else {
                float progress = voice.filterEnvTime / fEnv.release;
                voice.filterEnvelope = voice.filterReleaseStart * std::exp(-5.0f * progress);
                if (voice.filterEnvelope < 0.0001f || progress >= 1.0f) {
                    voice.filterEnvelope = 0.0f;
                    voice.filterEnvPhase = EnvelopePhase::Off;
                }
            }
            break;
        }
        case EnvelopePhase::Off:
            voice.filterEnvelope = 0.0f;
            break;
    }
    
    return voice.filterEnvelope;
}

void Synthesizer::applyFilter(Voice& voice, float& sampleL, float& sampleR, float cutoffMod) {
    if (!envelope_.filter.enabled) {
        return;
    }
    
    // Calculate effective cutoff with envelope modulation
    float baseCutoff = envelope_.filter.cutoff;
    float envMod = cutoffMod * envelope_.filter.envAmount;
    float effectiveCutoff = std::clamp(baseCutoff + envMod, 0.01f, 0.99f);
    
    // Convert normalized cutoff to frequency (20Hz to 20kHz logarithmic)
    float cutoffHz = 20.0f * std::pow(1000.0f, effectiveCutoff);
    
    // Calculate filter coefficients (state variable filter)
    float f = 2.0f * std::sin(3.14159265f * cutoffHz / static_cast<float>(sampleRate_));
    f = std::min(f, 1.0f);  // Stability limit
    
    float q = 1.0f - envelope_.filter.resonance * 0.9f;  // Q from 1.0 to 0.1
    q = std::max(q, 0.1f);
    
    // Process left channel
    float highL = sampleL - voice.filterLowL - q * voice.filterBandL;
    voice.filterBandL += f * highL;
    voice.filterLowL += f * voice.filterBandL;
    
    // Process right channel
    float highR = sampleR - voice.filterLowR - q * voice.filterBandR;
    voice.filterBandR += f * highR;
    voice.filterLowR += f * voice.filterBandR;
    
    // Output low-pass
    sampleL = voice.filterLowL;
    sampleR = voice.filterLowR;
}

void Synthesizer::applySaturation(float& sampleL, float& sampleR) {
    if (!envelope_.saturation.enabled) {
        return;
    }
    
    float drive = envelope_.saturation.drive;
    float mix = envelope_.saturation.mix;
    
    // Store dry signal
    float dryL = sampleL;
    float dryR = sampleR;
    
    // Apply drive
    float wetL = sampleL * drive;
    float wetR = sampleR * drive;
    
    // Soft clipping using tanh (smooth, tube-like saturation)
    wetL = std::tanh(wetL);
    wetR = std::tanh(wetR);
    
    // Normalize output
    wetL /= std::max(1.0f, drive * 0.5f);
    wetR /= std::max(1.0f, drive * 0.5f);
    
    // Mix dry/wet
    sampleL = dryL * (1.0f - mix) + wetL * mix;
    sampleR = dryR * (1.0f - mix) + wetR * mix;
}

void Synthesizer::calculatePortamento(Voice& voice, float deltaTime) {
    if (!envelope_.portamento.enabled || voice.portamentoProgress >= 1.0f) {
        return;
    }
    
    float glideTime = envelope_.portamento.time;
    if (glideTime <= 0.001f) {
        voice.portamentoProgress = 1.0f;
        voice.basePhaseIncrement = voice.targetPhaseIncrement;
        return;
    }
    
    // Update progress
    voice.portamentoProgress += deltaTime / glideTime;
    if (voice.portamentoProgress >= 1.0f) {
        voice.portamentoProgress = 1.0f;
        voice.basePhaseIncrement = voice.targetPhaseIncrement;
    } else {
        // Exponential interpolation for more natural glide
        float t = voice.portamentoProgress;
        t = t * t * (3.0f - 2.0f * t);  // Smoothstep
        
        // Interpolate in log space for musical pitch
        float logStart = std::log(voice.portamentoStartFreq);
        float logEnd = std::log(voice.targetPhaseIncrement);
        float logCurrent = logStart + t * (logEnd - logStart);
        voice.basePhaseIncrement = std::exp(logCurrent);
    }
}

float Synthesizer::calculateLFO(float& phase, const LFO& lfo, float deltaTime) {
    if (!lfo.enabled || lfo.depth <= 0.0f) {
        return 0.0f;
    }
    
    // Update LFO phase
    phase += lfo.rate * deltaTime;
    while (phase >= 1.0f) phase -= 1.0f;
    
    // Generate sine LFO
    float lfoValue = std::sin(phase * 2.0f * 3.14159265f);
    
    return lfoValue * lfo.depth;
}

void Synthesizer::setWaveform(Waveform waveform) {
    waveform_ = waveform;
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
    // Process pending MIDI messages
    if (hasPendingMessages_.load()) {
        std::vector<MidiMessage> messages;
        {
            std::lock_guard<std::mutex> lock(midiMutex_);
            messages.swap(pendingMessages_);
            hasPendingMessages_ = false;
        }
        
        for (const auto& msg : messages) {
            if (msg.isNoteOn()) {
                noteOn(msg.getNoteNumber(), msg.getVelocity());
            } else if (msg.isNoteOff()) {
                noteOff(msg.getNoteNumber());
            } else if (msg.getType() == MidiMessageType::ControlChange) {
                uint8_t controller = msg.getData1();
                uint8_t value = msg.getData2();
                
                if (controller == 64) {
                    handleSustainPedal(value);
                }
            }
        }
    }
    
    buffer.clear();
    
    size_t numChannels = buffer.getNumChannels();
    float deltaTime = 1.0f / static_cast<float>(sampleRate_);
    
    // Count active voices for scaling
    size_t activeVoiceCount = 0;
    for (const auto& voice : voices_) {
        if (voice.active || voice.envelope > 0.0f || voice.envPhase != EnvelopePhase::Off) {
            activeVoiceCount++;
        }
    }
    
    float voiceScale = activeVoiceCount > 0 ? (1.0f / sqrtf(static_cast<float>(activeVoiceCount))) : 1.0f;
    
    for (auto& voice : voices_) {
        if (voice.envPhase == EnvelopePhase::Off && voice.envelope <= 0.0f) {
            continue;
        }
        
        for (size_t i = 0; i < numFrames; ++i) {
            // Calculate ADSR envelope
            float envValue = calculateEnvelope(voice, deltaTime);
            
            if (envValue <= 0.0001f && voice.envPhase == EnvelopePhase::Off) {
                break;
            }
            
            // Calculate portamento (glide between notes)
            calculatePortamento(voice, deltaTime);
            
            // Calculate pitch envelope (for 808-style pitch drop)
            float pitchMod = calculatePitchEnvelope(voice, deltaTime);
            
            // Calculate filter envelope
            float filterEnvValue = calculateFilterEnvelope(voice, deltaTime);
            
            // Calculate LFO modulations
            float lfo1Value = calculateLFO(voice.lfo1Phase, envelope_.lfo1, deltaTime);
            float lfo2Value = calculateLFO(voice.lfo2Phase, envelope_.lfo2, deltaTime);
            
            // Apply LFO to pitch if configured
            float lfoFreqMod = 1.0f;
            if (envelope_.lfo1.enabled && envelope_.lfo1.target == LFO::Target::Pitch) {
                lfoFreqMod *= std::pow(2.0f, lfo1Value / 12.0f);  // LFO depth in semitones
            }
            if (envelope_.lfo2.enabled && envelope_.lfo2.target == LFO::Target::Pitch) {
                lfoFreqMod *= std::pow(2.0f, lfo2Value / 12.0f);
            }
            
            // Apply LFO to amplitude if configured
            float lfoAmpMod = 1.0f;
            if (envelope_.lfo1.enabled && envelope_.lfo1.target == LFO::Target::Amplitude) {
                lfoAmpMod *= (1.0f + lfo1Value * 0.5f);  // 50% modulation depth
            }
            if (envelope_.lfo2.enabled && envelope_.lfo2.target == LFO::Target::Amplitude) {
                lfoAmpMod *= (1.0f + lfo2Value * 0.5f);
            }
            
            // Update phase increment with pitch envelope and LFO
            voice.phaseIncrement = voice.basePhaseIncrement * pitchMod * lfoFreqMod;
            
            // Generate sample by summing all oscillators
            float sampleL = 0.0f;
            float sampleR = 0.0f;
            
            // Handle unison - generate multiple detuned voices
            int unisonVoices = envelope_.unison.enabled ? std::max(1, envelope_.unison.voices) : 1;
            float unisonScale = 1.0f / std::sqrt(static_cast<float>(unisonVoices));
            
            for (int uniVoice = 0; uniVoice < unisonVoices; ++uniVoice) {
                // Calculate unison detune and pan
                float unisonDetune = 0.0f;
                float unisonPan = 0.0f;
                if (unisonVoices > 1) {
                    float voicePos = (static_cast<float>(uniVoice) / (unisonVoices - 1)) - 0.5f;  // -0.5 to 0.5
                    unisonDetune = voicePos * envelope_.unison.detune * 2.0f;  // Spread across detune range
                    unisonPan = voicePos * envelope_.unison.spread * 2.0f;      // Spread across stereo
                }
                float uniDetuneMultiplier = std::pow(2.0f, unisonDetune / 1200.0f);
                
                if (oscillators_.empty()) {
                    float sample = generateWaveform(voice.phase * uniDetuneMultiplier, waveform_);
                    float panL = std::cos((unisonPan + 1.0f) * 0.25f * 3.14159265f);
                    float panR = std::sin((unisonPan + 1.0f) * 0.25f * 3.14159265f);
                    sampleL += sample * panL * unisonScale;
                    sampleR += sample * panR * unisonScale;
                } else {
                    for (const auto& osc : oscillators_) {
                        // Apply detune in cents (oscillator detune + unison detune)
                        float totalDetune = osc.detune + unisonDetune;
                        float detuneMultiplier = std::pow(2.0f, totalDetune / 1200.0f);
                        
                        // Calculate phase for this oscillator
                        float oscPhase = voice.phase * osc.frequencyMultiplier * detuneMultiplier;
                        while (oscPhase >= 1.0f) oscPhase -= 1.0f;
                        while (oscPhase < 0.0f) oscPhase += 1.0f;
                        
                        float sample = generateWaveform(oscPhase, osc.waveform) * osc.amplitude;
                        
                        // Apply stereo panning (oscillator pan + unison pan)
                        float totalPan = std::clamp(osc.pan + unisonPan, -1.0f, 1.0f);
                        float panL = std::cos((totalPan + 1.0f) * 0.25f * 3.14159265f);
                        float panR = std::sin((totalPan + 1.0f) * 0.25f * 3.14159265f);
                        
                        sampleL += sample * panL * unisonScale;
                        sampleR += sample * panR * unisonScale;
                    }
                }
            }
            
            // Apply saturation/soft clipping
            applySaturation(sampleL, sampleR);
            
            // Apply filter before amplitude envelope
            applyFilter(voice, sampleL, sampleR, filterEnvValue);
            
            // Apply envelope, velocity, volume, LFO amp mod, and voice scaling
            float finalGain = voice.amplitude * volume_ * envValue * lfoAmpMod * voiceScale;
            finalGain *= envelope_.masterVolume;  // Apply master volume
            sampleL *= finalGain;
            sampleR *= finalGain;
            
            // Apply master pan
            if (std::abs(envelope_.pan) > 0.001f) {
                float panL = std::cos((envelope_.pan + 1.0f) * 0.25f * 3.14159265f);
                float panR = std::sin((envelope_.pan + 1.0f) * 0.25f * 3.14159265f);
                float mono = (sampleL + sampleR) * 0.5f;
                sampleL = mono * panL * 1.414f;  // Compensate for energy loss
                sampleR = mono * panR * 1.414f;
            }
            
            // Write to buffer
            if (numChannels >= 2) {
                buffer.getWritePointer(0)[i] += sampleL;
                buffer.getWritePointer(1)[i] += sampleR;
            } else if (numChannels == 1) {
                buffer.getWritePointer(0)[i] += (sampleL + sampleR) * 0.5f;
            }
            
            // Update phase with modulated increment
            voice.phase += voice.phaseIncrement;
            while (voice.phase >= 1.0f) {
                voice.phase -= 1.0f;
            }
        }
    }
    
    // Soft limit to prevent clipping
    for (size_t ch = 0; ch < numChannels; ++ch) {
        float* output = buffer.getWritePointer(ch);
        for (size_t i = 0; i < numFrames; ++i) {
            // Soft tanh limiting
            output[i] = std::tanh(output[i]);
        }
    }
}

} // namespace pan
