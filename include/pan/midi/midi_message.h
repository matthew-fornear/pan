#pragma once

#include <cstdint>
#include <vector>

namespace pan {

/**
 * MIDI message types
 */
enum class MidiMessageType {
    NoteOff = 0x80,
    NoteOn = 0x90,
    PolyphonicKeyPressure = 0xA0,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    ChannelPressure = 0xD0,
    PitchBend = 0xE0,
    SystemMessage = 0xF0
};

/**
 * Represents a MIDI message
 */
class MidiMessage {
public:
    MidiMessage();
    MidiMessage(MidiMessageType type, uint8_t channel, uint8_t data1, uint8_t data2 = 0);
    MidiMessage(const std::vector<uint8_t>& rawData);
    
    // Message properties
    MidiMessageType getType() const { return type_; }
    uint8_t getChannel() const { return channel_; }
    uint8_t getData1() const { return data1_; }
    uint8_t getData2() const { return data2_; }
    
    // Convenience methods for common messages
    bool isNoteOn() const { return type_ == MidiMessageType::NoteOn && data2_ > 0; }
    bool isNoteOff() const { return type_ == MidiMessageType::NoteOff || 
                                    (type_ == MidiMessageType::NoteOn && data2_ == 0); }
    uint8_t getNoteNumber() const { return data1_; }
    uint8_t getVelocity() const { return data2_; }
    
    // Raw data access
    std::vector<uint8_t> getRawData() const;
    
private:
    MidiMessageType type_;
    uint8_t channel_;
    uint8_t data1_;
    uint8_t data2_;
};

} // namespace pan

