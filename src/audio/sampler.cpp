#include "pan/audio/sampler.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>

// MP3 decoding support
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "minimp3_ex.h"

namespace pan {

void Sample::generateWaveformDisplay() {
    waveformDisplay.clear();
    if (dataL.empty()) return;
    
    // Generate ~512 points for display
    const size_t displayPoints = 512;
    waveformDisplay.resize(displayPoints);
    
    size_t samplesPerPoint = std::max(size_t(1), dataL.size() / displayPoints);
    
    for (size_t i = 0; i < displayPoints; ++i) {
        size_t startSample = i * samplesPerPoint;
        size_t endSample = std::min(startSample + samplesPerPoint, dataL.size());
        
        float maxVal = 0.0f;
        for (size_t j = startSample; j < endSample; ++j) {
            float val = std::abs(dataL[j]);
            if (stereo && j < dataR.size()) {
                val = std::max(val, std::abs(dataR[j]));
            }
            maxVal = std::max(maxVal, val);
        }
        waveformDisplay[i] = maxVal;
    }
}

Sampler::Sampler(double sampleRate)
    : sampleRate_(sampleRate)
{
    // Initialize all voices
    for (auto& voice : voices_) {
        voice.active = false;
        voice.envStage = Voice::EnvStage::Off;
    }
}

double Sampler::getSampleDuration() const {
    if (!sample_ || sample_->dataL.empty()) return 0.0;
    return static_cast<double>(sample_->dataL.size()) / sample_->sampleRate;
}

size_t Sampler::getSampleFrames() const {
    if (!sample_) return 0;
    return sample_->dataL.size();
}

double Sampler::getSampleRate() const {
    if (!sample_) return sampleRate_;
    return sample_->sampleRate;
}

int Sampler::findFreeVoice() {
    // First, look for an inactive voice
    for (int i = 0; i < params_.voices && i < MAX_VOICES; ++i) {
        if (!voices_[i].active) return i;
    }
    // If all voices are active, steal the oldest one (voice 0)
    return 0;
}

float Sampler::processEnvelope(Voice& voice, double deltaTime) {
    float targetLevel = 0.0f;
    float rate = 0.0f;
    
    switch (voice.envStage) {
        case Voice::EnvStage::Attack:
            targetLevel = 1.0f;
            rate = params_.attack > 0.001f ? (1.0f / params_.attack) : 1000.0f;
            voice.envLevel += rate * static_cast<float>(deltaTime);
            if (voice.envLevel >= 1.0f) {
                voice.envLevel = 1.0f;
                voice.envStage = Voice::EnvStage::Decay;
            }
            break;
            
        case Voice::EnvStage::Decay:
            targetLevel = params_.sustain;
            rate = params_.decay > 0.001f ? ((1.0f - params_.sustain) / params_.decay) : 1000.0f;
            voice.envLevel -= rate * static_cast<float>(deltaTime);
            if (voice.envLevel <= params_.sustain) {
                voice.envLevel = params_.sustain;
                voice.envStage = Voice::EnvStage::Sustain;
            }
            break;
            
        case Voice::EnvStage::Sustain:
            voice.envLevel = params_.sustain;
            break;
            
        case Voice::EnvStage::Release:
            rate = params_.release > 0.001f ? (voice.envLevel / params_.release) : 1000.0f;
            voice.envLevel -= rate * static_cast<float>(deltaTime);
            if (voice.envLevel <= 0.0f) {
                voice.envLevel = 0.0f;
                voice.envStage = Voice::EnvStage::Off;
                voice.active = false;
            }
            break;
            
        case Voice::EnvStage::Off:
            voice.envLevel = 0.0f;
            break;
    }
    
    return std::max(0.0f, std::min(1.0f, voice.envLevel));
}

float Sampler::calculateLFO() {
    if (!params_.lfoEnabled) return 0.0f;
    
    float lfoValue = 0.0f;
    double phase = lfoPhase_;
    
    switch (params_.lfoWaveform) {
        case 0: // Sine
            lfoValue = std::sin(phase * 2.0 * M_PI);
            break;
        case 1: // Triangle
            lfoValue = 1.0f - 4.0f * std::abs(std::fmod(phase + 0.25, 1.0) - 0.5f);
            break;
        case 2: // Saw
            lfoValue = 2.0f * static_cast<float>(phase) - 1.0f;
            break;
        case 3: // Square
            lfoValue = phase < 0.5 ? 1.0f : -1.0f;
            break;
        case 4: // Random (sample & hold)
            lfoValue = static_cast<float>(rand()) / RAND_MAX * 2.0f - 1.0f;
            break;
    }
    
    return lfoValue * params_.lfoAmount;
}

bool Sampler::parseWavHeader(const std::vector<uint8_t>& data, int& channels, int& sampleRate, 
                             int& bitsPerSample, size_t& dataOffset, size_t& dataSize) {
    if (data.size() < 44) return false;
    
    // Check RIFF header
    if (memcmp(data.data(), "RIFF", 4) != 0) return false;
    if (memcmp(data.data() + 8, "WAVE", 4) != 0) return false;
    
    // Find fmt chunk
    size_t pos = 12;
    while (pos + 8 < data.size()) {
        uint32_t chunkSize = *reinterpret_cast<const uint32_t*>(data.data() + pos + 4);
        
        if (memcmp(data.data() + pos, "fmt ", 4) == 0) {
            if (pos + 24 > data.size()) return false;
            
            uint16_t audioFormat = *reinterpret_cast<const uint16_t*>(data.data() + pos + 8);
            if (audioFormat != 1 && audioFormat != 3) {
                // Only PCM (1) or IEEE float (3) supported
                return false;
            }
            
            channels = *reinterpret_cast<const uint16_t*>(data.data() + pos + 10);
            sampleRate = *reinterpret_cast<const uint32_t*>(data.data() + pos + 12);
            bitsPerSample = *reinterpret_cast<const uint16_t*>(data.data() + pos + 22);
            
            pos += 8 + chunkSize;
        } else if (memcmp(data.data() + pos, "data", 4) == 0) {
            dataOffset = pos + 8;
            dataSize = chunkSize;
            return true;
        } else {
            pos += 8 + chunkSize;
        }
        
        // Ensure chunk is word-aligned
        if (chunkSize % 2 != 0) pos++;
    }
    
    return false;
}

bool Sampler::loadMp3(const std::string& path) {
    mp3dec_t mp3d;
    mp3dec_file_info_t info;
    
    if (mp3dec_load(&mp3d, path.c_str(), &info, NULL, NULL)) {
        std::cerr << "Sampler: Failed to decode MP3: " << path << std::endl;
        return false;
    }
    
    if (info.samples == 0) {
        std::cerr << "Sampler: MP3 has no samples: " << path << std::endl;
        free(info.buffer);
        return false;
    }
    
    // Create new sample
    auto newSample = std::make_unique<Sample>();
    newSample->sampleRate = info.hz;
    newSample->stereo = (info.channels == 2);
    newSample->filePath = path;
    
    // Extract filename as name
    size_t lastSlash = path.find_last_of("/\\");
    newSample->name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    // Remove extension
    size_t dotPos = newSample->name.rfind('.');
    if (dotPos != std::string::npos) {
        newSample->name = newSample->name.substr(0, dotPos);
    }
    
    // Convert to float format
    size_t numFrames = info.samples / info.channels;
    newSample->dataL.resize(numFrames);
    if (info.channels == 2) {
        newSample->dataR.resize(numFrames);
    }
    
    // minimp3 outputs interleaved int16_t samples
    for (size_t i = 0; i < numFrames; ++i) {
        newSample->dataL[i] = info.buffer[i * info.channels] / 32768.0f;
        if (info.channels == 2) {
            newSample->dataR[i] = info.buffer[i * info.channels + 1] / 32768.0f;
        }
    }
    
    free(info.buffer);
    
    // Generate waveform display
    newSample->generateWaveformDisplay();
    
    sample_ = std::move(newSample);
    // Reset all voices
    for (auto& voice : voices_) {
        voice.active = false;
        voice.envStage = Voice::EnvStage::Off;
    }
    
    std::cout << "Sampler: Loaded MP3 '" << sample_->name << "' (" 
              << numFrames << " frames, " << info.channels << " ch, " 
              << info.hz << " Hz)" << std::endl;
    
    return true;
}

bool Sampler::loadSample(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Detect file type by extension
    std::string ext;
    size_t dotPos = path.rfind('.');
    if (dotPos != std::string::npos) {
        ext = path.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }
    
    // Handle MP3 files
    if (ext == ".mp3") {
        return loadMp3(path);
    }
    
    // Read entire file for WAV
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Sampler: Cannot open file: " << path << std::endl;
        return false;
    }
    
