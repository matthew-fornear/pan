#include "pan/audio/audio_engine.h"
#include "pan/audio/audio_buffer.h"
#include "pan/audio/audio_device.h"
#include <iostream>
#include <thread>
#include <mutex>
#include <cmath>
#include <string>
#include <cstring>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

namespace pan {

class AudioEngine::Impl {
public:
    std::shared_ptr<AudioDevice> currentDevice;
    double sampleRate = 44100.0;
    size_t bufferSize = 512;
    bool running = false;
    ProcessCallback processCallback;
    std::mutex callbackMutex;  // Always available for thread safety
    
#ifdef PAN_USE_PORTAUDIO
    PaStream* stream = nullptr;
    std::unique_ptr<AudioBuffer> inputBuffer;
    std::unique_ptr<AudioBuffer> outputBuffer;
    
    Impl() : inputBuffer(nullptr), outputBuffer(nullptr) {}
    
    void initializeBuffers(size_t numChannels, size_t numFrames) {
        inputBuffer = std::make_unique<AudioBuffer>(numChannels, numFrames);
        outputBuffer = std::make_unique<AudioBuffer>(numChannels, numFrames);
    }
#else
    Impl() = default;
#endif
};

AudioEngine::AudioEngine() : pImpl(std::make_unique<Impl>()) {
}

AudioEngine::~AudioEngine() {
    shutdown();
}

bool AudioEngine::initialize() {
#ifdef PAN_USE_PORTAUDIO
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        std::cerr << "PortAudio initialization failed: " << Pa_GetErrorText(err) << std::endl;
        return false;
    }
    std::cout << "AudioEngine: Initialized with PortAudio" << std::endl;
    
    // Initialize buffers with maximum expected size (bufferSize might be adjusted by PortAudio)
    // Use a larger size to handle variable frame sizes in callbacks
    pImpl->initializeBuffers(2, pImpl->bufferSize * 2);
    
    return true;
#else
    std::cout << "AudioEngine: Initializing (no audio backend - PortAudio not found)" << std::endl;
    return true;
#endif
}

void AudioEngine::shutdown() {
    if (pImpl->running) {
        stop();
    }
    pImpl->currentDevice.reset();
    
#ifdef PAN_USE_PORTAUDIO
    // Only terminate if PortAudio was initialized
    // Check if PortAudio is initialized by trying to get device count
    int deviceCount = Pa_GetDeviceCount();
    if (deviceCount >= 0) {
        // PortAudio is initialized, so terminate it
        PaError err = Pa_Terminate();
        if (err != paNoError && err != paNotInitialized) {
            std::cerr << "PortAudio termination error: " << Pa_GetErrorText(err) << std::endl;
        }
    }
#endif
}

bool AudioEngine::setAudioDevice(std::shared_ptr<AudioDevice> device) {
    if (pImpl->running) {
        std::cerr << "Cannot change audio device while engine is running" << std::endl;
        return false;
    }
    pImpl->currentDevice = device;
    return true;
}

std::shared_ptr<AudioDevice> AudioEngine::getCurrentDevice() const {
    return pImpl->currentDevice;
}

#ifdef PAN_USE_PORTAUDIO
// PortAudio callback function (forward declaration)
extern "C" int audioCallback(const void* inputBuffer, void* outputBuffer,
                             unsigned long framesPerBuffer,
                             const PaStreamCallbackTimeInfo* timeInfo,
                             PaStreamCallbackFlags statusFlags,
                             void* userData);
#endif

