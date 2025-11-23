#include <iostream>
#include "pan/audio/audio_engine.h"
#include "pan/track/track_manager.h"
#include "pan/midi/midi_clip.h"
#include "pan/midi/midi_message.h"
#include "pan/midi/synthesizer.h"
#include <thread>
#include <chrono>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

int main() {
    std::cout << "Pan MIDI Test - Playing a simple melody" << std::endl;
    
    // Initialize audio engine
    pan::AudioEngine engine;
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return 1;
    }
    
    // Create track manager
    pan::TrackManager trackManager;
    
    // Create a MIDI track
    auto midiTrack = trackManager.createTrack("MIDI Track", pan::Track::Type::MIDI);
    if (!midiTrack) {
        std::cerr << "Failed to create MIDI track" << std::endl;
        engine.shutdown();
        return 1;
    }
    
    // Initialize synthesizer for the track
    midiTrack->initializeSynthesizer(engine.getSampleRate());
    
    // Create a MIDI clip with a simple melody (C major scale)
    auto midiClip = std::make_shared<pan::MidiClip>("Melody");
    midiClip->setPlaying(true);
    
    // Add notes: C, D, E, F, G, A, B, C (C major scale)
    uint8_t notes[] = {60, 62, 64, 65, 67, 69, 71, 72}; // MIDI note numbers
    double sampleRate = engine.getSampleRate();
    int64_t noteDuration = static_cast<int64_t>(sampleRate * 0.3); // 300ms per note
    int64_t currentTime = 0;
    
    for (int i = 0; i < 8; ++i) {
        midiClip->addNote(currentTime, noteDuration, notes[i], 100);
        currentTime += noteDuration;
    }
    
    // Add the clip to the track
    midiTrack->addMidiClip(midiClip);
    
    // Set up audio processing
    int64_t timelinePosition = 0;
    engine.setProcessCallback([&](pan::AudioBuffer& input, pan::AudioBuffer& output, size_t numFrames) {
        output.clear();
        
        // Process tracks
        trackManager.processAllTracks(output, numFrames);
        
        // Update timeline position (simplified - real implementation needs proper transport)
        timelinePosition += numFrames;
    });
    
    // Start audio engine
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine" << std::endl;
        engine.shutdown();
        return 1;
    }
    
    std::cout << "Playing MIDI melody for 3 seconds..." << std::endl;
    std::cout << "Sample rate: " << engine.getSampleRate() << " Hz" << std::endl;
    
    // Play for 3 seconds
#ifdef PAN_USE_PORTAUDIO
    Pa_Sleep(3000);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
#endif
    
    // Stop
    engine.stop();
    engine.shutdown();
    
    std::cout << "Done!" << std::endl;
    return 0;
}