    size_t fileSize = file.tellg();
    file.seekg(0);
    
    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();
    
    // Parse WAV header
    int channels, fileSampleRate, bitsPerSample;
    size_t dataOffset, dataSize;
    
    if (!parseWavHeader(data, channels, fileSampleRate, bitsPerSample, dataOffset, dataSize)) {
        std::cerr << "Sampler: Invalid WAV file: " << path << std::endl;
        return false;
    }
    
    // Create new sample
    auto newSample = std::make_unique<Sample>();
    newSample->sampleRate = fileSampleRate;
    newSample->stereo = (channels == 2);
    newSample->filePath = path;
    
    // Extract filename as name
    size_t lastSlash = path.find_last_of("/\\");
    newSample->name = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
    // Remove extension
    {
        size_t extDotPos = newSample->name.rfind('.');
        if (extDotPos != std::string::npos) {
            newSample->name = newSample->name.substr(0, extDotPos);
        }
    }
    
    // Convert audio data
    size_t numSamples = dataSize / (channels * (bitsPerSample / 8));
    newSample->dataL.resize(numSamples);
    if (channels == 2) {
        newSample->dataR.resize(numSamples);
    }
    
    const uint8_t* audioData = data.data() + dataOffset;
    
    for (size_t i = 0; i < numSamples; ++i) {
        if (bitsPerSample == 16) {
            int16_t sampleL = *reinterpret_cast<const int16_t*>(audioData + i * channels * 2);
            newSample->dataL[i] = sampleL / 32768.0f;
            
            if (channels == 2) {
                int16_t sampleR = *reinterpret_cast<const int16_t*>(audioData + i * channels * 2 + 2);
                newSample->dataR[i] = sampleR / 32768.0f;
            }
        } else if (bitsPerSample == 24) {
            // 24-bit samples
            int32_t sampleL = (audioData[i * channels * 3] | 
                              (audioData[i * channels * 3 + 1] << 8) |
                              (audioData[i * channels * 3 + 2] << 16));
            if (sampleL & 0x800000) sampleL |= 0xFF000000;  // Sign extend
            newSample->dataL[i] = sampleL / 8388608.0f;
            
            if (channels == 2) {
                int32_t sampleR = (audioData[i * channels * 3 + 3] |
                                  (audioData[i * channels * 3 + 4] << 8) |
                                  (audioData[i * channels * 3 + 5] << 16));
                if (sampleR & 0x800000) sampleR |= 0xFF000000;
                newSample->dataR[i] = sampleR / 8388608.0f;
            }
        } else if (bitsPerSample == 32) {
            // Assume 32-bit float
            float sampleL = *reinterpret_cast<const float*>(audioData + i * channels * 4);
            newSample->dataL[i] = sampleL;
            
            if (channels == 2) {
                float sampleR = *reinterpret_cast<const float*>(audioData + i * channels * 4 + 4);
                newSample->dataR[i] = sampleR;
            }
        }
    }
    
