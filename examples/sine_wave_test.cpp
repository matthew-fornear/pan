#include "pan/audio/audio_engine.h"
#include "pan/audio/audio_buffer.h"
#include <iostream>
#include <cmath>
#include <thread>
#include <chrono>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

// Simple sine wave generator for testing
int main() {
    pan::AudioEngine engine;
    
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return 1;
    }
    
    // Generate a 440 Hz sine wave (A4 note)
    double frequency = 440.0;
    double phase = 0.0;
    double sampleRate = engine.getSampleRate();
    double phaseIncrement = 2.0 * M_PI * frequency / sampleRate;
    
    // Set up audio processing callback
    static size_t callbackCount = 0;
    engine.setProcessCallback([&](pan::AudioBuffer& input, pan::AudioBuffer& output, size_t numFrames) {
        callbackCount++;
        if (callbackCount == 1) {
            std::cout << "Audio callback called! Processing " << numFrames << " frames" << std::endl;
        }
        
        // Generate sine wave
        for (size_t ch = 0; ch < output.getNumChannels(); ++ch) {
            float* channelData = output.getWritePointer(ch);
            double currentPhase = phase;
            
            for (size_t i = 0; i < numFrames; ++i) {
                channelData[i] = static_cast<float>(std::sin(currentPhase)) * 0.8f; // 80% volume (louder for testing)
                currentPhase += phaseIncrement;
                if (currentPhase >= 2.0 * M_PI) {
                    currentPhase -= 2.0 * M_PI;
                }
            }
        }
        
        // Update phase for next callback
        phase += phaseIncrement * numFrames;
        if (phase >= 2.0 * M_PI) {
            phase -= 2.0 * M_PI;
        }
    });
    
    // Start audio engine
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine" << std::endl;
        engine.shutdown();
        return 1;
    }
    
    std::cout << "Playing 440 Hz sine wave for 3 seconds..." << std::endl;
    std::cout << "Press Ctrl+C to stop early" << std::endl;
    
    // Play for 3 seconds - poll stream to ensure callbacks are processed
#ifdef PAN_USE_PORTAUDIO
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
        Pa_Sleep(10);  // Sleep in small increments
        // Check if stream is still active
        if (!engine.isRunning()) {
            std::cerr << "Stream stopped unexpectedly!" << std::endl;
            break;
        }
    }
#else
    std::this_thread::sleep_for(std::chrono::seconds(3));
#endif
    
    engine.stop();
    engine.shutdown();
    
    std::cout << "Done!" << std::endl;
    return 0;
}

