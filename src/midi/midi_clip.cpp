#include "pan/midi/midi_clip.h"
#include <algorithm>

namespace pan {

MidiClip::MidiClip(const std::string& name)
    : name_(name)
    , startTime_(0)
    , endTime_(0)
    , isPlaying_(false)
{
}

MidiClip::~MidiClip() = default;

void MidiClip::addEvent(int64_t timestamp, const MidiMessage& message) {
    events_.emplace_back(timestamp, message);
    // Keep events sorted by timestamp
    std::sort(events_.begin(), events_.end(), 
              [](const MidiEvent& a, const MidiEvent& b) {
                  return a.timestamp < b.timestamp;
              });
    updateEndTime();
}

void MidiClip::addNote(int64_t startTime, int64_t duration, uint8_t note, uint8_t velocity) {
    // Add note on
    addEvent(startTime, MidiMessage(MidiMessageType::NoteOn, 0, note, velocity));
    // Add note off
    addEvent(startTime + duration, MidiMessage(MidiMessageType::NoteOff, 0, note, 0));
}

void MidiClip::updateEndTime() {
    if (events_.empty()) {
        endTime_ = startTime_;
        return;
    }
    
    // Find the latest event timestamp
    int64_t latestTime = startTime_;
    for (const auto& event : events_) {
        latestTime = std::max(latestTime, startTime_ + event.timestamp);
    }
    endTime_ = latestTime;
}

std::vector<MidiClip::MidiEvent> MidiClip::getEventsInRange(int64_t startSample, int64_t endSample) const {
    std::vector<MidiEvent> result;
    
    // Convert absolute samples to clip-relative samples
    int64_t clipStart = startSample - startTime_;
    int64_t clipEnd = endSample - startTime_;
    
    for (const auto& event : events_) {
        if (event.timestamp >= clipStart && event.timestamp < clipEnd) {
            result.push_back(event);
        }
    }
    
    return result;
}

} // namespace pan

