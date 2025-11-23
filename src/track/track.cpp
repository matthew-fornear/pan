#include "pan/track/track.h"
#include "pan/track/audio_clip.h"
#include "pan/midi/midi_clip.h"
#include "pan/midi/synthesizer.h"
#include "pan/audio/audio_buffer.h"
#include <algorithm>
#include <cmath>

namespace pan {

Track::Track(const std::string& name, Type type)
    : name_(name)
    , type_(type)
    , volume_(1.0f)
    , pan_(0.0f)
    , muted_(false)
    , soloed_(false)
{
}

Track::~Track() = default;

void Track::setVolume(float volume) {
    volume_ = std::clamp(volume, 0.0f, 2.0f);
}

void Track::setPan(float pan) {
    pan_ = std::clamp(pan, -1.0f, 1.0f);
}

void Track::addClip(std::shared_ptr<AudioClip> clip) {
    if (clip) {
        clips_.push_back(clip);
    }
}

void Track::removeClip(std::shared_ptr<AudioClip> clip) {
    clips_.erase(
        std::remove(clips_.begin(), clips_.end(), clip),
        clips_.end()
    );
}

void Track::addEffect(std::shared_ptr<EffectChain> effect) {
    effectChain_ = effect;
}

void Track::addMidiClip(std::shared_ptr<MidiClip> clip) {
    if (clip) {
        midiClips_.push_back(clip);
    }
}

void Track::removeMidiClip(std::shared_ptr<MidiClip> clip) {
    midiClips_.erase(
        std::remove(midiClips_.begin(), midiClips_.end(), clip),
        midiClips_.end()
    );
}

void Track::initializeSynthesizer(double sampleRate) {
    if (!synthesizer_) {
        synthesizer_ = std::make_shared<Synthesizer>(sampleRate);
    }
}

void Track::process(AudioBuffer& buffer, size_t numFrames) {
    if (muted_) {
        return;
    }

    // Process audio clips
    for (auto& clip : clips_) {
        if (!clip || !clip->hasAudioData() || !clip->isPlaying()) {
            continue;
        }
        
        auto clipData = clip->getAudioData();
        if (!clipData) {
            continue;
        }
        
        // TODO: Implement timeline position tracking
        size_t numChannels = std::min(buffer.getNumChannels(), clipData->getNumChannels());
        size_t framesToMix = std::min(numFrames, clipData->getNumFrames());
        float clipGain = clip->getGain() * volume_;
        
        for (size_t ch = 0; ch < numChannels; ++ch) {
            const float* clipSamples = clipData->getReadPointer(ch);
            float* outputSamples = buffer.getWritePointer(ch);
            
            for (size_t i = 0; i < framesToMix; ++i) {
                outputSamples[i] += clipSamples[i] * clipGain;
            }
        }
    }
    
    // Process MIDI clips
    if (type_ == Type::MIDI && !midiClips_.empty()) {
        // Initialize synthesizer if needed (requires sample rate - will be set later)
        // For now, we'll create a temporary buffer for MIDI synthesis
        AudioBuffer midiBuffer(buffer.getNumChannels(), numFrames);
        
        // Process MIDI events from all clips
        std::vector<pan::MidiMessage> midiMessages;
        for (auto& midiClip : midiClips_) {
            if (!midiClip || !midiClip->isPlaying()) {
                continue;
            }
            
            // Get events in the current time range (simplified - assumes starting from 0)
            // TODO: Use actual timeline position
            const auto& events = midiClip->getEvents();
            for (const auto& event : events) {
                midiMessages.push_back(event.message);
            }
        }
        
        // Process all MIDI messages through synthesizer
        if (synthesizer_ && !midiMessages.empty()) {
            synthesizer_->processMidiMessages(midiMessages);
        }
        
        // Generate audio from MIDI
        if (synthesizer_) {
            synthesizer_->generateAudio(midiBuffer, numFrames);
            
            // Mix MIDI audio into output buffer
            for (size_t ch = 0; ch < buffer.getNumChannels(); ++ch) {
                const float* midiSamples = midiBuffer.getReadPointer(ch);
                float* outputSamples = buffer.getWritePointer(ch);
                
                for (size_t i = 0; i < numFrames; ++i) {
                    outputSamples[i] += midiSamples[i] * volume_;
                }
            }
        }
    }
    
    // TODO: Apply effects (effectChain_)
    // TODO: Apply panning
}

} // namespace pan

