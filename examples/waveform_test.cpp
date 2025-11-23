#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include "pan/audio/audio_engine.h"
#include "pan/midi/midi_input.h"
#include "pan/midi/synthesizer.h"

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

void printMenu(int currentWaveform) {
    std::cout << "\n=== Pan Waveform Test ===" << std::endl;
    std::cout << "Current Waveform: ";
    switch (currentWaveform) {
        case 0: std::cout << "Sine"; break;
        case 1: std::cout << "Square"; break;
        case 2: std::cout << "Sawtooth"; break;
        case 3: std::cout << "Triangle"; break;
    }
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  1 - Sine" << std::endl;
    std::cout << "  2 - Square" << std::endl;
    std::cout << "  3 - Sawtooth" << std::endl;
    std::cout << "  4 - Triangle" << std::endl;
    std::cout << "  q - Quit" << std::endl;
    std::cout << "\nSelect waveform (1-4): ";
}

int main() {
    // Set up signal handlers
    std::signal(SIGINT, signalHandler);   // Ctrl+C
    std::signal(SIGHUP, signalHandler);   // Terminal closed
    std::signal(SIGTERM, signalHandler);  // Termination request
    
    std::cout << "Pan Waveform Test" << std::endl;
    std::cout << "Connect your MIDI keyboard and play notes!" << std::endl;
    std::cout << "Press Ctrl+C or close terminal to exit." << std::endl;
    
    // Initialize audio engine
    pan::AudioEngine engine;
    g_engine = &engine;
    
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return 1;
    }
    
    // Create synthesizer
    auto synth = std::make_shared<pan::Synthesizer>(engine.getSampleRate());
    synth->setVolume(0.5f);
    synth->setWaveform(pan::Waveform::Sine);
    
    int currentWaveform = 0;
    std::atomic<int> waveformSelection{0};
    
    // Set up audio processing
    engine.setProcessCallback([&](pan::AudioBuffer& input, pan::AudioBuffer& output, size_t numFrames) {
        output.clear();
        synth->generateAudio(output, numFrames);
    });
    
    // Start audio engine
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine" << std::endl;
        engine.shutdown();
        return 1;
    }
    
    // Setup MIDI input
    pan::MidiInput midiInput;
    g_midiInput = &midiInput;
    auto midiDevices = pan::MidiInput::enumerateDevices();
    
    if (!midiDevices.empty()) {
        std::cout << "Opening MIDI device: " << midiDevices[0] << std::endl;
        if (midiInput.openDevice(midiDevices[0])) {
            midiInput.setCallback([&synth](const pan::MidiMessage& msg) {
                synth->processMidiMessage(msg);
            });
            midiInput.start();
            std::cout << "MIDI keyboard ready!" << std::endl;
        }
    } else {
        std::cout << "No MIDI devices found" << std::endl;
    }
    
    // Interactive menu loop in a separate thread
    std::thread menuThread([&]() {
        std::string input;
        while (!shouldQuit) {
            printMenu(currentWaveform);
            std::getline(std::cin, input);
            
            if (input == "q" || input == "Q") {
                shouldQuit = true;
                break;
            } else if (input == "1") {
                currentWaveform = 0;
                waveformSelection = 0;
                synth->setWaveform(pan::Waveform::Sine);
                std::cout << "Switched to Sine wave" << std::endl;
            } else if (input == "2") {
                currentWaveform = 1;
                waveformSelection = 1;
                synth->setWaveform(pan::Waveform::Square);
                std::cout << "Switched to Square wave" << std::endl;
            } else if (input == "3") {
                currentWaveform = 2;
                waveformSelection = 2;
                synth->setWaveform(pan::Waveform::Sawtooth);
                std::cout << "Switched to Sawtooth wave" << std::endl;
            } else if (input == "4") {
                currentWaveform = 3;
                waveformSelection = 3;
                synth->setWaveform(pan::Waveform::Triangle);
                std::cout << "Switched to Triangle wave" << std::endl;
            } else {
                std::cout << "Invalid selection!" << std::endl;
            }
        }
    });
    
    // Main loop
    while (!shouldQuit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Cleanup
    menuThread.join();
    g_midiInput = nullptr;
    g_engine = nullptr;
    
    midiInput.stop();
    engine.stop();
    engine.shutdown();
    
    std::cout << "\nDone!" << std::endl;
    return 0;
}