bool AudioEngine::start() {
    if (pImpl->running) {
        return true;
    }
    
#ifdef PAN_USE_PORTAUDIO
    if (!pImpl->processCallback) {
        std::cerr << "Cannot start audio engine: no process callback set" << std::endl;
        return false;
    }
    
    // List available host APIs and prefer PulseAudio (which works with PipeWire)
    int numHostApis = Pa_GetHostApiCount();
    PaHostApiIndex pulseApiIndex = -1;
    PaHostApiIndex alsaApiIndex = -1;
    
    std::cout << "Available PortAudio host APIs:" << std::endl;
    PaHostApiIndex jackApiIndex = -1;
    for (int i = 0; i < numHostApis; ++i) {
        const PaHostApiInfo* apiInfo = Pa_GetHostApiInfo(i);
        if (apiInfo) {
            std::cout << "  " << i << ": " << apiInfo->name << std::endl;
            // Check by name since type constants may vary by PortAudio version
            std::string apiName = apiInfo->name;
            if (apiName.find("PulseAudio") != std::string::npos || 
                apiName.find("Pulse") != std::string::npos ||
                apiName.find("PipeWire") != std::string::npos) {
                pulseApiIndex = i;
            } else if (apiName.find("JACK") != std::string::npos) {
                jackApiIndex = i;
            } else if (apiName.find("ALSA") != std::string::npos) {
                alsaApiIndex = i;
            }
        }
    }
    
    // Use PulseAudio backend if available - it's designed to work with PipeWire
    // and won't interfere with system audio like direct ALSA can
    PaHostApiIndex preferredApi = -1;
    if (pulseApiIndex >= 0) {
        preferredApi = pulseApiIndex;
        std::cout << "Using PulseAudio API (compatible with PipeWire, won't interfere with system audio)" << std::endl;
    } else if (alsaApiIndex >= 0) {
        preferredApi = alsaApiIndex;
        std::cout << "Using ALSA API (fallback)" << std::endl;
    } else if (numHostApis > 0) {
        // Fallback to first available API
        preferredApi = 0;
    }
    
    if (preferredApi >= 0) {
        const PaHostApiInfo* apiInfo = Pa_GetHostApiInfo(preferredApi);
        std::cout << "Using host API: " << apiInfo->name << std::endl;
    } else {
        std::cerr << "No suitable host API found!" << std::endl;
        return false;
    }
    
    PaStreamParameters outputParameters;
    // Use the default device - PipeWire will handle routing
    // Don't try to select a specific device as that can interfere
    outputParameters.device = Pa_GetDefaultOutputDevice();
    if (outputParameters.device == paNoDevice) {
        std::cerr << "No default output device found" << std::endl;
        return false;
    }
    
    const PaDeviceInfo* deviceInfo = Pa_GetDeviceInfo(outputParameters.device);
    std::cout << "Output device: " << deviceInfo->name << std::endl;
    std::cout << "Max output channels: " << deviceInfo->maxOutputChannels << std::endl;
    std::cout << "Device default sample rate: " << deviceInfo->defaultSampleRate << std::endl;
    
    // Use the device's default sample rate (JACK uses 48000, ALSA often uses 44100)
    if (deviceInfo->defaultSampleRate > 0) {
        std::cout << "Setting sample rate from " << pImpl->sampleRate << " to " << deviceInfo->defaultSampleRate << std::endl;
        pImpl->sampleRate = deviceInfo->defaultSampleRate;  // Keep as double
        std::cout << "Sample rate is now: " << pImpl->sampleRate << " Hz" << std::endl;
    } else {
        std::cerr << "WARNING: Device default sample rate is invalid: " << deviceInfo->defaultSampleRate << std::endl;
    }
    
    outputParameters.channelCount = 2;
    outputParameters.sampleFormat = paFloat32;
    // Use default high latency - this is safer and won't interfere with system audio
    // Low latency can cause conflicts with PipeWire's buffer management
    outputParameters.suggestedLatency = deviceInfo->defaultHighOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = nullptr;
    
    // Open output-only stream (input is optional)
    PaStreamParameters* inputParameters = nullptr;
    PaStreamParameters inputParams;
    PaDeviceIndex inputDevice = paNoDevice;
    if (preferredApi >= 0) {
        const PaHostApiInfo* apiInfo = Pa_GetHostApiInfo(preferredApi);
        inputDevice = apiInfo->defaultInputDevice;
    } else {
        inputDevice = Pa_GetDefaultInputDevice();
    }
    
    if (inputDevice != paNoDevice) {
        inputParams.device = inputDevice;
        inputParams.channelCount = 2;
        inputParams.sampleFormat = paFloat32;
        inputParams.suggestedLatency = Pa_GetDeviceInfo(inputDevice)->defaultLowInputLatency;
        inputParams.hostApiSpecificStreamInfo = nullptr;
        inputParameters = &inputParams;
    }
    
    std::cout << "Opening stream with callback at: " << (void*)audioCallback << std::endl;
    // Use a reasonable buffer size that won't interfere with system audio
    // 512 frames is a good balance - not too small (causes underruns) not too large (interferes)
    unsigned long framesPerBuffer = 512;
    std::cout << "Using buffer size: " << framesPerBuffer << " frames" << std::endl;
    
    std::cout << "Opening stream with sample rate: " << pImpl->sampleRate << " Hz" << std::endl;
    
    PaError err = Pa_OpenStream(
        &pImpl->stream,
        nullptr,  // No input - output only
        &outputParameters,
        pImpl->sampleRate,
        framesPerBuffer,
        paClipOff,
        audioCallback,
        this
    );
    
    if (err != paNoError) {
        std::cerr << "Failed to open audio stream: " << Pa_GetErrorText(err) << std::endl;
        std::cerr << "Error code: " << err << std::endl;
        return false;
    }
    
    std::cout << "Stream opened successfully!" << std::endl;
    
    // Check what buffer size PortAudio actually chose
    const PaStreamInfo* streamInfo = Pa_GetStreamInfo(pImpl->stream);
    if (streamInfo) {
        std::cout << "Actual stream latency - input: " << streamInfo->inputLatency 
                  << "s, output: " << streamInfo->outputLatency << "s" << std::endl;
    }
    
    err = Pa_StartStream(pImpl->stream);
    if (err != paNoError) {
        std::cerr << "Failed to start audio stream: " << Pa_GetErrorText(err) << std::endl;
        Pa_CloseStream(pImpl->stream);
        pImpl->stream = nullptr;
        return false;
    }
    
    // Verify stream is actually running
    int isActive = Pa_IsStreamActive(pImpl->stream);
    if (isActive == 1) {
        std::cout << "AudioEngine: Stream is active" << std::endl;
    } else if (isActive == 0) {
        std::cerr << "Warning: Audio stream is not active after start!" << std::endl;
    } else {
        std::cerr << "Error checking stream status: " << Pa_GetErrorText(isActive) << std::endl;
    }
    
    // Check if stream is stopped
    int isStopped = Pa_IsStreamStopped(pImpl->stream);
    if (isStopped == 1) {
        std::cerr << "Warning: Stream reports as stopped!" << std::endl;
    }
    
    // Get stream info (already got it above, but get again to show final state)
    const PaStreamInfo* finalStreamInfo = Pa_GetStreamInfo(pImpl->stream);
    if (finalStreamInfo) {
        std::cout << "Stream latency - input: " << finalStreamInfo->inputLatency 
                  << "s, output: " << finalStreamInfo->outputLatency << "s" << std::endl;
    }
    
    pImpl->running = true;
    std::cout << "AudioEngine: Started (Sample Rate: " << pImpl->sampleRate 
              << ", Buffer Size: " << pImpl->bufferSize << ")" << std::endl;
    return true;
#else
    pImpl->running = true;
    std::cout << "AudioEngine: Started (no audio backend - PortAudio not found)" << std::endl;
    return true;
#endif
}

