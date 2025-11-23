#include <iostream>
#include "pan/audio/audio_engine.h"
#include "pan/track/track_manager.h"
#include "pan/midi/midi_input.h"
#include "pan/midi/synthesizer.h"
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

std::atomic<bool> shouldQuit{false};
pan::AudioEngine* g_engine = nullptr;
pan::MidiInput* g_midiInput = nullptr;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    shouldQuit = true;
    
    // Force cleanup
    if (g_midiInput) {
        g_midiInput->stop();
    }
    if (g_engine) {
        g_engine->stop();
        g_engine->shutdown();
    }
    
    std::exit(0);
}

int main() {
    // Set up signal handlers
    std::signal(SIGINT, signalHandler);   // Ctrl+C
    std::signal(SIGHUP, signalHandler);   // Terminal closed
    std::signal(SIGTERM, signalHandler);  // Termination request
    std::cout << "Pan MIDI Keyboard Test" << std::endl;
    std::cout << "Connect your MIDI keyboard and play notes!" << std::endl;
    
    // Initialize audio engine
    pan::AudioEngine engine;
    g_engine = &engine;
    
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return 1;
    }
    
    // Create track manager
    pan::TrackManager trackManager;
    
    // Create a MIDI track with synthesizer
    auto midiTrack = trackManager.createTrack("MIDI Keyboard", pan::Track::Type::MIDI);
    if (!midiTrack) {
        std::cerr << "Failed to create MIDI track" << std::endl;
        engine.shutdown();
        return 1;
    }
    
    // Initialize synthesizer
    midiTrack->initializeSynthesizer(engine.getSampleRate());
    
    // Create a synthesizer for direct MIDI input
    auto synth = std::make_shared<pan::Synthesizer>(engine.getSampleRate());
    synth->setVolume(0.5f);
    
    // Set up audio processing - generate audio from synthesizer
    engine.setProcessCallback([&](pan::AudioBuffer& input, pan::AudioBuffer& output, size_t numFrames) {
        output.clear();
        
        // Generate audio from synthesizer
        synth->generateAudio(output, numFrames);
        
        // Also process tracks (for future use)
        trackManager.processAllTracks(output, numFrames);
    });
    
    // Start audio engine
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine" << std::endl;
        engine.shutdown();
        return 1;
    }
    
    // Enumerate MIDI devices
    std::cout << "\nAvailable MIDI input devices:" << std::endl;
    auto midiDevices = pan::MidiInput::enumerateDevices();
    if (midiDevices.empty()) {
        std::cout << "  No MIDI devices found!" << std::endl;
        std::cout << "  Make sure your MIDI keyboard is connected." << std::endl;
    } else {
        for (size_t i = 0; i < midiDevices.size(); ++i) {
            std::cout << "  " << i << ": " << midiDevices[i] << std::endl;
        }
    }
    
    // Open MIDI input
    pan::MidiInput midiInput;
    g_midiInput = &midiInput;
    
    if (!midiDevices.empty()) {
        std::cout << "\nOpening first MIDI device: " << midiDevices[0] << std::endl;
        if (!midiInput.openDevice(midiDevices[0])) {
            std::cerr << "Failed to open MIDI device" << std::endl;
            engine.stop();
            engine.shutdown();
            return 1;
        }
        
        // Set callback to route MIDI to synthesizer
        midiInput.setCallback([&synth](const pan::MidiMessage& msg) {
            synth->processMidiMessage(msg);
        });
        
        if (!midiInput.start()) {
            std::cerr << "Failed to start MIDI input" << std::endl;
            engine.stop();
            engine.shutdown();
            return 1;
        }
        
        std::cout << "\nMIDI keyboard ready! Play some notes..." << std::endl;
        std::cout << "Press Ctrl+C or close terminal to exit.\n" << std::endl;
    } else {
        std::cout << "\nNo MIDI devices found. Running without MIDI input." << std::endl;
        std::cout << "You can still test the audio engine.\n" << std::endl;
    }
    
    // Run for a while (or until interrupted)
    while (!shouldQuit) {
#ifdef PAN_USE_PORTAUDIO
        Pa_Sleep(100);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
    }
    
    // Cleanup
    g_midiInput = nullptr;
    g_engine = nullptr;
    
    midiInput.stop();
    engine.stop();
    engine.shutdown();
    
    std::cout << "\nDone!" << std::endl;
    return 0;
}

