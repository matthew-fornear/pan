#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include "pan/midi/midi_message.h"

namespace pan {

/**
 * Represents a MIDI clip containing MIDI events
 */
class MidiClip {
public:
    struct MidiEvent {
        int64_t timestamp;  // Time in samples from clip start
        MidiMessage message;
        
        MidiEvent(int64_t ts, const MidiMessage& msg) : timestamp(ts), message(msg) {}
    };

    MidiClip(const std::string& name);
    ~MidiClip();

    // Clip properties
    std::string getName() const { return name_; }
    void setName(const std::string& name) { name_ = name; }
    
    // Timeline position (in samples)
    int64_t getStartTime() const { return startTime_; }
    void setStartTime(int64_t startTime) { startTime_ = startTime; }
    
    int64_t getEndTime() const { return endTime_; }
    int64_t getLength() const { return endTime_ - startTime_; }
    
    // MIDI events
    void addEvent(int64_t timestamp, const MidiMessage& message);
    void addNote(int64_t startTime, int64_t duration, uint8_t note, uint8_t velocity = 100);
    const std::vector<MidiEvent>& getEvents() const { return events_; }
    
    // Playback state
    bool isPlaying() const { return isPlaying_; }
    void setPlaying(bool playing) { isPlaying_ = playing; }
    
    // Get events in a time range
    std::vector<MidiEvent> getEventsInRange(int64_t startSample, int64_t endSample) const;

private:
    std::string name_;
    int64_t startTime_;
    int64_t endTime_;
    std::vector<MidiEvent> events_;
    bool isPlaying_;
    
    void updateEndTime();
};

} // namespace pan