bool AudioEngine::stop() {
    if (!pImpl->running) {
        return true;
    }
    
#ifdef PAN_USE_PORTAUDIO
    if (pImpl->stream) {
        // Stop stream gracefully first (drains buffers)
        PaError err = Pa_StopStream(pImpl->stream);
        if (err != paNoError && err != paStreamIsStopped) {
            std::cerr << "Error stopping stream: " << Pa_GetErrorText(err) << std::endl;
        }
        
        // Wait for buffers to drain
        Pa_Sleep(50);
        
        // Now close the stream
        err = Pa_CloseStream(pImpl->stream);
        if (err != paNoError) {
            std::cerr << "Error closing stream: " << Pa_GetErrorText(err) << std::endl;
        }
        pImpl->stream = nullptr;
    }
#endif
    
    pImpl->running = false;
    std::cout << "AudioEngine: Stopped" << std::endl;
    return true;
}

bool AudioEngine::isRunning() const {
    return pImpl->running;
}

void AudioEngine::setSampleRate(double sampleRate) {
    if (pImpl->running) {
        std::cerr << "Cannot change sample rate while engine is running" << std::endl;
        return;
    }
    pImpl->sampleRate = sampleRate;
}

double AudioEngine::getSampleRate() const {
    return pImpl->sampleRate;
}

void AudioEngine::setBufferSize(size_t bufferSize) {
    if (pImpl->running) {
        std::cerr << "Cannot change buffer size while engine is running" << std::endl;
        return;
    }
    pImpl->bufferSize = bufferSize;
    
#ifdef PAN_USE_PORTAUDIO
    // Reinitialize buffers with new size if already initialized
    if (pImpl->inputBuffer) {
        pImpl->initializeBuffers(2, bufferSize);
    }
#endif
}

size_t AudioEngine::getBufferSize() const {
    return pImpl->bufferSize;
}

void AudioEngine::setProcessCallback(ProcessCallback callback) {
    std::lock_guard<std::mutex> lock(pImpl->callbackMutex);
    pImpl->processCallback = callback;
}

