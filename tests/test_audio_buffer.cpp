#include <cassert>
#include "pan/audio/audio_buffer.h"

void testAudioBufferCreation() {
    pan::AudioBuffer buffer(2, 1024);
    assert(buffer.getNumChannels() == 2);
    assert(buffer.getNumFrames() == 1024);
    assert(buffer.getSize() == 2048);
}

void testAudioBufferOperations() {
    pan::AudioBuffer buffer(2, 1024);
    
    // Test clear
    buffer.clear();
    const float* data = buffer.getReadPointer(0);
    assert(data[0] == 0.0f);
    
    // Test fill
    buffer.fill(0.5f);
    assert(data[0] == 0.5f);
}

int main() {
    testAudioBufferCreation();
    testAudioBufferOperations();
    return 0;
}