    // Generate waveform display
    newSample->generateWaveformDisplay();
    
    sample_ = std::move(newSample);
    // Reset all voices
    for (auto& voice : voices_) {
        voice.active = false;
        voice.envStage = Voice::EnvStage::Off;
    }
    
    std::cout << "Sampler: Loaded sample '" << sample_->name << "' (" 
              << numSamples << " samples, " << channels << " ch, " 
              << fileSampleRate << " Hz, " << bitsPerSample << " bit)" << std::endl;
    
    return true;
}

void Sampler::noteOn(uint8_t note, uint8_t velocity) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sample_ || sample_->dataL.empty()) return;
    
    // Find a free voice
    int voiceIdx = findFreeVoice();
    Voice& voice = voices_[voiceIdx];
    
    // Calculate pitch shift based on note relative to root
    int semitones = note - sample_->rootNote + params_.transpose;
    double detuneCents = params_.detune;
    double pitchRatio = std::pow(2.0, (semitones + detuneCents / 100.0) / 12.0);
    
    // Adjust for sample rate difference
    double srRatio = sample_->sampleRate / sampleRate_;
    
    // Determine slice boundaries
    size_t startSample = 0;
    size_t endSample = sample_->dataL.size();
    if (params_.mode == SamplerMode::Slice && !params_.sliceMarkers.empty()) {
        // Build boundaries [0, markers...,1]
        std::vector<float> boundaries;
        boundaries.reserve(params_.sliceMarkers.size() + 2);
        boundaries.push_back(0.0f);
        for (float m : params_.sliceMarkers) boundaries.push_back(std::clamp(m, 0.0f, 1.0f));
        boundaries.push_back(1.0f);
        std::sort(boundaries.begin(), boundaries.end());
        size_t numSlices = boundaries.size() - 1;
        size_t idx = std::min<size_t>(numSlices - 1, static_cast<size_t>(std::max<int>(0, note - sample_->rootNote)));
        float s = boundaries[idx];
        float e = boundaries[idx + 1];
        startSample = static_cast<size_t>(s * sample_->dataL.size());
        endSample = static_cast<size_t>(e * sample_->dataL.size());
        endSample = std::max(endSample, startSample + 1);
    } else {
        startSample = static_cast<size_t>(params_.startPos * sample_->dataL.size());
        endSample = static_cast<size_t>((params_.startPos + params_.length) * sample_->dataL.size());
        endSample = std::min(endSample, sample_->dataL.size());
        endSample = std::max(endSample, startSample + 1);
    }
    
    voice.active = true;
    voice.position = startSample;
    voice.increment = pitchRatio * srRatio;
    voice.velocity = velocity / 127.0f;
    voice.note = note;
    voice.releasing = false;
    voice.startSample = startSample;
    voice.endSample = endSample;
    voice.loopStartSample = params_.loopEnabled
        ? startSample + static_cast<size_t>(params_.loopStart * (endSample - startSample))
        : startSample;
    
    // Start envelope
    voice.envStage = Voice::EnvStage::Attack;
    voice.envLevel = 0.0f;
    voice.envTime = 0.0;
    
    activeVoiceCount_ = std::min(activeVoiceCount_ + 1, params_.voices);
}