void AudioEngine::processAudioCallback(AudioBuffer& input, AudioBuffer& output, size_t numFrames) {
    std::lock_guard<std::mutex> lock(pImpl->callbackMutex);
    if (pImpl->processCallback) {
        pImpl->processCallback(input, output, numFrames);
    }
}

AudioBuffer* AudioEngine::getInputBuffer() {
#ifdef PAN_USE_PORTAUDIO
    return pImpl->inputBuffer.get();
#else
    return nullptr;
#endif
}

AudioBuffer* AudioEngine::getOutputBuffer() {
#ifdef PAN_USE_PORTAUDIO
    return pImpl->outputBuffer.get();
#else
    return nullptr;
#endif
}

void AudioEngine::resizeBuffersIfNeeded(size_t numChannels, size_t numFrames) {
#ifdef PAN_USE_PORTAUDIO
    if (!pImpl->outputBuffer || pImpl->outputBuffer->getNumFrames() < numFrames) {
        pImpl->initializeBuffers(numChannels, numFrames);
    }
#endif
}

} // namespace pan

#ifdef PAN_USE_PORTAUDIO
// PortAudio callback function implementation - MUST be in global namespace, not inside pan namespace!
extern "C" int audioCallback(const void* inputBuffer, void* outputBuffer,
                             unsigned long framesPerBuffer,
                             const PaStreamCallbackTimeInfo* timeInfo,
                             PaStreamCallbackFlags statusFlags,
                             void* userData) {
    // If no output buffer, return immediately
    if (!outputBuffer || framesPerBuffer == 0) {
        return paContinue;
    }
    
    // Get AudioEngine instance
    pan::AudioEngine* engine = static_cast<pan::AudioEngine*>(userData);
    if (!engine) {
        // Zero output buffer as fallback
        memset(outputBuffer, 0, framesPerBuffer * 2 * sizeof(float));
        return paContinue;
    }
    
    // Get input/output buffers
    pan::AudioBuffer* inputBuf = engine->getInputBuffer();
    pan::AudioBuffer* outputBuf = engine->getOutputBuffer();
    
    if (!outputBuf) {
        // Zero output buffer as fallback
        memset(outputBuffer, 0, framesPerBuffer * 2 * sizeof(float));
        return paContinue;
    }
    
    // Resize buffers if needed (PortAudio may use variable frame sizes)
    engine->resizeBuffersIfNeeded(2, framesPerBuffer);
    inputBuf = engine->getInputBuffer();
    outputBuf = engine->getOutputBuffer();
    
    // Copy input from PortAudio (if available) - interleaved format
    if (inputBuffer && inputBuf) {
        const float* in = static_cast<const float*>(inputBuffer);
        for (size_t ch = 0; ch < inputBuf->getNumChannels() && ch < 2; ++ch) {
            float* channelData = inputBuf->getWritePointer(ch);
            for (unsigned long i = 0; i < framesPerBuffer && i < inputBuf->getNumFrames(); ++i) {
                channelData[i] = in[i * 2 + ch];
            }
        }
    } else if (inputBuf) {
        inputBuf->clear();
    }
    
    // Clear output buffer
    outputBuf->clear();
    
    // Call user's process callback - this will generate the sine wave
    engine->processAudioCallback(*inputBuf, *outputBuf, framesPerBuffer);
    
    // Copy output to PortAudio buffer (interleaved format: L, R, L, R, ...)
    float* out = static_cast<float*>(outputBuffer);
    size_t numChannels = outputBuf->getNumChannels();
    
    // Ensure buffer is large enough
    if (outputBuf->getNumFrames() < framesPerBuffer) {
        memset(outputBuffer, 0, framesPerBuffer * 2 * sizeof(float));
        return paContinue;
    }
    
    // Copy from AudioBuffer (de-interleaved) to PortAudio buffer (interleaved)
    // PortAudio expects interleaved: [L0, R0, L1, R1, L2, R2, ...]
    for (unsigned long i = 0; i < framesPerBuffer; ++i) {
        if (numChannels >= 1) {
            const float* leftChannel = outputBuf->getReadPointer(0);
            out[i * 2 + 0] = leftChannel[i];  // Left channel
        } else {
            out[i * 2 + 0] = 0.0f;
        }
        if (numChannels >= 2) {
            const float* rightChannel = outputBuf->getReadPointer(1);
            out[i * 2 + 1] = rightChannel[i];  // Right channel
        } else {
            out[i * 2 + 1] = 0.0f;
        }
    }
    
    return paContinue;
}
#endif

