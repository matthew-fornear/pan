#pragma once

#include <string>
#include <memory>
#include <vector>
#include "pan/audio/audio_buffer.h"
#include "pan/track/audio_clip.h"

namespace pan {

class MidiClip;
class Synthesizer;
class EffectChain;

/**
 * Represents a single audio track in the project
 */
class Track {
public:
    enum class Type {
        Audio,
        MIDI,
        Bus,
        Master
    };

    Track(const std::string& name, Type type = Type::Audio);
    ~Track();

    // Track properties
    std::string getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    
    Type getType() const { return type_; }
    void setType(Type type) { type_ = type; }

    // Volume and panning
    float getVolume() const { return volume_; }
    void setVolume(float volume);
    
    float getPan() const { return pan_; }
    void setPan(float pan);

    // Mute and solo
    bool isMuted() const { return muted_; }
    void setMuted(bool muted) { muted_ = muted; }
    
    bool isSoloed() const { return soloed_; }
    void setSoloed(bool soloed) { soloed_ = soloed; }

    // Audio clips
    void addClip(std::shared_ptr<AudioClip> clip);
    void removeClip(std::shared_ptr<AudioClip> clip);
    std::vector<std::shared_ptr<AudioClip>> getClips() const { return clips_; }
    
    // MIDI clips
    void addMidiClip(std::shared_ptr<MidiClip> clip);
    void removeMidiClip(std::shared_ptr<MidiClip> clip);
    std::vector<std::shared_ptr<MidiClip>> getMidiClips() const { return midiClips_; }

    // Effects
    void addEffect(std::shared_ptr<EffectChain> effect);
    std::shared_ptr<EffectChain> getEffectChain() const { return effectChain_; }

    // Processing
    void process(AudioBuffer& buffer, size_t numFrames);
    
    // MIDI synthesizer initialization
    void initializeSynthesizer(double sampleRate);

private:
    std::string name_;
    Type type_;
    float volume_;
    float pan_;
    bool muted_;
    bool soloed_;
    
    std::vector<std::shared_ptr<AudioClip>> clips_;
    std::vector<std::shared_ptr<MidiClip>> midiClips_;
    std::shared_ptr<Synthesizer> synthesizer_;  // For MIDI tracks
    std::shared_ptr<EffectChain> effectChain_;
};

} // namespace pan

