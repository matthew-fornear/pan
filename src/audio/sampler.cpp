#include "pan/audio/sampler.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>

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

bool Sampler::loadSample(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Read entire file
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
    // Remove .wav extension
    if (newSample->name.size() > 4 && newSample->name.substr(newSample->name.size() - 4) == ".wav") {
        newSample->name = newSample->name.substr(0, newSample->name.size() - 4);
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
    voice_.active = false;
    
    std::cout << "Sampler: Loaded sample '" << sample_->name << "' (" 
              << numSamples << " samples, " << channels << " ch, " 
              << fileSampleRate << " Hz, " << bitsPerSample << " bit)" << std::endl;
    
    return true;
}

void Sampler::noteOn(uint8_t note, uint8_t velocity) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!sample_ || sample_->dataL.empty()) return;
    
    // Calculate pitch shift based on note relative to root
    int semitones = note - sample_->rootNote;
    double pitchRatio = std::pow(2.0, semitones / 12.0);
    
    // Adjust for sample rate difference
    double srRatio = sample_->sampleRate / sampleRate_;
    
    voice_.active = true;
    voice_.position = 0.0;
    voice_.increment = pitchRatio * srRatio;
    voice_.velocity = velocity / 127.0f;
    voice_.note = note;
}

void Sampler::noteOff(uint8_t note) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (voice_.active && voice_.note == note) {
        voice_.active = false;
    }
}

void Sampler::allNotesOff() {
    std::lock_guard<std::mutex> lock(mutex_);
    voice_.active = false;
}

void Sampler::process(float* outL, float* outR, size_t numFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!sample_ || sample_->dataL.empty() || !voice_.active) {
        // Output silence
        for (size_t i = 0; i < numFrames; ++i) {
            outL[i] = 0.0f;
            outR[i] = 0.0f;
        }
        return;
    }
    
    for (size_t i = 0; i < numFrames; ++i) {
        if (!voice_.active || voice_.position >= sample_->dataL.size() - 1) {
            outL[i] = 0.0f;
            outR[i] = 0.0f;
            voice_.active = false;
            continue;
        }
        
        // Linear interpolation for pitch shifting
        size_t pos0 = static_cast<size_t>(voice_.position);
        size_t pos1 = pos0 + 1;
        float frac = static_cast<float>(voice_.position - pos0);
        
        float sampleL = sample_->dataL[pos0] * (1.0f - frac) + sample_->dataL[pos1] * frac;
        float sampleR = sampleL;
        if (sample_->stereo && !sample_->dataR.empty()) {
            sampleR = sample_->dataR[pos0] * (1.0f - frac) + sample_->dataR[pos1] * frac;
        }
        
        outL[i] = sampleL * voice_.velocity * volume_;
        outR[i] = sampleR * voice_.velocity * volume_;
        
        voice_.position += voice_.increment;
    }
}

} // namespace pan