void Sampler::noteOff(uint8_t note) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // One-shot mode ignores note off
    if (params_.mode == SamplerMode::OneShot) return;
    
    // Find the voice playing this note and release it
    for (auto& voice : voices_) {
        if (voice.active && voice.note == note && voice.envStage != Voice::EnvStage::Release) {
            voice.envStage = Voice::EnvStage::Release;
            voice.releasing = true;
        }
    }
}

void Sampler::allNotesOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& voice : voices_) {
        if (voice.active) {
            voice.envStage = Voice::EnvStage::Release;
            voice.releasing = true;
        }
    }
}

void Sampler::process(float* outL, float* outR, size_t numFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Clear output
    for (size_t i = 0; i < numFrames; ++i) {
        outL[i] = 0.0f;
        outR[i] = 0.0f;
    }
    
    if (!sample_ || sample_->dataL.empty()) return;
    
    // Calculate volume in linear
    float volumeLinear = std::pow(10.0f, params_.volume / 20.0f);
    float gainLinear = std::pow(10.0f, params_.gain / 20.0f);
    float totalGain = volumeLinear * gainLinear;
    
    // Update LFO phase
    double lfoIncrement = params_.lfoRate / sampleRate_;
    
    // Calculate sample boundaries per voice
    size_t sampleLength = sample_->dataL.size();
    
    // Process each active voice
    for (auto& voice : voices_) {
        if (!voice.active) continue;
        
        double deltaTime = 1.0 / sampleRate_;
        
        for (size_t i = 0; i < numFrames; ++i) {
            // Process envelope
            float envLevel = processEnvelope(voice, deltaTime);
            if (!voice.active) break;
            
            // Check if we've reached the end
            size_t pos = static_cast<size_t>(voice.position);
            size_t vEnd = voice.endSample > 0 ? std::min(voice.endSample, sampleLength) : sampleLength;
            size_t vLoopStart = params_.loopEnabled ? voice.loopStartSample : voice.startSample;
            if (pos >= vEnd) {
                if (params_.loopEnabled && params_.mode == SamplerMode::Classic) {
                    voice.position = static_cast<double>(vLoopStart);
                    pos = vLoopStart;
                } else {
                    // End of sample
                    if (params_.mode == SamplerMode::OneShot) {
                        voice.active = false;
                    } else {
                        voice.envStage = Voice::EnvStage::Release;
                    }
                    continue;
                }
            }
            
            // Linear interpolation for pitch shifting
            size_t pos0 = pos;
            size_t pos1 = std::min(pos0 + 1, sampleLength - 1);
            float frac = static_cast<float>(voice.position - pos0);
            
            float sampleL = sample_->dataL[pos0] * (1.0f - frac) + sample_->dataL[pos1] * frac;
            float sampleR = sampleL;
            if (sample_->stereo && !sample_->dataR.empty()) {
                sampleR = sample_->dataR[pos0] * (1.0f - frac) + sample_->dataR[pos1] * frac;
            }
            
            // Apply LFO to volume if targeted
            float lfoMod = 1.0f;
            if (params_.lfoEnabled && params_.lfoTarget == 3) {
                lfoMod = 1.0f + calculateLFO() * 0.5f;
            }
            
            // Apply envelope and velocity
            float amp = envLevel * voice.velocity * totalGain * lfoMod;
            
            // Apply pan
            float panL = 1.0f, panR = 1.0f;
            if (params_.pan < 0) {
                panR = 1.0f + params_.pan;
            } else if (params_.pan > 0) {
                panL = 1.0f - params_.pan;
            }
            
            outL[i] += sampleL * amp * panL;
            outR[i] += sampleR * amp * panR;
            
            // Advance position
            voice.position += voice.increment;
            
            // Update LFO phase
            lfoPhase_ += lfoIncrement;
            if (lfoPhase_ >= 1.0) lfoPhase_ -= 1.0;
        }
    }
    
    // Count active voices
    activeVoiceCount_ = 0;
    for (const auto& voice : voices_) {
        if (voice.active) activeVoiceCount_++;
    }
}

} // namespace pan

