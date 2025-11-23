#include "pan/midi/midi_message.h"
#include <algorithm>

namespace pan {

MidiMessage::MidiMessage()
    : type_(MidiMessageType::NoteOff)
    , channel_(0)
    , data1_(0)
    , data2_(0)
{
}

MidiMessage::MidiMessage(MidiMessageType type, uint8_t channel, uint8_t data1, uint8_t data2)
    : type_(type)
    , channel_(channel & 0x0F)  // Channel is 4 bits
    , data1_(data1 & 0x7F)      // Data is 7 bits
    , data2_(data2 & 0x7F)
{
}

MidiMessage::MidiMessage(const std::vector<uint8_t>& rawData) {
    if (rawData.empty()) {
        type_ = MidiMessageType::NoteOff;
        channel_ = 0;
        data1_ = 0;
        data2_ = 0;
        return;
    }
    
    uint8_t status = rawData[0];
    type_ = static_cast<MidiMessageType>(status & 0xF0);
    channel_ = status & 0x0F;
    
    if (rawData.size() > 1) {
        data1_ = rawData[1] & 0x7F;
    } else {
        data1_ = 0;
    }
    
    if (rawData.size() > 2) {
        data2_ = rawData[2] & 0x7F;
    } else {
        data2_ = 0;
    }
}

std::vector<uint8_t> MidiMessage::getRawData() const {
    std::vector<uint8_t> data;
    uint8_t status = static_cast<uint8_t>(type_) | channel_;
    data.push_back(status);
    data.push_back(data1_);
    
    // Some messages only have one data byte
    if (type_ != MidiMessageType::ProgramChange && 
        type_ != MidiMessageType::ChannelPressure) {
        data.push_back(data2_);
    }
    
    return data;
}

} // namespace pan

