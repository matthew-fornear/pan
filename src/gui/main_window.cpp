#include "pan/gui/main_window.h"
#include "pan/audio/reverb.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <climits>
#include <unistd.h>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>
#include <utility>
#include <filesystem>
#include <sys/stat.h>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

#ifdef PAN_USE_GUI
// ImGui includes - must define IMGUI_DEFINE_MATH_OPERATORS before imgui.h
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#endif

namespace pan {

Track::Track() 
    : isRecording(false)
    , name("")
    , waveformSet(false)
    , waveformBuffer(WAVEFORM_BUFFER_SIZE, 0.0f)
    , waveformBufferWritePos(0)
    , waveformBufferMutex(std::make_unique<std::mutex>())
{
    // Initialize with one default sine oscillator
    oscillators.push_back(Oscillator(Waveform::Sine, 1.0f, 1.0f));
}

void Track::addWaveformSample(float sample) {
    if (waveformBufferMutex) {
        std::lock_guard<std::mutex> lock(*waveformBufferMutex);
        waveformBuffer[waveformBufferWritePos] = sample;
        waveformBufferWritePos = (waveformBufferWritePos + 1) % WAVEFORM_BUFFER_SIZE;
    }
}

std::vector<float> Track::getWaveformSamples() const {
    if (!waveformBufferMutex) {
        return std::vector<float>(WAVEFORM_BUFFER_SIZE, 0.0f);
    }
    
    std::lock_guard<std::mutex> lock(*waveformBufferMutex);
    std::vector<float> result(WAVEFORM_BUFFER_SIZE);
    
    // Return samples in chronological order (from oldest to newest)
    for (size_t i = 0; i < WAVEFORM_BUFFER_SIZE; ++i) {
        size_t idx = (waveformBufferWritePos + i) % WAVEFORM_BUFFER_SIZE;
        result[i] = waveformBuffer[idx];
    }
    
    return result;
}

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

MainWindow::MainWindow()
    : window_(nullptr)
    , shouldQuit_(false)
    , bpm_(120.0f)
    , masterRecord_(false)
    , timelinePosition_(0.0f)
    , timelineScrollX_(0.0f)
    , isPlaying_(false)
    , lastTime_(0.0)
    , isDraggingPlayhead_(false)
    , dragStartBeat_(0.0f)
    , playbackSamplePosition_(0)
    , selectedTrackIndex_(0)
    , currentProjectPath_("")
    , hasUnsavedChanges_(false)
    , triggerSaveAsDialog_(false)
    , fileBrowserPath_(std::filesystem::current_path().string())
    , countInEnabled_(true)
    , isCountingIn_(false)
    , countInBeatsRemaining_(0)
    , countInLastBeatTime_(0.0)
    , pianoRollActive_(false)
    , pencilToolActive_(false)
    , gridSnapEnabled_(true)
    , currentGridDivision_(GridDivision::Sixteenth)
    , pianoRollScrollY_(0.0f)
    , pianoRollHoverNote_(-1)
    , showContextMenu_(false)
    , pianoRollCenterNote_(60)  // C4
    , pianoRollAutoPositioned_(false)
    , folderIconTexture_(nullptr)
    , drawIconTexture_(nullptr)
    , folderIconWidth_(0)
    , folderIconHeight_(0)
    , drawIconWidth_(0)
    , drawIconHeight_(0)
    , drawIconTipOffsetX_(0)
    , drawIconTipOffsetY_(0)
{
    draggedNote_.isDragging = false;
    draggedNote_.clipIndex = 0;
    draggedNote_.eventIndex = 0;
    draggedNote_.startBeat = 0.0f;
    draggedNote_.startNote = 0;
    draggedNote_.noteDuration = 0.0f;
    
    boxSelection_.isSelecting = false;
    boxSelection_.startX = 0.0f;
    boxSelection_.startY = 0.0f;
    boxSelection_.currentX = 0.0f;
    boxSelection_.currentY = 0.0f;
    
    drawingNote_.isDrawing = false;
    drawingNote_.startBeat = 0.0f;
    drawingNote_.noteNum = 60;
    drawingNote_.clipIndex = 0;
    
    resizingNote_.isResizing = false;
    resizingNote_.isLeftEdge = false;
    resizingNote_.clipIndex = 0;
    resizingNote_.eventIndex = 0;
    resizingNote_.originalStartBeat = 0.0f;
    resizingNote_.originalEndBeat = 0.0f;
    resizingNote_.noteNum = 0;
    
    // Initialize notes playing tracker
    notesPlaying_.fill(false);
    
    // Initialize common directories for file browser
    initializeCommonDirectories();
}

MainWindow::~MainWindow() {
    shutdown();
}

bool MainWindow::initialize() {
#ifdef PAN_USE_GUI
    // Setup GLFW
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return false;
    }
    
    // GL 3.3 + GLSL 330
    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Get primary monitor for fullscreen
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);
    
    // Create window - use monitor resolution
    GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, "Pan Synthesizer", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }
    
    window_ = window;
    
    // Load and set window icon
    // Try multiple paths: current directory, executable directory, or project root
    GLFWimage images[1];
    int width, height, channels;
    unsigned char* pixels = nullptr;
    const char* iconPath = nullptr;
    
    // Try current directory first
    pixels = stbi_load("betaicon.png", &width, &height, &channels, 0);  // Load as-is (let stb decide format)
    if (pixels) {
        iconPath = "betaicon.png (current dir)";
        // Convert RGB to RGBA if needed
        if (channels == 3) {
            unsigned char* rgbaPixels = (unsigned char*)malloc(width * height * 4);
            for (int i = 0; i < width * height; ++i) {
                rgbaPixels[i * 4 + 0] = pixels[i * 3 + 0];  // R
                rgbaPixels[i * 4 + 1] = pixels[i * 3 + 1];  // G
                rgbaPixels[i * 4 + 2] = pixels[i * 3 + 2];  // B
                rgbaPixels[i * 4 + 3] = 255;                // A (opaque)
            }
            stbi_image_free(pixels);
            pixels = rgbaPixels;
            channels = 4;
        }
    } else {
        // Try executable directory
        char exePath[PATH_MAX];
        if (readlink("/proc/self/exe", exePath, PATH_MAX) != -1) {
            std::string exeDir = exePath;
            size_t lastSlash = exeDir.find_last_of('/');
            if (lastSlash != std::string::npos) {
                exeDir = exeDir.substr(0, lastSlash + 1);
                std::string iconFile = exeDir + "betaicon.png";
                pixels = stbi_load(iconFile.c_str(), &width, &height, &channels, 0);
                if (pixels) {
                    iconPath = iconFile.c_str();
                    // Convert RGB to RGBA if needed
                    if (channels == 3) {
                        unsigned char* rgbaPixels = (unsigned char*)malloc(width * height * 4);
                        for (int i = 0; i < width * height; ++i) {
                            rgbaPixels[i * 4 + 0] = pixels[i * 3 + 0];
                            rgbaPixels[i * 4 + 1] = pixels[i * 3 + 1];
                            rgbaPixels[i * 4 + 2] = pixels[i * 3 + 2];
                            rgbaPixels[i * 4 + 3] = 255;
                        }
                        stbi_image_free(pixels);
                        pixels = rgbaPixels;
                        channels = 4;
                    }
                }
            }
        }
    }
    if (!pixels) {
        // Try project root (parent of build directory)
        pixels = stbi_load("../betaicon.png", &width, &height, &channels, 0);
        if (pixels) {
            iconPath = "../betaicon.png";
            // Convert RGB to RGBA if needed
            if (channels == 3) {
                unsigned char* rgbaPixels = (unsigned char*)malloc(width * height * 4);
                for (int i = 0; i < width * height; ++i) {
                    rgbaPixels[i * 4 + 0] = pixels[i * 3 + 0];
                    rgbaPixels[i * 4 + 1] = pixels[i * 3 + 1];
                    rgbaPixels[i * 4 + 2] = pixels[i * 3 + 2];
                    rgbaPixels[i * 4 + 3] = 255;
                }
                stbi_image_free(pixels);
                pixels = rgbaPixels;
                channels = 4;
            }
        }
    }
    
    if (pixels) {
        std::cout << "Loaded icon from: " << iconPath << " (size: " << width << "x" << height << ", channels: " << channels << ")" << std::endl;
        images[0].width = width;
        images[0].height = height;
        images[0].pixels = pixels;
        
        // Set window icon - this affects window decoration
        // Note: On Linux, the launcher/taskbar icon typically comes from a .desktop file,
        // but this will at least set the window decoration icon
        glfwSetWindowIcon(window, 1, images);
        std::cout << "Window icon set via GLFW" << std::endl;
        
        // Free pixels (use free() if we allocated RGBA, otherwise stbi_image_free)
        if (channels == 4 && iconPath) {
            free(pixels);  // We allocated this with malloc()
        } else {
            stbi_image_free(pixels);
        }
    } else {
        std::cerr << "Warning: Could not load betaicon.png" << std::endl;
        std::cerr << "  Tried paths: current dir, executable dir, and ../" << std::endl;
        char cwd[PATH_MAX];
        if (getcwd(cwd, PATH_MAX) != nullptr) {
            std::cerr << "  Current working directory: " << cwd << std::endl;
        }
        char exePath[PATH_MAX];
        if (readlink("/proc/self/exe", exePath, PATH_MAX) != -1) {
            std::cerr << "  Executable path: " << exePath << std::endl;
        }
    }
    
    // Center window
    glfwSetWindowPos(window, 0, 0);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync
    
    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    
    // Setup Dear ImGui style with custom colors: slate grey, light grey, and ivory
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Color definitions
    ImVec4 slateGrey(0.18f, 0.20f, 0.22f, 1.0f);      // #2E3338 - dark slate grey
    ImVec4 lightGrey(0.35f, 0.37f, 0.39f, 1.0f);       // #595E63 - light grey
    ImVec4 ivory(0.98f, 0.98f, 0.94f, 1.0f);          // #FAFAF0 - ivory (for text)
    ImVec4 darkSlate(0.12f, 0.14f, 0.16f, 1.0f);      // #1F2326 - darker slate for backgrounds
    
    // Window colors
    style.Colors[ImGuiCol_WindowBg] = slateGrey;
    style.Colors[ImGuiCol_ChildBg] = darkSlate;
    style.Colors[ImGuiCol_PopupBg] = slateGrey;
    style.Colors[ImGuiCol_Border] = lightGrey;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    
    // Text colors - all ivory
    style.Colors[ImGuiCol_Text] = ivory;
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.5f, 0.5f, 0.47f, 1.0f);
    
    // Frame/button colors
    style.Colors[ImGuiCol_FrameBg] = darkSlate;
    style.Colors[ImGuiCol_FrameBgHovered] = lightGrey;
    style.Colors[ImGuiCol_FrameBgActive] = lightGrey;
    
    // Button colors
    style.Colors[ImGuiCol_Button] = lightGrey;
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.47f, 0.49f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.27f, 0.29f, 1.0f);
    
    // Header colors
    style.Colors[ImGuiCol_Header] = lightGrey;
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.47f, 0.49f, 1.0f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.27f, 0.29f, 1.0f);
    
    // Title bar
    style.Colors[ImGuiCol_TitleBg] = darkSlate;
    style.Colors[ImGuiCol_TitleBgActive] = lightGrey;
    style.Colors[ImGuiCol_TitleBgCollapsed] = darkSlate;
    
    // Scrollbar
    style.Colors[ImGuiCol_ScrollbarBg] = darkSlate;
    style.Colors[ImGuiCol_ScrollbarGrab] = lightGrey;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.45f, 0.47f, 0.49f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.25f, 0.27f, 0.29f, 1.0f);
    
    // Slider
    style.Colors[ImGuiCol_SliderGrab] = lightGrey;
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.25f, 0.27f, 0.29f, 1.0f);
    
    // Checkbox
    style.Colors[ImGuiCol_CheckMark] = ivory;
    
    // Combo/Dropdown
    style.Colors[ImGuiCol_PopupBg] = slateGrey;
    
    // Separator
    style.Colors[ImGuiCol_Separator] = lightGrey;
    style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.45f, 0.47f, 0.49f, 1.0f);
    style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.25f, 0.27f, 0.29f, 1.0f);
    
    // Resize grip
    style.Colors[ImGuiCol_ResizeGrip] = lightGrey;
    style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.45f, 0.47f, 0.49f, 1.0f);
    style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(0.25f, 0.27f, 0.29f, 1.0f);
    
    // Tab
    style.Colors[ImGuiCol_Tab] = darkSlate;
    style.Colors[ImGuiCol_TabHovered] = lightGrey;
    style.Colors[ImGuiCol_TabActive] = lightGrey;
    style.Colors[ImGuiCol_TabUnfocused] = darkSlate;
    style.Colors[ImGuiCol_TabUnfocusedActive] = slateGrey;
    
    // Docking
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(lightGrey.x, lightGrey.y, lightGrey.z, 0.3f);
    style.Colors[ImGuiCol_DockingEmptyBg] = darkSlate;
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    // Load SVG icons after OpenGL context is created
    loadSVGIcons();
    
    // Initialize audio and MIDI
    if (!initializeAudio()) {
        return false;
    }
    
    if (!initializeMIDI()) {
        std::cout << "Warning: No MIDI devices found, continuing without MIDI" << std::endl;
    }
    
    return true;
#else
    std::cerr << "GUI support not compiled. Install GLFW3 and OpenGL." << std::endl;
    return false;
#endif
}

bool MainWindow::initializeAudio() {
    engine_ = std::make_shared<AudioEngine>();
    if (!engine_->initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        return false;
    }
    
    // Create 1 track initially, auto-select it
    tracks_.resize(1);
    for (auto& track : tracks_) {
        track.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
        track.synth->setVolume(0.5f);
        // Sync synthesizer oscillators with track oscillators
        track.synth->setOscillators(track.oscillators);
        // Auto-select first track
        track.isRecording = true;
    }
    
    // Set up audio processing - mix all recording tracks and play back clips
    engine_->setProcessCallback([this](AudioBuffer& input, AudioBuffer& output, size_t numFrames) {
        output.clear();
        
        // Handle count-in clicks
        if (isCountingIn_) {
            double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
            double samplesPerBeat = (60.0 / bpm_) * sampleRate;
            
            // Check if we need to generate a click this buffer
            auto now = std::chrono::steady_clock::now();
            auto duration = now.time_since_epoch();
            double currentTime = std::chrono::duration<double>(duration).count();
            
            // Check if it's time for the next click
            if (countInLastBeatTime_ == 0.0) {
                // First click
                countInLastBeatTime_ = currentTime;
                generateClickSound(output, numFrames, true);  // Accent first beat
                countInBeatsRemaining_--;
            } else if (currentTime - countInLastBeatTime_ >= (60.0 / bpm_)) {
                // Time for next click
                countInLastBeatTime_ = currentTime;
                generateClickSound(output, numFrames, countInBeatsRemaining_ == 4);  // Accent if first beat
                countInBeatsRemaining_--;
                
                // If count-in complete, start actual playback
                if (countInBeatsRemaining_ <= 0) {
                    isCountingIn_ = false;
                    isPlaying_ = true;
                    lastTime_ = 0.0;  // Reset for timeline update
                }
            }
            
            return;  // Don't play tracks during count-in
        }
        
        // Get current playback position for MIDI clip playback
        int64_t currentPlaybackPos = playbackSamplePosition_;
        
        // If playing, trigger MIDI events from clips
        if (isPlaying_) {
            for (auto& track : tracks_) {
                if (track.synth) {
                    // Play back all completed clips on this track
                    for (const auto& clip : track.clips) {
                        const auto& events = clip->getEvents();
                        int64_t clipStartSample = clip->getStartTime();
                        
                        // Check which events fall in this audio buffer time window
                        for (const auto& event : events) {
                            int64_t absoluteSamplePos = clipStartSample + event.timestamp;
                            
                            // If this event falls within the current buffer, trigger it
                            if (absoluteSamplePos >= currentPlaybackPos && 
                                absoluteSamplePos < currentPlaybackPos + static_cast<int64_t>(numFrames)) {
                                track.synth->processMidiMessage(event.message);
                            }
                        }
                    }
                }
            }
        }
        
        // Mix all recording tracks (armed tracks get live MIDI input)
        for (auto& track : tracks_) {
            if (track.synth) {
                AudioBuffer trackBuffer(output.getNumChannels(), numFrames);
                track.synth->generateAudio(trackBuffer, numFrames);
                
                // Apply effects chain
                for (auto& effect : track.effects) {
                    if (effect && effect->isEnabled()) {
                        effect->process(trackBuffer, numFrames);
                    }
                }
                
                // Capture waveform samples for visualization (use first channel, after effects)
                if (trackBuffer.getNumChannels() > 0) {
                    const float* trackSamples = trackBuffer.getReadPointer(0);
                    for (size_t i = 0; i < numFrames; ++i) {
                        track.addWaveformSample(trackSamples[i]);
                    }
                }
                
                // Mix into output
                for (size_t ch = 0; ch < output.getNumChannels(); ++ch) {
                    const float* trackSamples = trackBuffer.getReadPointer(ch);
                    float* outputSamples = output.getWritePointer(ch);
                    for (size_t i = 0; i < numFrames; ++i) {
                        outputSamples[i] += trackSamples[i];
                    }
                }
            } else {
                // No synth - clear waveform buffer
                for (size_t i = 0; i < numFrames && i < Track::WAVEFORM_BUFFER_SIZE; ++i) {
                    track.addWaveformSample(0.0f);
                }
            }
        }
        
        // Update playback position if playing
        if (isPlaying_) {
            playbackSamplePosition_ += numFrames;
        }
    });
    
    // Start audio engine
    if (!engine_->start()) {
        std::cerr << "Failed to start audio engine" << std::endl;
        return false;
    }
    
    return true;
}

bool MainWindow::initializeMIDI() {
    midiInput_ = std::make_shared<MidiInput>();
    auto midiDevices = MidiInput::enumerateDevices();
    
    if (!midiDevices.empty()) {
        std::cout << "Opening MIDI device: " << midiDevices[0] << std::endl;
        if (midiInput_->openDevice(midiDevices[0])) {
            midiInput_->setCallback([this](const MidiMessage& msg) {
                // Track note on/off for piano roll visualization
                {
                    std::lock_guard<std::mutex> lock(notesMutex_);
                    if (msg.getType() == MidiMessageType::NoteOn && msg.getVelocity() > 0) {
                        notesPlaying_[msg.getNoteNumber()] = true;
                        
                        // Auto-position piano roll on first note
                        if (!pianoRollAutoPositioned_) {
                            pianoRollCenterNote_ = msg.getNoteNumber();
                            pianoRollAutoPositioned_ = true;
                        }
                    } else if (msg.getType() == MidiMessageType::NoteOff || 
                               (msg.getType() == MidiMessageType::NoteOn && msg.getVelocity() == 0)) {
                        notesPlaying_[msg.getNoteNumber()] = false;
                    }
                }
                
                // Route MIDI to all recording tracks
                for (size_t trackIdx = 0; trackIdx < tracks_.size(); ++trackIdx) {
                    auto& track = tracks_[trackIdx];
                    if (track.isRecording && track.synth) {
                        track.synth->processMidiMessage(msg);
                        
                        // If master record is active and playing, record to timeline
                        if (masterRecord_ && isPlaying_) {
                            if (!track.recordingClip) {
                                std::cout << "WARNING: Track " << trackIdx << " is recording but has no recordingClip!" << std::endl;
                                // Create recording clip on the fly if missing
                                track.recordingClip = std::make_shared<MidiClip>("Recording");
                                track.recordingClip->setStartTime(0);
                            }
                            
                            // Get timeline position thread-safely
                            float currentTimelinePos;
                            {
                                std::lock_guard<std::mutex> lock(timelineMutex_);
                                currentTimelinePos = timelinePosition_;
                            }
                            
                            // Convert timeline position (beats) to samples relative to clip start
                            // Timeline position is in beats, we need samples from clip start (0)
                            double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                            float beatsPerSecond = bpm_ / 60.0f;
                            // Convert beats to samples (currentTimelinePos is in beats, clip starts at 0)
                            int64_t currentSample = static_cast<int64_t>(currentTimelinePos / beatsPerSecond * sampleRate);
                            
                            // Add event to recording clip (timestamp is relative to clip start)
                            track.recordingClip->addEvent(currentSample, msg);
                            
                            // Debug output
                            static int eventCount = 0;
                            if (++eventCount % 10 == 0) {  // Print every 10th event to avoid spam
                                std::cout << "Recorded MIDI event: track=" << trackIdx 
                                          << ", beat=" << currentTimelinePos 
                                          << ", sample=" << currentSample 
                                          << ", clipEvents=" << track.recordingClip->getEvents().size() << std::endl;
                            }
                        }
                    }
                }
            });
            midiInput_->start();
            std::cout << "MIDI keyboard ready!" << std::endl;
            return true;
        }
    }
    
    return false;
}

void MainWindow::shutdown() {
    if (midiInput_) {
        midiInput_->stop();
        midiInput_.reset();
    }
    
    if (engine_) {
        engine_->stop();
        engine_->shutdown();
        engine_.reset();
    }
    
#ifdef PAN_USE_GUI
    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        
        glfwDestroyWindow(static_cast<GLFWwindow*>(window_));
        glfwTerminate();
        window_ = nullptr;
    }
#endif
}

void MainWindow::requestQuit() {
    shouldQuit_ = true;
}

void MainWindow::run() {
#ifdef PAN_USE_GUI
    GLFWwindow* window = static_cast<GLFWwindow*>(window_);
    
    // Main loop
    while (!glfwWindowShouldClose(window) && !shouldQuit_) {
        glfwPollEvents();
        
        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        renderUI();
        
        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        
        glfwSwapBuffers(window);
        
        // Small sleep to avoid spinning CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }
#else
    std::cerr << "GUI support not compiled" << std::endl;
#endif
}

void MainWindow::renderUI() {
#ifdef PAN_USE_GUI
    // Update timeline if playing
    updateTimeline();
    
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    
    // Render menu bar (separate from dockspace)
    renderMenuBar();
    
    // Render transport controls (BPM, master record) - separate from dockspace
    renderTransportControls();
    
    // Calculate space for dockspace (below menu bar and transport)
    float menuBarHeight = ImGui::GetFrameHeight();
    float transportHeight = 70.0f;  // Match the transport window height
    ImVec2 dockspacePos = ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight + transportHeight);
    ImVec2 dockspaceSize = ImVec2(viewport->Size.x, viewport->Size.y - menuBarHeight - transportHeight);
    
    static bool dockspace_setup = false;
    static ImGuiID dock_id_left = 0;
    static ImGuiID dock_id_right = 0;
    static ImGuiID dock_id_bottom_static = 0;
    static ImGuiID dock_id_piano_roll = 0;
    static ImGuiID dock_id_components = 0;
    
    // Dockspace window - separate from master GUI
    ImGui::SetNextWindowPos(dockspacePos);
    ImGui::SetNextWindowSize(dockspaceSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    
    ImGui::Begin("DockSpace", nullptr, 
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    
    ImGui::PopStyleVar(3);
    
    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
    
    // Set up dock layout on first frame - split vertically: top 2/3 for tracks, bottom 1/3 for components
    if (!dockspace_setup) {
        dockspace_setup = true;
        
        // Clear any existing layout
        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, dockspaceSize);
        
        // Split vertically: top ~60% for tracks, bottom ~40% for piano roll + components
        ImGuiID dock_id_top;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.40f, &dock_id_bottom_static, &dock_id_top);
        
        // Split top into left (Sample Library) and right (tracks with timeline)
        // Use 0.1f (10%) for Sample Library to make it much more compact
        ImGui::DockBuilderSplitNode(dock_id_top, ImGuiDir_Left, 0.1f, &dock_id_left, &dock_id_right);
        
        // Split bottom into piano roll (top 60%) and components (bottom 40%)
        ImGui::DockBuilderSplitNode(dock_id_bottom_static, ImGuiDir_Down, 0.40f, &dock_id_components, &dock_id_piano_roll);
        
        // Configure nodes
        ImGuiDockNode* node_piano_roll = ImGui::DockBuilderGetNode(dock_id_piano_roll);
        if (node_piano_roll) {
            node_piano_roll->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
            node_piano_roll->LocalFlags |= ImGuiDockNodeFlags_NoDockingSplit;
            node_piano_roll->LocalFlags |= ImGuiDockNodeFlags_NoUndocking;
        }
        
        ImGuiDockNode* node_components = ImGui::DockBuilderGetNode(dock_id_components);
        if (node_components) {
            node_components->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
            node_components->LocalFlags |= ImGuiDockNodeFlags_NoDockingSplit;
            node_components->LocalFlags |= ImGuiDockNodeFlags_NoUndocking;
        }
        
        ImGuiDockNode* node_left = ImGui::DockBuilderGetNode(dock_id_left);
        if (node_left) {
            node_left->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
            node_left->LocalFlags |= ImGuiDockNodeFlags_NoDockingSplit;
            node_left->LocalFlags |= ImGuiDockNodeFlags_NoUndocking;
        }
        
        ImGuiDockNode* node_right = ImGui::DockBuilderGetNode(dock_id_right);
        if (node_right) {
            node_right->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
            node_right->LocalFlags |= ImGuiDockNodeFlags_NoDockingSplit;
            node_right->LocalFlags |= ImGuiDockNodeFlags_NoUndocking;
        }
        
        // Dock windows
        ImGui::DockBuilderDockWindow("Sample Library", dock_id_left);
        ImGui::DockBuilderDockWindow("Piano Roll", dock_id_piano_roll);
        ImGui::DockBuilderDockWindow("Components", dock_id_components);
        ImGui::DockBuilderDockWindow("Tracks", dock_id_right);
        
        // Set initial split ratio to make Sample Library much smaller (10% of screen width)
        ImGuiDockNode* leftNode = ImGui::DockBuilderGetNode(dock_id_left);
        if (leftNode) {
            leftNode->Size.x = viewport->Size.x * 0.1f;  // 10% of screen width - much more compact
        }
        
        ImGui::DockBuilderFinish(dockspace_id);
    }
    
    // Render windows - they'll be docked into the pre-configured nodes
    // Pass the specific node IDs so they dock into the correct split nodes
    renderSampleLibrary(dock_id_left);
    renderPianoRoll();
    renderComponents(dock_id_components);
    renderTracks(dock_id_right);
    
    // Handle keyboard shortcuts for piano roll (global)
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !ImGui::GetIO().WantTextInput) {
        pencilToolActive_ = !pencilToolActive_;
        std::cout << "Draw mode (global): " << (pencilToolActive_ ? "ON" : "OFF") << std::endl;
    }
    
    // Handle Ctrl+S for save
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S) && !ImGui::GetIO().WantTextInput) {
        if (!currentProjectPath_.empty()) {
            saveProject();
        } else {
            // Trigger Save As dialog for new projects
            triggerSaveAsDialog_ = true;
        }
    }
    
    ImGui::End(); // DockSpace
#endif
}

void MainWindow::renderSampleLibrary(ImGuiID target_dock_id) {
#ifdef PAN_USE_GUI
    // Force window to ALWAYS dock into the specific LEFT split node
    if (target_dock_id != 0) {
        ImGui::SetNextWindowDockID(target_dock_id, ImGuiCond_Always);
    }
    
    ImGui::Begin("Sample Library", nullptr, ImGuiWindowFlags_None);
    
    // Title above Basic Waves - centered
    float windowWidth = ImGui::GetWindowWidth();
    const char* libraryText = "Library";
    float textWidth = ImGui::CalcTextSize(libraryText).x;
    ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
    ImGui::Text("%s", libraryText);
    ImGui::Separator();
    ImGui::Spacing();
    
    if (ImGui::CollapsingHeader("Basic Waves", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
        Waveform waveforms[] = { 
            Waveform::Sine, 
            Waveform::Square, 
            Waveform::Sawtooth, 
            Waveform::Triangle 
        };
        
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            
            // Make waveform draggable
            if (ImGui::Button(waveNames[i], ImVec2(-1, 0))) {
                // Click to select
            }
            
            // Setup drag source (ImGui will automatically detect if button is being dragged)
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("WAVEFORM", &waveforms[i], sizeof(Waveform));
                ImGui::Text("Dragging %s", waveNames[i]);
                ImGui::EndDragDropSource();
            }
            
            ImGui::PopID();
        }
    }
    
    ImGui::End();
#endif
}

void MainWindow::renderComponents(ImGuiID target_dock_id) {
#ifdef PAN_USE_GUI
    // Force window to ALWAYS dock into the specific BOTTOM split node
    if (target_dock_id != 0) {
        ImGui::SetNextWindowDockID(target_dock_id, ImGuiCond_Always);
    }
    
    ImGui::Begin("Components", nullptr, ImGuiWindowFlags_None);
    
    // Tab bar for Components and Effects
    if (ImGui::BeginTabBar("ComponentsTabs")) {
        // Components tab - shows component boxes from selected track only
        if (ImGui::BeginTabItem("Components")) {
            // Ensure selectedTrackIndex_ is valid
            if (selectedTrackIndex_ >= tracks_.size()) {
                selectedTrackIndex_ = tracks_.empty() ? 0 : tracks_.size() - 1;
            }
            
            // Create a child window to act as the drop zone
            ImVec2 contentSize = ImGui::GetContentRegionAvail();
            ImGui::BeginChild("ComponentsDropZone", ImVec2(contentSize.x, contentSize.y), false);
            
            // Show track name at top
            if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                char trackLabel[64];
                snprintf(trackLabel, sizeof(trackLabel), "Track %zu Components:", selectedTrackIndex_ + 1);
                ImGui::Text("%s", trackLabel);
                ImGui::Separator();
                ImGui::Spacing();
                
                // Render component boxes from selected track only
                auto& oscillators = tracks_[selectedTrackIndex_].oscillators;
                for (size_t oscIdx = 0; oscIdx < oscillators.size(); ++oscIdx) {
                    ImGui::PushID(static_cast<int>(selectedTrackIndex_ * 1000 + oscIdx));
                    renderComponentBox(selectedTrackIndex_, oscIdx, oscillators[oscIdx]);
                    ImGui::PopID();
                    ImGui::SameLine();
                }
            } else {
                ImGui::Text("No tracks available");
            }
            
            ImGui::EndChild();
            
            // Drop target for the entire components area
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WAVEFORM")) {
                    if (payload->DataSize == sizeof(Waveform)) {
                        Waveform droppedWave = *(Waveform*)payload->Data;
                        // Add to selected track
                        if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                            tracks_[selectedTrackIndex_].oscillators.push_back(Oscillator(droppedWave, 1.0f, 0.5f));
                            tracks_[selectedTrackIndex_].synth->setOscillators(tracks_[selectedTrackIndex_].oscillators);
                            markDirty();
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            
            ImGui::EndTabItem();
        }
        
        // Effects tab - shows effects from selected track
        if (ImGui::BeginTabItem("Effects")) {
            // Ensure selectedTrackIndex_ is valid
            if (selectedTrackIndex_ >= tracks_.size() && !tracks_.empty()) {
                selectedTrackIndex_ = tracks_.size() - 1;
            }
            
            // Show track name at top
            if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                char trackLabel[64];
                snprintf(trackLabel, sizeof(trackLabel), "Track %zu Effects:", selectedTrackIndex_ + 1);
                ImGui::Text("%s", trackLabel);
                ImGui::Separator();
                ImGui::Spacing();
                
                // "Add Effect" button
                if (ImGui::Button("+ Add Reverb", ImVec2(150, 30))) {
                    // Add reverb effect to selected track
                    auto reverb = std::make_shared<Reverb>(engine_ ? engine_->getSampleRate() : 44100.0);
                    tracks_[selectedTrackIndex_].effects.push_back(reverb);
                    markDirty();
                }
                
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                
                // Render effect boxes from selected track
                auto& effects = tracks_[selectedTrackIndex_].effects;
                for (size_t effectIdx = 0; effectIdx < effects.size(); ++effectIdx) {
                    ImGui::PushID(static_cast<int>(selectedTrackIndex_ * 10000 + effectIdx));
                    renderEffectBox(selectedTrackIndex_, effectIdx, effects[effectIdx]);
                    ImGui::PopID();
                    ImGui::SameLine();
                }
            } else {
                ImGui::Text("No tracks available");
            }
            
            ImGui::EndTabItem();
        }
        
        ImGui::EndTabBar();
    }
    
    ImGui::End();
#endif
}

void MainWindow::renderComponentBox(size_t trackIndex, size_t oscIndex, Oscillator& osc) {
#ifdef PAN_USE_GUI
    ImGui::PushID(static_cast<int>(oscIndex + 10000 * trackIndex));
    
    // Get background color and create lighter shades for sliders
    ImVec4 bgColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    ImVec4 sliderGrab = ImVec4(bgColor.x * 1.8f, bgColor.y * 1.8f, bgColor.z * 1.8f, 1.0f);
    ImVec4 sliderGrabActive = ImVec4(bgColor.x * 2.2f, bgColor.y * 2.2f, bgColor.z * 2.2f, 1.0f);
    ImVec4 frameBg = ImVec4(bgColor.x * 1.3f, bgColor.y * 1.3f, bgColor.z * 1.3f, 1.0f);
    ImVec4 frameBgHovered = ImVec4(bgColor.x * 1.5f, bgColor.y * 1.5f, bgColor.z * 1.5f, 1.0f);
    ImVec4 frameBgActive = ImVec4(bgColor.x * 1.7f, bgColor.y * 1.7f, bgColor.z * 1.7f, 1.0f);
    
    // Push lighter slider colors for better visibility
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, sliderGrab);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, sliderGrabActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBgHovered);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frameBgActive);
    
    // Component box - bordered rectangle
    ImGui::BeginChild(ImGui::GetID("component_box"), ImVec2(250, 120), true, ImGuiWindowFlags_None);
    
    // Component header with remove button
    const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
    const char* waveName = waveNames[static_cast<int>(osc.waveform)];
    ImGui::Text("%s", waveName);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
    
    auto& oscillators = tracks_[trackIndex].oscillators;
    if (oscillators.size() > 1 && ImGui::Button("X", ImVec2(30, 0))) {
        oscillators.erase(oscillators.begin() + oscIndex);
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
        ImGui::EndChild();
        ImGui::PopStyleColor(5);
        ImGui::PopID();
        return;
    }
    
    ImGui::Separator();
    
    // Waveform selector
    int currentWave = static_cast<int>(osc.waveform);
    if (ImGui::Combo("Wave", &currentWave, waveNames, 4)) {
        osc.waveform = static_cast<Waveform>(currentWave);
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
    }
    
    // Frequency multiplier
    float freqMult = osc.frequencyMultiplier;
    if (ImGui::SliderFloat("Freq", &freqMult, 0.1f, 4.0f, "%.2fx")) {
        osc.frequencyMultiplier = freqMult;
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
    }
    
    // Amplitude
    float amp = osc.amplitude;
    if (ImGui::SliderFloat("Amp", &amp, 0.0f, 1.0f, "%.2f")) {
        osc.amplitude = amp;
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
    }
    
    ImGui::EndChild();
    
    // Pop slider style colors
    ImGui::PopStyleColor(5);
    
    ImGui::PopID();
#endif
}

void MainWindow::renderEffectBox(size_t trackIndex, size_t effectIndex, std::shared_ptr<Effect> effect) {
#ifdef PAN_USE_GUI
    if (!effect) return;
    
    ImGui::PushID(static_cast<int>(effectIndex + 20000 * trackIndex));
    
    // Get background color and create lighter shades for sliders
    ImVec4 bgColor = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    ImVec4 sliderGrab = ImVec4(bgColor.x * 1.8f, bgColor.y * 1.8f, bgColor.z * 1.8f, 1.0f);
    ImVec4 sliderGrabActive = ImVec4(bgColor.x * 2.2f, bgColor.y * 2.2f, bgColor.z * 2.2f, 1.0f);
    ImVec4 frameBg = ImVec4(bgColor.x * 1.3f, bgColor.y * 1.3f, bgColor.z * 1.3f, 1.0f);
    ImVec4 frameBgHovered = ImVec4(bgColor.x * 1.5f, bgColor.y * 1.5f, bgColor.z * 1.5f, 1.0f);
    ImVec4 frameBgActive = ImVec4(bgColor.x * 1.7f, bgColor.y * 1.7f, bgColor.z * 1.7f, 1.0f);
    
    // Push lighter slider colors for better visibility
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, sliderGrab);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, sliderGrabActive);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, frameBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, frameBgHovered);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, frameBgActive);
    
    // Effect box - bordered rectangle
    ImGui::BeginChild(ImGui::GetID("effect_box"), ImVec2(280, 220), true, ImGuiWindowFlags_None);
    
    // Check if this is a Reverb effect for preset dropdown
    auto reverb = std::dynamic_pointer_cast<Reverb>(effect);
    
    // Effect header with preset dropdown (for reverb)
    if (reverb) {
        // Make the title a combo box for preset selection
        const char* currentPresetName = Reverb::getPresetName(reverb->getCurrentPreset());
        char comboLabel[64];
        snprintf(comboLabel, sizeof(comboLabel), "Reverb: %s", currentPresetName);
        
        if (ImGui::BeginCombo("##preset", comboLabel, ImGuiComboFlags_NoArrowButton)) {
            for (int i = 0; i < 7; ++i) {
                Reverb::Preset preset = static_cast<Reverb::Preset>(i);
                const char* presetName = Reverb::getPresetName(preset);
                bool isSelected = (reverb->getCurrentPreset() == preset);
                
                if (ImGui::Selectable(presetName, isSelected)) {
                    reverb->loadPreset(preset);
                    markDirty();
                }
                
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::Text("%s", effect->getName().c_str());
    }
    
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
    
    bool enabled = effect->isEnabled();
    if (ImGui::Checkbox("On", &enabled)) {
        effect->setEnabled(enabled);
        markDirty();
    }
    
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 30);
    if (ImGui::Button("X", ImVec2(25, 0))) {
        tracks_[trackIndex].effects.erase(tracks_[trackIndex].effects.begin() + effectIndex);
        markDirty();
        ImGui::EndChild();
        ImGui::PopStyleColor(5);
        ImGui::PopID();
        return;
    }
    
    ImGui::Separator();
    
    // Show parameters if this is a Reverb
    if (reverb) {
        bool anyChanged = false;
        
        // Room Size
        float roomSize = reverb->getRoomSize();
        if (ImGui::SliderFloat("Room Size", &roomSize, 0.0f, 1.0f, "%.2f")) {
            reverb->setRoomSize(roomSize);
            anyChanged = true;
            markDirty();
        }
        
        // Damping
        float damping = reverb->getDamping();
        if (ImGui::SliderFloat("Damping", &damping, 0.0f, 1.0f, "%.2f")) {
            reverb->setDamping(damping);
            anyChanged = true;
            markDirty();
        }
        
        // Wet Level
        float wet = reverb->getWetLevel();
        if (ImGui::SliderFloat("Wet", &wet, 0.0f, 1.0f, "%.2f")) {
            reverb->setWetLevel(wet);
            anyChanged = true;
            markDirty();
        }
        
        // Dry Level
        float dry = reverb->getDryLevel();
        if (ImGui::SliderFloat("Dry", &dry, 0.0f, 1.0f, "%.2f")) {
            reverb->setDryLevel(dry);
            anyChanged = true;
            markDirty();
        }
        
        // Width
        float width = reverb->getWidth();
        if (ImGui::SliderFloat("Width", &width, 0.0f, 1.0f, "%.2f")) {
            reverb->setWidth(width);
            anyChanged = true;
            markDirty();
        }
        
        // If any parameter changed manually and current preset isn't Custom, mark as Custom
        if (anyChanged && reverb->getCurrentPreset() != Reverb::Preset::Custom) {
            reverb->setCurrentPreset(Reverb::Preset::Custom);
        }
    }
    
    ImGui::EndChild();
    
    // Pop slider style colors
    ImGui::PopStyleColor(5);
    
    ImGui::PopID();
#endif
}

void MainWindow::renderTracks(ImGuiID target_dock_id) {
#ifdef PAN_USE_GUI
    // Force window to ALWAYS dock into the specific RIGHT split node
    if (target_dock_id != 0) {
        ImGui::SetNextWindowDockID(target_dock_id, ImGuiCond_Always);
    }
    
    ImGui::Begin("Tracks", nullptr, ImGuiWindowFlags_None);
    
    // Get window info for drawing background (declare at function scope)
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    ImU32 darkerBgColor = IM_COL32(31, 36, 41, 255);  // Dark background matching mockup
    
    // Draw dark background for entire window (behind everything)
    drawList->AddRectFilled(
        windowPos,
        ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
        darkerBgColor
    );
    
    // Split into two columns: left for track controls, right for timeline
    ImGui::Columns(2, "track_columns", false);
    ImGui::SetColumnWidth(0, 400.0f);  // Left column: track controls
    // Right column: timeline (auto-sized)
    
    // Render all real tracks
    for (size_t i = 0; i < tracks_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        
        // Add spacing between tracks
        if (i > 0) {
            ImGui::Spacing();
            ImGui::Spacing();
        }
        
        // LEFT COLUMN: Track header
        float headerHeight = 120.0f;
        float columnWidth = ImGui::GetColumnWidth(0);
        ImVec2 headerStartPos = ImGui::GetCursorScreenPos();
        
        // Draw track header background rectangle (highlight if selected)
        bool isSelected = (i == selectedTrackIndex_);
        ImU32 trackHeaderBg;
        if (isSelected) {
            // Selected track - brighter/highlighted color
            trackHeaderBg = IM_COL32(90, 100, 110, 255);
        } else {
            // Normal alternating colors
            trackHeaderBg = (i % 2 == 0) ? IM_COL32(70, 75, 80, 255) : IM_COL32(75, 80, 85, 255);
        }
        drawList->AddRectFilled(
            headerStartPos,
            ImVec2(headerStartPos.x + columnWidth, headerStartPos.y + headerHeight),
            trackHeaderBg
        );
        
        // Add selection border if selected
        if (isSelected) {
            drawList->AddRect(
                headerStartPos,
                ImVec2(headerStartPos.x + columnWidth, headerStartPos.y + headerHeight),
                IM_COL32(150, 170, 190, 255),
                0.0f, 0, 2.0f
            );
        }
        
        // This is a real track - render normal track content
        ImVec2 textSize = ImGui::CalcTextSize("Track 1");
        float buttonWidth = 30.0f;
        float spacing = 20.0f;
        float totalWidth = textSize.x + spacing + buttonWidth;
        ImVec2 headerCenter = ImVec2(headerStartPos.x + columnWidth / 2.0f, headerStartPos.y + headerHeight / 2.0f);
        
        // Track label - centered
        char trackLabel[32];
        snprintf(trackLabel, sizeof(trackLabel), "Track %zu", i + 1);
        ImVec2 textPos = ImVec2(headerCenter.x - totalWidth / 2.0f, headerCenter.y - textSize.y / 2.0f);
        drawList->AddText(textPos, IM_COL32(250, 250, 240, 255), trackLabel);
        
        // Hot button - grey/red circle
        ImVec2 buttonPos = ImVec2(textPos.x + textSize.x + spacing, headerCenter.y - 15.0f);
        ImVec2 buttonRectMin = ImVec2(buttonPos.x - 2, buttonPos.y - 2);
        ImVec2 buttonRectMax = ImVec2(buttonPos.x + 32, buttonPos.y + 32);
        
        ImVec2 mousePos = ImGui::GetMousePos();
        bool isHovered = (mousePos.x >= buttonRectMin.x && mousePos.x <= buttonRectMax.x &&
                         mousePos.y >= buttonRectMin.y && mousePos.y <= buttonRectMax.y);
        
        if (isHovered && ImGui::IsMouseClicked(0)) {
            tracks_[i].isRecording = !tracks_[i].isRecording;
        }
        
        // Draw filled circle for hot button
        ImVec2 center = ImVec2(buttonPos.x + 15, buttonPos.y + 15);
        float radius = 10.0f;
        ImU32 circleColor = tracks_[i].isRecording ? 
            IM_COL32(255, 0, 0, 255) : IM_COL32(120, 120, 120, 255);
        drawList->AddCircleFilled(center, radius, circleColor);
        
        // Remove track button (X) - only show if more than 1 track
        if (tracks_.size() > 1) {
            ImVec2 xButtonPos = ImVec2(headerStartPos.x + columnWidth - 35.0f, headerStartPos.y + 5.0f);
            ImVec2 xButtonRectMin = ImVec2(xButtonPos.x, xButtonPos.y);
            ImVec2 xButtonRectMax = ImVec2(xButtonPos.x + 25, xButtonPos.y + 25);
            
            bool xHovered = (mousePos.x >= xButtonRectMin.x && mousePos.x <= xButtonRectMax.x &&
                            mousePos.y >= xButtonRectMin.y && mousePos.y <= xButtonRectMax.y);
            
            if (xHovered) {
                drawList->AddRectFilled(xButtonRectMin, xButtonRectMax, IM_COL32(100, 100, 100, 100));
            }
            
            drawList->AddText(xButtonPos, IM_COL32(250, 250, 240, 255), "X");
            
            if (xHovered && ImGui::IsMouseClicked(0)) {
                tracks_.erase(tracks_.begin() + i);
                // Adjust selected track index if necessary
                if (selectedTrackIndex_ >= tracks_.size() && !tracks_.empty()) {
                    selectedTrackIndex_ = tracks_.size() - 1;
                } else if (tracks_.empty()) {
                    selectedTrackIndex_ = 0;
                }
                markDirty();
                ImGui::PopID();
                ImGui::NextColumn();
                ImGui::NextColumn();
                continue;
            }
        }
        
        // Use Dummy to advance cursor and make track header clickable
        ImGui::Dummy(ImVec2(columnWidth, headerHeight));
        
        // Check if track header was clicked (for selection)
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            // Only select if click wasn't on hot button or X button
            ImVec2 mousePos = ImGui::GetMousePos();
            bool clickedHotButton = (mousePos.x >= buttonRectMin.x && mousePos.x <= buttonRectMax.x &&
                                     mousePos.y >= buttonRectMin.y && mousePos.y <= buttonRectMax.y);
            bool clickedXButton = false;
            if (tracks_.size() > 1) {
                ImVec2 xButtonPos = ImVec2(headerStartPos.x + columnWidth - 35.0f, headerStartPos.y + 5.0f);
                ImVec2 xButtonRectMin = ImVec2(xButtonPos.x, xButtonPos.y);
                ImVec2 xButtonRectMax = ImVec2(xButtonPos.x + 25, xButtonPos.y + 25);
                clickedXButton = (mousePos.x >= xButtonRectMin.x && mousePos.x <= xButtonRectMax.x &&
                                 mousePos.y >= xButtonRectMin.y && mousePos.y <= xButtonRectMax.y);
            }
            
            if (!clickedHotButton && !clickedXButton) {
                selectedTrackIndex_ = i;
            }
        }
        
        // Drop target for waveforms
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WAVEFORM")) {
                if (payload->DataSize == sizeof(Waveform)) {
                    Waveform droppedWave = *(Waveform*)payload->Data;
                    auto& oscillators = tracks_[i].oscillators;
                    oscillators.push_back(Oscillator(droppedWave, 1.0f, 0.5f));
                    tracks_[i].synth->setOscillators(oscillators);
                    tracks_[i].waveformSet = true;
                    // Select this track when a sample is dropped on it
                    selectedTrackIndex_ = i;
                    markDirty();
                }
            }
            ImGui::EndDragDropTarget();
        }
        
        // RIGHT COLUMN: Timeline
        ImGui::NextColumn();
        renderTrackTimeline(i);
        ImGui::NextColumn();
        
        // Add separator between tracks (but not after last one)
        if (i < tracks_.size() - 1) {
            ImGui::Separator();
            ImGui::NextColumn();
            ImGui::Separator();
            ImGui::NextColumn();
        }
        
        ImGui::PopID();
    }
    
    // After all tracks are rendered, add "+ Add Track" button in left column only
    ImGui::Spacing();
    ImGui::Spacing();
    
    // Draw "+ Add Track" button - centered in column
    float columnWidth = ImGui::GetColumnWidth(0);
    ImVec2 buttonStartPos = ImGui::GetCursorScreenPos();
    ImVec2 buttonSize = ImVec2(100, 30);
    // Center button horizontally in the column
    float leftPadding = (columnWidth - buttonSize.x) / 2.0f;
    ImVec2 buttonPos = ImVec2(buttonStartPos.x + leftPadding, buttonStartPos.y);
    
    ImVec2 mousePos = ImGui::GetMousePos();
    bool isHovered = (mousePos.x >= buttonPos.x && mousePos.x <= buttonPos.x + buttonSize.x &&
                     mousePos.y >= buttonPos.y && mousePos.y <= buttonPos.y + buttonSize.y);
    
    // Draw button
    ImU32 buttonColor = isHovered ? IM_COL32(70, 70, 70, 255) : IM_COL32(60, 60, 60, 255);
    drawList->AddRectFilled(buttonPos, ImVec2(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y), 
                           buttonColor, 3.0f);
    drawList->AddRect(buttonPos, ImVec2(buttonPos.x + buttonSize.x, buttonPos.y + buttonSize.y), 
                     IM_COL32(100, 100, 100, 255), 3.0f, 0, 1.0f);
    
    // Draw button text
    ImVec2 textSize = ImGui::CalcTextSize("+ Add Track");
    ImVec2 textPos = ImVec2(buttonPos.x + (buttonSize.x - textSize.x) / 2.0f, 
                            buttonPos.y + (buttonSize.y - textSize.y) / 2.0f);
    drawList->AddText(textPos, IM_COL32(200, 200, 200, 255), "+ Add Track");
    
    if (isHovered && ImGui::IsMouseClicked(0)) {
        Track newTrack;
        newTrack.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
        newTrack.synth->setVolume(0.5f);
        newTrack.synth->setOscillators(newTrack.oscillators);
        tracks_.push_back(std::move(newTrack));
        // Select the newly created track
        selectedTrackIndex_ = tracks_.size() - 1;
        markDirty();
    }
    
    // Small dummy to advance cursor past button
    ImGui::Dummy(ImVec2(columnWidth, buttonSize.y + 10.0f));
    
    // End columns
    ImGui::Columns(1);
    
    ImGui::End();
#endif
}

void MainWindow::initializeCommonDirectories() {
    namespace fs = std::filesystem;
    
    // Get home directory
    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        homeDir = getenv("USERPROFILE"); // Windows fallback
    }
    
    if (homeDir) {
        fs::path homePath(homeDir);
        
        // Add home directory
        commonDirectories_.push_back({"🏠 Home", homePath.string()});
        
        // Add common subdirectories if they exist
        std::vector<std::pair<std::string, std::string>> commonDirs = {
            {"📄 Documents", (homePath / "Documents").string()},
            {"⬇️ Downloads", (homePath / "Downloads").string()},
            {"🎵 Music", (homePath / "Music").string()},
            {"🖼️ Pictures", (homePath / "Pictures").string()},
            {"🎬 Videos", (homePath / "Videos").string()},
            {"🖥️ Desktop", (homePath / "Desktop").string()}
        };
        
        for (const auto& [name, path] : commonDirs) {
            if (fs::exists(path) && fs::is_directory(path)) {
                commonDirectories_.push_back({name, path});
            }
        }
    }
    
    // Add root directory
    commonDirectories_.push_back({"💾 Root", "/"});
}

void MainWindow::updateFileBrowser(const std::string& path) {
    fileBrowserDirs_.clear();
    fileBrowserFiles_.clear();
    fileBrowserPath_ = path;
    
    try {
        namespace fs = std::filesystem;
        
        // Add parent directory option
        fs::path currentPath(path);
        if (currentPath.has_parent_path() && currentPath != currentPath.root_path()) {
            fileBrowserDirs_.push_back("..");
        }
        
        // List directories and files
        for (const auto& entry : fs::directory_iterator(path)) {
            try {
                if (entry.is_directory()) {
                    fileBrowserDirs_.push_back(entry.path().filename().string());
                } else if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    // Show all files, or filter by extension if needed
                    fileBrowserFiles_.push_back(filename);
                }
            } catch (...) {
                // Skip entries that can't be accessed
            }
        }
        
        // Sort directories and files alphabetically
        std::sort(fileBrowserDirs_.begin(), fileBrowserDirs_.end());
        std::sort(fileBrowserFiles_.begin(), fileBrowserFiles_.end());
        
    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << std::endl;
    }
}

void MainWindow::renderMenuBar() {
#ifdef PAN_USE_GUI
    static bool showNewDialog = false;
    static bool showOpenDialog = false;
    static bool showSaveAsDialog = false;
    static bool showUnsavedDialog = false;
    static enum class PendingAction { None, New, Open, Quit } pendingAction = PendingAction::None;
    
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) {
                if (hasUnsavedChanges_) {
                    showUnsavedDialog = true;
                    pendingAction = PendingAction::New;
                } else {
                    newProject();
                }
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                if (hasUnsavedChanges_) {
                    showUnsavedDialog = true;
                    pendingAction = PendingAction::Open;
                } else {
                    showOpenDialog = true;
                }
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                if (currentProjectPath_.empty()) {
                    showSaveAsDialog = true;
                } else {
                    saveProject();
                }
            }
            if (ImGui::MenuItem("Save As...")) {
                showSaveAsDialog = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
                if (hasUnsavedChanges_) {
                    showUnsavedDialog = true;
                    pendingAction = PendingAction::Quit;
                } else {
                    shouldQuit_ = true;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Options")) {
            ImGui::MenuItem("Preferences", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    
    // Handle unsaved changes dialog
    if (showUnsavedDialog) {
        ImGui::OpenPopup("Unsaved Changes##menu");
        showUnsavedDialog = false;
    }
    
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2.0f, viewport->Pos.y + viewport->Size.y / 2.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    
    if (ImGui::BeginPopupModal("Unsaved Changes##menu", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You have unsaved changes.");
        ImGui::Text("Would you like to save before continuing?");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            if (currentProjectPath_.empty()) {
                showSaveAsDialog = true;
            } else {
                saveProject();
            }
            ImGui::CloseCurrentPopup();
            
            // Execute pending action after save
            if (pendingAction == PendingAction::New) {
                newProject();
            } else if (pendingAction == PendingAction::Open) {
                showOpenDialog = true;
            } else if (pendingAction == PendingAction::Quit) {
                shouldQuit_ = true;
            }
            pendingAction = PendingAction::None;
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            
            // Execute pending action without saving
            if (pendingAction == PendingAction::New) {
                newProject();
            } else if (pendingAction == PendingAction::Open) {
                showOpenDialog = true;
            } else if (pendingAction == PendingAction::Quit) {
                shouldQuit_ = true;
            }
            pendingAction = PendingAction::None;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
            pendingAction = PendingAction::None;
        }
        
        ImGui::EndPopup();
    }
    
    // Handle Save As dialog (from menu or keyboard shortcut)
    if (showSaveAsDialog || triggerSaveAsDialog_) {
        ImGui::OpenPopup("Save Project As##menu");
        updateFileBrowser(fileBrowserPath_);
        showSaveAsDialog = false;
        triggerSaveAsDialog_ = false;
    }
    
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2.0f, viewport->Pos.y + viewport->Size.y / 2.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(750, 450), ImGuiCond_Appearing);
    
    if (ImGui::BeginPopupModal("Save Project As##menu", nullptr, ImGuiWindowFlags_NoResize)) {
        static char filename[256] = "untitled.pan";
        static char locationBuf[512];
        
        // Editable location path
        ImGui::Text("Location:");
        ImGui::SameLine();
        strncpy(locationBuf, fileBrowserPath_.c_str(), sizeof(locationBuf) - 1);
        locationBuf[sizeof(locationBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##location", locationBuf, sizeof(locationBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            namespace fs = std::filesystem;
            if (fs::exists(locationBuf) && fs::is_directory(locationBuf)) {
                updateFileBrowser(locationBuf);
            }
        }
        
        ImGui::Separator();
        
        // Two-column layout: sidebar + file browser
        ImGui::BeginChild("SidebarSave", ImVec2(150, 300), true);
        ImGui::Text("Places");
        ImGui::Separator();
        
        for (const auto& [name, path] : commonDirectories_) {
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_DontClosePopups)) {
                updateFileBrowser(path);
            }
        }
        
        ImGui::EndChild();
        
        ImGui::SameLine();
        
        // File browser area
        ImGui::BeginChild("FileBrowser", ImVec2(0, 300), true);
        
        ImDrawList* fileBrowserDrawList = ImGui::GetWindowDrawList();
        
        // List directories
        for (const auto& dir : fileBrowserDirs_) {
            ImVec2 selectablePos = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::Selectable(("        " + dir).c_str(), false, ImGuiSelectableFlags_DontClosePopups);
            
            // Draw folder icon on top of selectable
            if (folderIconTexture_) {
                ImVec2 iconPos(selectablePos.x + 4, selectablePos.y + 2);
                fileBrowserDrawList->AddImage(folderIconTexture_, 
                                             iconPos, 
                                             ImVec2(iconPos.x + folderIconWidth_, iconPos.y + folderIconHeight_));
            }
            
            if (clicked) {
                namespace fs = std::filesystem;
                if (dir == "..") {
                    fs::path parent = fs::path(fileBrowserPath_).parent_path();
                    updateFileBrowser(parent.string());
                } else {
                    fs::path newPath = fs::path(fileBrowserPath_) / dir;
                    updateFileBrowser(newPath.string());
                }
            }
        }
        
        // List .pan files
        for (const auto& file : fileBrowserFiles_) {
            if (file.find(".pan") != std::string::npos) {
                ImVec2 selectablePos = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::Selectable(("        " + file).c_str(), false, ImGuiSelectableFlags_DontClosePopups);
                
                // Draw file icon (simple document icon using text)
                fileBrowserDrawList->AddText(ImVec2(selectablePos.x + 4, selectablePos.y), 
                                            IM_COL32(250, 250, 240, 255), 
                                            "\xF0\x9F\x93\x84");  // Document emoji UTF-8
                
                if (clicked) {
                    strncpy(filename, file.c_str(), sizeof(filename) - 1);
                    filename[sizeof(filename) - 1] = '\0';
                }
            }
        }
        
        ImGui::EndChild();
        
        ImGui::Separator();
        ImGui::Text("Filename:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##filename", filename, sizeof(filename));
        ImGui::Spacing();
        
        if (ImGui::Button("Save", ImVec2(120, 0))) {
            namespace fs = std::filesystem;
            std::string fname = filename;
            if (fname.find(".pan") == std::string::npos) {
                fname += ".pan";
            }
            currentProjectPath_ = (fs::path(fileBrowserPath_) / fname).string();
            saveProject();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
    
    // Handle Open dialog
    if (showOpenDialog) {
        ImGui::OpenPopup("Open Project##menu");
        updateFileBrowser(fileBrowserPath_);
        showOpenDialog = false;
    }
    
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2.0f, viewport->Pos.y + viewport->Size.y / 2.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(750, 450), ImGuiCond_Appearing);
    
    if (ImGui::BeginPopupModal("Open Project##menu", nullptr, ImGuiWindowFlags_NoResize)) {
        static char selectedFile[256] = "";
        static char locationBufOpen[512];
        
        // Editable location path
        ImGui::Text("Location:");
        ImGui::SameLine();
        strncpy(locationBufOpen, fileBrowserPath_.c_str(), sizeof(locationBufOpen) - 1);
        locationBufOpen[sizeof(locationBufOpen) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##locationOpen", locationBufOpen, sizeof(locationBufOpen), ImGuiInputTextFlags_EnterReturnsTrue)) {
            namespace fs = std::filesystem;
            if (fs::exists(locationBufOpen) && fs::is_directory(locationBufOpen)) {
                updateFileBrowser(locationBufOpen);
            }
        }
        
        ImGui::Separator();
        
        // Two-column layout: sidebar + file browser
        ImGui::BeginChild("SidebarOpen", ImVec2(150, 310), true);
        ImGui::Text("Places");
        ImGui::Separator();
        
        for (const auto& [name, path] : commonDirectories_) {
            if (ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_DontClosePopups)) {
                updateFileBrowser(path);
            }
        }
        
        ImGui::EndChild();
        
        ImGui::SameLine();
        
        // File browser area
        ImGui::BeginChild("FileBrowserOpen", ImVec2(0, 310), true);
        
        ImDrawList* fileBrowserOpenDrawList = ImGui::GetWindowDrawList();
        
        // List directories
        for (const auto& dir : fileBrowserDirs_) {
            ImVec2 selectablePos = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::Selectable(("        " + dir).c_str(), false, ImGuiSelectableFlags_DontClosePopups);
            
            // Draw folder icon on top of selectable
            if (folderIconTexture_) {
                ImVec2 iconPos(selectablePos.x + 4, selectablePos.y + 2);
                fileBrowserOpenDrawList->AddImage(folderIconTexture_, 
                                                 iconPos, 
                                                 ImVec2(iconPos.x + folderIconWidth_, iconPos.y + folderIconHeight_));
            }
            
            if (clicked) {
                namespace fs = std::filesystem;
                if (dir == "..") {
                    fs::path parent = fs::path(fileBrowserPath_).parent_path();
                    updateFileBrowser(parent.string());
                } else {
                    fs::path newPath = fs::path(fileBrowserPath_) / dir;
                    updateFileBrowser(newPath.string());
                }
            }
        }
        
        // List .pan files
        for (const auto& file : fileBrowserFiles_) {
            if (file.find(".pan") != std::string::npos) {
                bool isSelected = (std::string(selectedFile) == file);
                ImVec2 selectablePos = ImGui::GetCursorScreenPos();
                bool clicked = ImGui::Selectable(("        " + file).c_str(), isSelected, ImGuiSelectableFlags_DontClosePopups);
                
                // Draw file icon (simple document icon using text)
                fileBrowserOpenDrawList->AddText(ImVec2(selectablePos.x + 4, selectablePos.y), 
                                                IM_COL32(250, 250, 240, 255), 
                                                "\xF0\x9F\x93\x84");  // Document emoji UTF-8
                
                if (clicked) {
                    strncpy(selectedFile, file.c_str(), sizeof(selectedFile) - 1);
                    selectedFile[sizeof(selectedFile) - 1] = '\0';
                }
                
                // Double-click to open
                if (isSelected && ImGui::IsMouseDoubleClicked(0)) {
                    namespace fs = std::filesystem;
                    std::string fullPath = (fs::path(fileBrowserPath_) / selectedFile).string();
                    
                    std::ifstream fileStream(fullPath);
                    if (fileStream.is_open()) {
                        std::string data((std::istreambuf_iterator<char>(fileStream)),
                                        std::istreambuf_iterator<char>());
                        fileStream.close();
                        
                        if (deserializeProject(data)) {
                            currentProjectPath_ = fullPath;
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
        }
        
        ImGui::EndChild();
        
        ImGui::Separator();
        ImGui::Text("Selected: %s", selectedFile[0] ? selectedFile : "None");
        ImGui::Spacing();
        
        if (ImGui::Button("Open", ImVec2(120, 0)) && selectedFile[0]) {
            namespace fs = std::filesystem;
            std::string fullPath = (fs::path(fileBrowserPath_) / selectedFile).string();
            
            std::ifstream file(fullPath);
            if (file.is_open()) {
                std::string data((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
                file.close();
                
                if (deserializeProject(data)) {
                    currentProjectPath_ = fullPath;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::EndPopup();
    }
#endif
}

void MainWindow::renderTransportControls() {
#ifdef PAN_USE_GUI
    // Create a bar below menu bar for transport controls - separate from dockspace
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    float menuBarHeight = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + menuBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 70.0f));  // Increased height for more space
    ImGui::SetNextWindowViewport(viewport->ID);
    
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15.0f, 12.0f));  // More padding
    
    ImGui::Begin("Transport", nullptr, 
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    
    ImGui::PopStyleVar(3);
    
    // Calculate center position for transport buttons first
    float windowWidth = ImGui::GetContentRegionAvail().x;
    float buttonAreaWidth = 200.0f;  // Width for all four buttons
    float centerX = windowWidth / 2.0f;
    
    // Position BPM controls to the left of center
    float bpmWidth = 150.0f;  // Total width for "BPM:" + input
    float bpmStartX = centerX - buttonAreaWidth / 2.0f - bpmWidth - 20.0f;  // 20px gap before buttons
    
    ImGui::SetCursorPosX(bpmStartX);
    
    // Vertically center the text and button
    float textHeight = ImGui::GetTextLineHeight();
    float buttonHeight = 30.0f;
    float verticalOffset = (buttonHeight - textHeight) / 2.0f;
    
    // BPM label - vertically centered
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalOffset);
    ImGui::Text("BPM:");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - verticalOffset);  // Reset Y
    ImGui::SameLine();
    
    // BPM input with drag, scroll, and double-click editing
    static bool bpmEditing = false;
    static char bpmInput[16] = "";
    static bool bpmDragging = false;
    static float bpmDragStartY = 0.0f;
    static float bpmDragStartValue = 0.0f;
    
    ImGui::PushID("bpm");
    ImGui::SetNextItemWidth(80.0f);
    
    if (bpmEditing) {
        // Text input mode (double-clicked)
        if (ImGui::InputText("##bpm_input", bpmInput, sizeof(bpmInput), 
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal)) {
            float newBpm = std::atof(bpmInput);
            if (newBpm > 0.0f && newBpm <= 300.0f) {
                bpm_ = newBpm;
                markDirty();
            }
            bpmEditing = false;
            bpmInput[0] = '\0';
        }
        if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
            // Clicked away - exit edit mode
            bpmEditing = false;
            bpmInput[0] = '\0';
        }
    } else {
        // Display mode with drag, scroll
        char bpmDisplay[16];
        snprintf(bpmDisplay, sizeof(bpmDisplay), "%.0f", bpm_);
        
        ImVec2 bpmButtonPos = ImGui::GetCursorScreenPos();
        ImVec2 bpmButtonSize(80.0f, buttonHeight);
        
        // Draw the BPM value as a button
        ImGui::Button(bpmDisplay, bpmButtonSize);
        bool wasHovered = ImGui::IsItemHovered();
        
        // Handle dragging - continues even when mouse leaves button area
        if (bpmDragging) {
            // Set cursor to resize vertical
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            
            float dragDelta = bpmDragStartY - ImGui::GetMousePos().y;  // Invert: drag up = increase
            float bpmDelta = dragDelta * 0.3f;  // Smoother sensitivity: ~3 pixels = 1 BPM
            float newBpm = std::clamp(bpmDragStartValue + bpmDelta, 1.0f, 300.0f);
            if (std::abs(newBpm - bpm_) > 0.01f) {  // Smooth updates
                bpm_ = newBpm;
                markDirty();
            }
            
            // Stop dragging when mouse released
            if (!ImGui::IsMouseDown(0)) {
                bpmDragging = false;
            }
        } else {
            // Start dragging if clicked and mouse moved
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) {
                bpmDragging = true;
                bpmDragStartY = ImGui::GetMousePos().y;
                bpmDragStartValue = bpm_;
            }
        }
        
        // Handle hover interactions (only when not dragging)
        if (!bpmDragging && wasHovered) {
            // Change cursor to indicate draggable
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
            
            if (ImGui::IsMouseDoubleClicked(0)) {
                // Double-click to edit
                snprintf(bpmInput, sizeof(bpmInput), "%.0f", bpm_);
                bpmEditing = true;
            } else {
                // Scroll to change BPM
                float scroll = ImGui::GetIO().MouseWheel;
                if (scroll != 0.0f) {
                    bpm_ = std::clamp(bpm_ + scroll, 1.0f, 300.0f);
                    markDirty();
                }
            }
        }
    }
    ImGui::PopID();
    
    // Transport buttons: Play, Pause, Record, Stop - centered
    ImGui::SameLine();
    ImGui::SetCursorPosX(centerX - buttonAreaWidth / 2.0f);
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Play button
    ImVec2 playButtonPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
    
    if (ImGui::Button("##play", ImVec2(40, 40))) {
        // Check if count-in is enabled and we're recording
        if (countInEnabled_ && masterRecord_) {
            // Start count-in
            isCountingIn_ = true;
            countInBeatsRemaining_ = 4;  // 4 beat count-in
            countInLastBeatTime_ = 0.0;
            isPlaying_ = false;  // Don't start playing yet
            
            // Reset timeline position
            {
                std::lock_guard<std::mutex> lock(timelineMutex_);
                timelinePosition_ = 0.0f;
            }
            timelineScrollX_ = 0.0f;
            playbackSamplePosition_ = 0;
            
            // Create recording clips for all hot tracks
            for (auto& track : tracks_) {
                if (track.isRecording) {
                    track.recordingClip = std::make_shared<MidiClip>("Recording");
                    track.recordingClip->setStartTime(0);
                }
            }
        } else {
            // Normal playback without count-in
            isPlaying_ = true;
            
            // Reset playback position to timeline position (in samples)
            double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
            float beatsPerSecond = bpm_ / 60.0f;
            float currentTimelinePos;
            {
                std::lock_guard<std::mutex> lock(timelineMutex_);
                currentTimelinePos = timelinePosition_;
            }
            playbackSamplePosition_ = static_cast<int64_t>(currentTimelinePos / beatsPerSecond * sampleRate);
            
            if (masterRecord_) {
                // Start recording - reset timeline position when play starts
                {
                    std::lock_guard<std::mutex> lock(timelineMutex_);
                    timelinePosition_ = 0.0f;
                }
                timelineScrollX_ = 0.0f;
                playbackSamplePosition_ = 0;
                
                // Create recording clips for all hot tracks
                for (auto& track : tracks_) {
                    if (track.isRecording) {
                        track.recordingClip = std::make_shared<MidiClip>("Recording");
                        track.recordingClip->setStartTime(0);
                    }
                }
            }
        }
    }
    ImGui::PopStyleColor(3);
    
    // Draw play triangle
    ImVec2 playCenter = ImVec2(playButtonPos.x + 20, playButtonPos.y + 20);
    ImVec2 playP1 = ImVec2(playCenter.x - 8, playCenter.y - 8);
    ImVec2 playP2 = ImVec2(playCenter.x - 8, playCenter.y + 8);
    ImVec2 playP3 = ImVec2(playCenter.x + 8, playCenter.y);
    drawList->AddTriangleFilled(playP1, playP2, playP3, IM_COL32(200, 200, 200, 255));
    
    ImGui::SameLine();
    
    // Pause button (between play and record)
    ImVec2 pauseButtonPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
    
    if (ImGui::Button("##pause", ImVec2(40, 40))) {
        isPlaying_ = false;
        isCountingIn_ = false;  // Cancel count-in if active
        
        // Stop all notes on all tracks
        for (auto& track : tracks_) {
            if (track.synth) {
                // Send all notes off (MIDI CC 123)
                for (int note = 0; note < 128; ++note) {
                    MidiMessage noteOff(MidiMessageType::NoteOff, 1, note, 0);
                    track.synth->processMidiMessage(noteOff);
                }
            }
        }
        
        // Finalize recording clips when paused
        if (masterRecord_) {
            for (auto& track : tracks_) {
                if (track.recordingClip) {
                    // Save the recording clip to the track's clips list
                    track.clips.push_back(track.recordingClip);
                    track.recordingClip.reset();  // Clear current recording
                }
            }
        }
    }
    ImGui::PopStyleColor(3);
    
    // Draw pause bars
    ImVec2 pauseCenter = ImVec2(pauseButtonPos.x + 20, pauseButtonPos.y + 20);
    drawList->AddRectFilled(
        ImVec2(pauseCenter.x - 8, pauseCenter.y - 8),
        ImVec2(pauseCenter.x - 3, pauseCenter.y + 8),
        IM_COL32(200, 200, 200, 255)
    );
    drawList->AddRectFilled(
        ImVec2(pauseCenter.x + 3, pauseCenter.y - 8),
        ImVec2(pauseCenter.x + 8, pauseCenter.y + 8),
        IM_COL32(200, 200, 200, 255)
    );
    
    ImGui::SameLine();
    
    // Master record button
    ImVec2 recButtonPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
    
    if (ImGui::Button("##master_rec", ImVec2(40, 40))) {
        masterRecord_ = !masterRecord_;
        // Don't start playback or reset timeline - just prime for recording
    }
    ImGui::PopStyleColor(3);
    
    // Draw filled circle for master record button
    ImVec2 center = ImVec2(recButtonPos.x + 20, recButtonPos.y + 20);
    float radius = 12.0f;
    ImU32 circleColor = masterRecord_ ? 
        IM_COL32(255, 0, 0, 255) : IM_COL32(120, 120, 120, 255);
    drawList->AddCircleFilled(center, radius, circleColor);
    drawList->AddCircle(center, radius + 1.0f, IM_COL32(200, 200, 200, 255), 0, 2.0f);
    
    ImGui::SameLine();
    
    // Stop button
    ImVec2 stopButtonPos = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.5f));
    
    bool stopButtonClicked = ImGui::Button("##stop", ImVec2(40, 40));
    bool stopButtonDoubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0);
    
    if (stopButtonClicked) {
        isPlaying_ = false;
        isCountingIn_ = false;  // Cancel count-in if active
        
        // Stop all notes on all tracks
        for (auto& track : tracks_) {
            if (track.synth) {
                // Send all notes off (MIDI CC 123)
                for (int note = 0; note < 128; ++note) {
                    MidiMessage noteOff(MidiMessageType::NoteOff, 1, note, 0);
                    track.synth->processMidiMessage(noteOff);
                }
            }
        }
        
        // Finalize recording clips when stopped
        if (masterRecord_) {
            for (auto& track : tracks_) {
                if (track.recordingClip) {
                    track.clips.push_back(track.recordingClip);
                    track.recordingClip.reset();
                }
            }
        }
    }
    
    if (stopButtonDoubleClicked) {
        // Return playhead to beginning
        {
            std::lock_guard<std::mutex> lock(timelineMutex_);
            timelinePosition_ = 0.0f;
        }
        timelineScrollX_ = 0.0f;
    }
    
    ImGui::PopStyleColor(3);
    
    // Draw stop square
    ImVec2 stopCenter = ImVec2(stopButtonPos.x + 20, stopButtonPos.y + 20);
    drawList->AddRectFilled(
        ImVec2(stopCenter.x - 8, stopCenter.y - 8),
        ImVec2(stopCenter.x + 8, stopCenter.y + 8),
        IM_COL32(200, 200, 200, 255)
    );
    
    // Count-in button - positioned to the right of transport buttons
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(40, 0));  // Spacing
    ImGui::SameLine();
    
    // Count-in checkbox/button
    ImVec2 countInPos = ImGui::GetCursorPos();
    bool countInChanged = ImGui::Checkbox("Count-In", &countInEnabled_);
    if (countInChanged) {
        markDirty();
    }
    
    // Show indicator when counting in
    if (isCountingIn_) {
        ImGui::SameLine();
        char countText[16];
        snprintf(countText, sizeof(countText), "(%d)", countInBeatsRemaining_);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", countText);
    }
    
    ImGui::End();
#endif
}

void MainWindow::updateTimeline() {
#ifdef PAN_USE_GUI
    if (isPlaying_ && !isDraggingPlayhead_) {  // Don't update timeline position while dragging
        // Get current time
        auto now = std::chrono::steady_clock::now();
        auto duration = now.time_since_epoch();
        double currentTime = std::chrono::duration<double>(duration).count();
        
        if (lastTime_ > 0.0) {
            double deltaTime = currentTime - lastTime_;
            // Update timeline position based on BPM
            // 1 beat = 60/BPM seconds
            float beatsPerSecond = bpm_ / 60.0f;
            {
                std::lock_guard<std::mutex> lock(timelineMutex_);
                timelinePosition_ += static_cast<float>(deltaTime * beatsPerSecond);
            }
            
            // Check if playhead has reached right edge and scroll
            const float pixelsPerBeat = 50.0f;  // Timeline scale
            float currentTimelinePos;
            {
                std::lock_guard<std::mutex> lock(timelineMutex_);
                currentTimelinePos = timelinePosition_;
            }
            float playheadX = timelineScrollX_ + (currentTimelinePos * pixelsPerBeat);
            ImGuiViewport* viewport = ImGui::GetMainViewport();
            float rightEdge = viewport->Size.x * 0.67f;  // Right pane is 2/3 of screen
            
            if (playheadX >= rightEdge - 20.0f) {
                // Scroll timeline to keep playhead visible
                timelineScrollX_ = rightEdge - (currentTimelinePos * pixelsPerBeat) - 20.0f;
            }
        }
        
        lastTime_ = currentTime;
    } else {
        lastTime_ = 0.0;
    }
#endif
}

void MainWindow::renderTrackTimeline(size_t trackIndex) {
#ifdef PAN_USE_GUI
    const float pixelsPerBeat = 50.0f;
    const float timelineHeight = 120.0f;  // Height per track timeline - make it more visible
    const float beatMarkerHeight = 25.0f;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 600.0f);  // Minimum width - make it wider
    canvasSize.y = timelineHeight;
    
    // Draw timeline background (z-order 1.5 - same as track header or slightly above)
    // Use same color as track header for consistency
    ImU32 bgColor = (trackIndex % 2 == 0) ? IM_COL32(70, 75, 80, 255) : IM_COL32(75, 80, 85, 255);
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), bgColor);
    
    // Draw beat markers at top
    float visibleBeats = canvasSize.x / pixelsPerBeat;
    float startBeat = std::floor(timelineScrollX_ / pixelsPerBeat);
    float endBeat = startBeat + visibleBeats + 1.0f;
    
    for (float beat = startBeat; beat <= endBeat; beat += 1.0f) {
        float x = canvasPos.x + (beat * pixelsPerBeat) - timelineScrollX_;
        if (x >= canvasPos.x && x <= canvasPos.x + canvasSize.x) {
            // Major beat marker
            drawList->AddLine(
                ImVec2(x, canvasPos.y),
                ImVec2(x, canvasPos.y + beatMarkerHeight),
                IM_COL32(150, 150, 150, 255)
            );
            
            // Beat number (only show every 4 beats to avoid clutter)
            if (static_cast<int>(beat) % 4 == 0) {
                char beatLabel[16];
                snprintf(beatLabel, sizeof(beatLabel), "%.0f", beat);
                drawList->AddText(ImVec2(x + 2, canvasPos.y + 2), IM_COL32(200, 200, 200, 255), beatLabel);
            }
        }
    }
    
    // Draw track lane background - use same color as track header
    ImU32 laneColor = (trackIndex % 2 == 0) ? IM_COL32(35, 35, 35, 255) : IM_COL32(40, 40, 40, 255);
    drawList->AddRectFilled(
        ImVec2(canvasPos.x, canvasPos.y + beatMarkerHeight),
        ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
        laneColor
    );
    
    // For Track 1, the timeline area should match the header background color
    // The darker background will be drawn below Track 1, covering the area where Track 2 would be
    
    // Draw recorded MIDI clips/notes
    auto& track = tracks_[trackIndex];
    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
    float beatsPerSecond = bpm_ / 60.0f;
    float samplesPerBeat = sampleRate / beatsPerSecond;
    
    // Draw all clips (both recording and completed)
    std::vector<std::shared_ptr<MidiClip>> clipsToDraw = track.clips;
    if (track.recordingClip) {
        clipsToDraw.push_back(track.recordingClip);
    }
    
    // Debug: print clip info occasionally
    static int debugFrameCount = 0;
    if (++debugFrameCount % 300 == 0 && trackIndex == 0) {  // Every 5 seconds at 60fps
        std::cout << "Timeline render: Track " << trackIndex 
                  << " has " << track.clips.size() << " completed clips, "
                  << (track.recordingClip ? "1" : "0") << " recording clip" << std::endl;
        if (track.recordingClip) {
            std::cout << "  Recording clip has " << track.recordingClip->getEvents().size() << " events" << std::endl;
        }
        for (size_t i = 0; i < track.clips.size(); ++i) {
            std::cout << "  Clip " << i << " has " << track.clips[i]->getEvents().size() << " events" << std::endl;
        }
    }
    
    // Build note list: all note-on/note-off pairs
    struct NoteRect {
        uint8_t noteNum;
        float startBeat;
        float endBeat;
        uint8_t velocity;
    };
    std::vector<NoteRect> notesToDraw;
    
    for (auto& clip : clipsToDraw) {
        const auto& events = clip->getEvents();
        float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
        
        // Track active notes for this clip
        std::map<uint8_t, std::pair<float, uint8_t>> activeNotes;  // noteNum -> (startBeat, velocity)
        
        for (const auto& event : events) {
            // Convert sample timestamp to beat position (relative to clip start)
            float beatPos = static_cast<float>(event.timestamp) / samplesPerBeat;
            float absoluteBeatPos = clipStartBeat + beatPos;
            
            if (event.message.isNoteOn()) {
                uint8_t noteNum = event.message.getNoteNumber();
                activeNotes[noteNum] = {absoluteBeatPos, event.message.getVelocity()};
            } else if (event.message.isNoteOff()) {
                uint8_t noteNum = event.message.getNoteNumber();
                if (activeNotes.find(noteNum) != activeNotes.end()) {
                    // Found matching note-on, create note rectangle
                    notesToDraw.push_back({
                        noteNum,
                        activeNotes[noteNum].first,
                        absoluteBeatPos,
                        activeNotes[noteNum].second
                    });
                    activeNotes.erase(noteNum);
                }
            }
        }
        
        // For any notes still active (no note-off yet), extend to current timeline position
        float currentTimelinePos;
        {
            std::lock_guard<std::mutex> lock(timelineMutex_);
            currentTimelinePos = timelinePosition_;
        }
        for (const auto& [noteNum, noteData] : activeNotes) {
            notesToDraw.push_back({
                noteNum,
                noteData.first,
                std::max(noteData.first + 0.25f, currentTimelinePos),  // Minimum 1/4 beat or extend to playhead
                noteData.second
            });
        }
    }
    
    // Find the range of notes to auto-center the view
    uint8_t minNote = 127;
    uint8_t maxNote = 0;
    for (const auto& note : notesToDraw) {
        minNote = std::min(minNote, note.noteNum);
        maxNote = std::max(maxNote, note.noteNum);
    }
    
    // If no notes, use a default center (middle C = 60)
    if (notesToDraw.empty()) {
        minNote = 48;  // C3
        maxNote = 72;  // C5
    }
    
    // Calculate center note and visible range (show extra notes above/below for context)
    float centerNote = (minNote + maxNote) / 2.0f;
    float visibleNoteRange = 24.0f;  // Show 24 semitones (2 octaves) centered around played notes
    
    // Draw notes as rectangles
    float laneHeight = canvasSize.y - beatMarkerHeight - 20.0f;
    float pixelsPerNote = laneHeight / visibleNoteRange;  // Pixels per semitone
    float noteHeight = std::max(12.0f, pixelsPerNote * 0.8f);  // 80% of space for note, 20% for gap
    
    for (const auto& note : notesToDraw) {
        float startX = canvasPos.x + (note.startBeat * pixelsPerBeat) - timelineScrollX_;
        float endX = canvasPos.x + (note.endBeat * pixelsPerBeat) - timelineScrollX_;
        float noteWidth = endX - startX;
        
        // Only draw if visible (minimum width of 2px for very short notes)
        if (endX >= canvasPos.x && startX <= canvasPos.x + canvasSize.x) {
            // Map note number to vertical position, centered around the played notes
            // Center note is in the middle of the lane, other notes are relative to it
            float noteOffsetFromCenter = centerNote - note.noteNum;
            float noteY = canvasPos.y + beatMarkerHeight + 10.0f + (laneHeight / 2.0f) + (noteOffsetFromCenter * pixelsPerNote);
            
            // Color based on note velocity (brighter = higher velocity)
            uint8_t velocity = note.velocity;
            ImU32 noteColor = IM_COL32(100 + velocity / 2, 150 + velocity / 3, 200 + velocity / 4, 255);
            
            // Draw rectangle for the note (ensure minimum width of 3px)
            float drawEndX = std::max(endX, startX + 3.0f);
            drawList->AddRectFilled(
                ImVec2(startX, noteY),
                ImVec2(drawEndX, noteY + noteHeight),
                noteColor
            );
            // Add border for visibility
            drawList->AddRect(
                ImVec2(startX, noteY),
                ImVec2(drawEndX, noteY + noteHeight),
                IM_COL32(255, 255, 255, 200),
                0.0f, 0, 1.0f
            );
        }
    }
    
    // Draw playhead (vertical line) - draw AFTER background so it appears on top
    float currentTimelinePos;
    {
        std::lock_guard<std::mutex> lock(timelineMutex_);
        currentTimelinePos = timelinePosition_;
    }
    float playheadX = canvasPos.x + (currentTimelinePos * pixelsPerBeat) - timelineScrollX_;
    if (playheadX >= canvasPos.x && playheadX <= canvasPos.x + canvasSize.x) {
        // Use ivory color to match the color scheme: RGB(250, 250, 240)
        drawList->AddLine(
            ImVec2(playheadX, canvasPos.y),
            ImVec2(playheadX, canvasPos.y + canvasSize.y),
            IM_COL32(250, 250, 240, 255),  // Ivory playhead
            2.0f
        );
    }
    
    // Set cursor for scrolling
    ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y));
    ImGui::InvisibleButton("timeline_canvas", canvasSize);
    
    // Handle horizontal scrolling with mouse wheel (only on first track to avoid conflicts)
    if (trackIndex == 0 && ImGui::IsItemHovered()) {
        float scroll = ImGui::GetIO().MouseWheelH;
        if (scroll != 0.0f) {
            timelineScrollX_ = std::max(0.0f, timelineScrollX_ - scroll * 20.0f);
        }
    }
#endif
}

void MainWindow::markDirty() {
    hasUnsavedChanges_ = true;
}

bool MainWindow::checkUnsavedChanges() {
    return !hasUnsavedChanges_;
}

void MainWindow::newProject() {
    // Reset to defaults
    tracks_.clear();
    tracks_.resize(1);
    for (auto& track : tracks_) {
        track.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
        track.synth->setVolume(0.5f);
        track.synth->setOscillators(track.oscillators);
        track.isRecording = true;  // Auto-select first track
    }
    
    selectedTrackIndex_ = 0;
    bpm_ = 120.0f;
    timelinePosition_ = 0.0f;
    timelineScrollX_ = 0.0f;
    isPlaying_ = false;
    masterRecord_ = false;
    currentProjectPath_ = "";
    hasUnsavedChanges_ = false;
}

std::string MainWindow::serializeProject() const {
    std::string data;
    
    // Format: Simple text-based format
    // Line 1: BPM
    // Line 2: Number of tracks
    // For each track:
    //   - Number of oscillators
    //   - For each oscillator: waveform,freq_mult,amplitude
    //   - Number of clips
    //   - For each clip: ... (simplified for now, just save count)
    
    data += std::to_string(bpm_) + "\n";
    data += std::to_string(tracks_.size()) + "\n";
    
    for (const auto& track : tracks_) {
        data += std::to_string(track.oscillators.size()) + "\n";
        for (const auto& osc : track.oscillators) {
            data += std::to_string(static_cast<int>(osc.waveform)) + ",";
            data += std::to_string(osc.frequencyMultiplier) + ",";
            data += std::to_string(osc.amplitude) + "\n";
        }
        // For now, don't serialize clips (TODO: implement MIDI clip serialization)
        data += "0\n";  // Number of clips
    }
    
    return data;
}

bool MainWindow::deserializeProject(const std::string& data) {
    std::istringstream stream(data);
    std::string line;
    
    try {
        // Read BPM
        std::getline(stream, line);
        bpm_ = std::stof(line);
        
        // Read number of tracks
        std::getline(stream, line);
        size_t numTracks = std::stoul(line);
        
        tracks_.clear();
        tracks_.resize(numTracks);
        
        for (size_t i = 0; i < numTracks; ++i) {
            auto& track = tracks_[i];
            track.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
            track.synth->setVolume(0.5f);
            
            // Read number of oscillators
            std::getline(stream, line);
            size_t numOscs = std::stoul(line);
            
            track.oscillators.clear();
            for (size_t j = 0; j < numOscs; ++j) {
                std::getline(stream, line);
                std::istringstream oscStream(line);
                std::string waveStr, freqStr, ampStr;
                
                std::getline(oscStream, waveStr, ',');
                std::getline(oscStream, freqStr, ',');
                std::getline(oscStream, ampStr, ',');
                
                Waveform wave = static_cast<Waveform>(std::stoi(waveStr));
                float freq = std::stof(freqStr);
                float amp = std::stof(ampStr);
                
                track.oscillators.push_back(Oscillator(wave, freq, amp));
            }
            
            track.synth->setOscillators(track.oscillators);
            
            // Read number of clips (ignore for now)
            std::getline(stream, line);
        }
        
        selectedTrackIndex_ = 0;
        hasUnsavedChanges_ = false;
        return true;
    } catch (...) {
        return false;
    }
}

bool MainWindow::saveProject() {
    if (currentProjectPath_.empty()) {
        return saveProjectAs();
    }
    
    std::string data = serializeProject();
    std::ofstream file(currentProjectPath_);
    if (file.is_open()) {
        file << data;
        file.close();
        hasUnsavedChanges_ = false;
        return true;
    }
    
    return false;
}

bool MainWindow::saveProjectAs() {
    // This should only be called from the menu, which handles the dialog
    // Don't call saveProject() here as it would cause infinite recursion
    return false;
}

bool MainWindow::openProject() {
    // Handled in renderMenuBar
    return false;
}

void MainWindow::generateClickSound(AudioBuffer& buffer, size_t numFrames, bool isAccent) {
    // Generate a simple click sound (short sine wave beep)
    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
    float frequency = isAccent ? 1200.0f : 800.0f;  // Higher pitch for accent
    float amplitude = isAccent ? 0.3f : 0.2f;
    size_t clickDuration = static_cast<size_t>(sampleRate * 0.05);  // 50ms click
    
    for (size_t ch = 0; ch < buffer.getNumChannels(); ++ch) {
        float* samples = buffer.getWritePointer(ch);
        for (size_t i = 0; i < std::min(clickDuration, numFrames); ++i) {
            float t = static_cast<float>(i) / sampleRate;
            float envelope = 1.0f - (static_cast<float>(i) / clickDuration);  // Decay envelope
            samples[i] = amplitude * envelope * std::sin(2.0f * M_PI * frequency * t);
        }
    }
}

float MainWindow::snapToGrid(float beat) const {
    if (!gridSnapEnabled_) {
        return beat;
    }
    
    float subdivision = 0.0f;
    switch (currentGridDivision_) {
        case GridDivision::Whole:           subdivision = 4.0f; break;
        case GridDivision::Half:            subdivision = 2.0f; break;
        case GridDivision::Quarter:         subdivision = 1.0f; break;
        case GridDivision::Eighth:          subdivision = 0.5f; break;
        case GridDivision::Sixteenth:       subdivision = 0.25f; break;
        case GridDivision::ThirtySecond:    subdivision = 0.125f; break;
        case GridDivision::QuarterTriplet:  subdivision = 4.0f / 3.0f; break;  // 3 per bar
        case GridDivision::EighthTriplet:   subdivision = 2.0f / 3.0f; break;  // 6 per bar
        case GridDivision::SixteenthTriplet: subdivision = 1.0f / 3.0f; break;  // 12 per bar
    }
    
    return std::round(beat / subdivision) * subdivision;
}

const char* MainWindow::getGridDivisionName(GridDivision division) const {
    switch (division) {
        case GridDivision::Whole:           return "1/1 (Whole)";
        case GridDivision::Half:            return "1/2 (Half)";
        case GridDivision::Quarter:         return "1/4 (Quarter)";
        case GridDivision::Eighth:          return "1/8 (Eighth)";
        case GridDivision::Sixteenth:       return "1/16 (Sixteenth)";
        case GridDivision::ThirtySecond:    return "1/32 (32nd)";
        case GridDivision::QuarterTriplet:  return "1/4T (Quarter Triplet)";
        case GridDivision::EighthTriplet:   return "1/8T (Eighth Triplet)";
        case GridDivision::SixteenthTriplet: return "1/16T (16th Triplet)";
        default: return "Unknown";
    }
}

void MainWindow::quantizeSelectedTrack() {
    if (selectedTrackIndex_ >= tracks_.size()) return;
    
    auto& track = tracks_[selectedTrackIndex_];
    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
    float beatsPerSecond = bpm_ / 60.0f;
    float samplesPerBeat = sampleRate / beatsPerSecond;
    
    // Quantize all clips in the selected track
    for (auto& clip : track.clips) {
        auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
        float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
        
        // Quantize note-on events
        for (auto& event : events) {
            if (event.message.isNoteOn()) {
                // Convert timestamp to beats
                float beatPos = static_cast<float>(event.timestamp) / samplesPerBeat + clipStartBeat;
                
                // Snap to grid
                float quantizedBeat = snapToGrid(beatPos);
                
                // Convert back to samples relative to clip start
                int64_t newTimestamp = static_cast<int64_t>((quantizedBeat - clipStartBeat) * samplesPerBeat);
                event.timestamp = std::max(static_cast<int64_t>(0), newTimestamp);
            }
        }
        
        // Sort events by timestamp after quantization
        std::sort(events.begin(), events.end(), 
                 [](const MidiClip::MidiEvent& a, const MidiClip::MidiEvent& b) {
                     return a.timestamp < b.timestamp;
                 });
    }
    
    markDirty();
    std::cout << "Quantized track " << (selectedTrackIndex_ + 1) << " to " 
              << getGridDivisionName(currentGridDivision_) << std::endl;
}

void MainWindow::renderPianoRoll() {
#ifdef PAN_USE_GUI
    ImGui::Begin("Piano Roll", nullptr, ImGuiWindowFlags_None);
    
    pianoRollActive_ = ImGui::IsWindowFocused();
    
    // Toolbar: Grid division, snap toggle, pencil tool
    ImGui::Text("Grid:");
    ImGui::SameLine();
    
    // Set combo width to fit the longest text with some padding
    ImGui::SetNextItemWidth(180.0f);
    
    if (ImGui::BeginCombo("##grid", getGridDivisionName(currentGridDivision_))) {
        GridDivision divisions[] = {
            GridDivision::Whole, GridDivision::Half, GridDivision::Quarter, 
            GridDivision::Eighth, GridDivision::Sixteenth, GridDivision::ThirtySecond,
            GridDivision::QuarterTriplet, GridDivision::EighthTriplet, GridDivision::SixteenthTriplet
        };
        
        for (const auto& div : divisions) {
            if (ImGui::Selectable(getGridDivisionName(div), currentGridDivision_ == div)) {
                currentGridDivision_ = div;
            }
        }
        ImGui::EndCombo();
    }
    
    ImGui::SameLine();
    if (ImGui::Checkbox("Snap", &gridSnapEnabled_)) {
        // Right-click also toggles, but checkbox is more discoverable
    }
    
    ImGui::SameLine();
    ImGui::Checkbox("Draw (Ctrl+D)", &pencilToolActive_);
    
    ImGui::SameLine();
    ImGui::Text("Selected Track: %zu", selectedTrackIndex_ + 1);
    
    ImGui::Separator();
    
    // Check if we have tracks and a selected track
    if (tracks_.empty() || selectedTrackIndex_ >= tracks_.size()) {
        ImGui::Text("No track selected");
        ImGui::End();
        return;
    }
    
    auto& track = tracks_[selectedTrackIndex_];
    
    // Piano roll canvas
    const float pixelsPerBeat = 50.0f;
    const float pianoKeyWidth = 60.0f;
    const float noteHeight = 12.0f;  // Height per MIDI note
    const int numOctaves = 4;  // Show 4 octaves at a time
    const int totalNotes = numOctaves * 12;  // 48 notes (4 octaves)
    const int lowestNote = std::max(0, pianoRollCenterNote_ - totalNotes / 2);  // Center around played notes
    
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.y = std::max(canvasSize.y, totalNotes * noteHeight);
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), 
                           IM_COL32(25, 25, 25, 255));
    
    // Draw piano keys on the left
    for (int i = 0; i < totalNotes; ++i) {
        int noteNum = lowestNote + (totalNotes - 1 - i);
        int noteInOctave = noteNum % 12;
        float y = canvasPos.y + i * noteHeight;
        
        // Black keys are darker
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || 
                          noteInOctave == 8 || noteInOctave == 10);
        ImU32 keyColor = isBlackKey ? IM_COL32(40, 40, 40, 255) : IM_COL32(200, 200, 200, 255);
        
        drawList->AddRectFilled(ImVec2(canvasPos.x, y), 
                               ImVec2(canvasPos.x + pianoKeyWidth, y + noteHeight),
                               keyColor);
        drawList->AddRect(ImVec2(canvasPos.x, y), 
                         ImVec2(canvasPos.x + pianoKeyWidth, y + noteHeight),
                         IM_COL32(100, 100, 100, 255));
        
        // Draw note name on C notes
        if (noteInOctave == 0) {
            char noteName[8];
            snprintf(noteName, sizeof(noteName), "C%d", noteNum / 12 - 1);
            drawList->AddText(ImVec2(canvasPos.x + 5, y + 1), 
                             isBlackKey ? IM_COL32(200, 200, 200, 255) : IM_COL32(0, 0, 0, 255), 
                             noteName);
        }
    }
    
    // Draw grid lines (beats and divisions)
    float gridStart = canvasPos.x + pianoKeyWidth;
    float gridWidth = canvasSize.x - pianoKeyWidth;
    int visibleBeats = static_cast<int>(gridWidth / pixelsPerBeat) + 2;
    
    // Calculate subdivision size
    float subdivisionBeats = 0.25f;  // Default to sixteenth notes
    switch (currentGridDivision_) {
        case GridDivision::Whole:           subdivisionBeats = 4.0f; break;
        case GridDivision::Half:            subdivisionBeats = 2.0f; break;
        case GridDivision::Quarter:         subdivisionBeats = 1.0f; break;
        case GridDivision::Eighth:          subdivisionBeats = 0.5f; break;
        case GridDivision::Sixteenth:       subdivisionBeats = 0.25f; break;
        case GridDivision::ThirtySecond:    subdivisionBeats = 0.125f; break;
        case GridDivision::QuarterTriplet:  subdivisionBeats = 4.0f / 3.0f; break;
        case GridDivision::EighthTriplet:   subdivisionBeats = 2.0f / 3.0f; break;
        case GridDivision::SixteenthTriplet: subdivisionBeats = 1.0f / 3.0f; break;
    }
    
    float subdivisionPixels = subdivisionBeats * pixelsPerBeat;
    int totalSubdivisions = static_cast<int>(gridWidth / subdivisionPixels) + 2;
    
    // Draw subdivision lines
    for (int i = 0; i < totalSubdivisions; ++i) {
        float x = gridStart + i * subdivisionPixels;
        float beatPos = i * subdivisionBeats;
        
        // Determine line strength based on position
        bool isBar = (std::abs(std::fmod(beatPos, 4.0f)) < 0.001f);
        bool isBeat = (std::abs(std::fmod(beatPos, 1.0f)) < 0.001f);
        
        ImU32 lineColor;
        float lineThickness;
        
        if (isBar) {
            // Bar line (every 4 beats)
            lineColor = IM_COL32(100, 100, 100, 255);
            lineThickness = 2.0f;
        } else if (isBeat) {
            // Beat line
            lineColor = IM_COL32(80, 80, 80, 255);
            lineThickness = 1.5f;
        } else {
            // Subdivision line
            lineColor = IM_COL32(60, 60, 60, 255);
            lineThickness = 1.0f;
        }
        
        drawList->AddLine(ImVec2(x, canvasPos.y), 
                         ImVec2(x, canvasPos.y + canvasSize.y),
                         lineColor, lineThickness);
        
        // Draw bar number on bar lines
        if (isBar) {
            char beatLabel[8];
            snprintf(beatLabel, sizeof(beatLabel), "%d", static_cast<int>(beatPos / 4.0f) + 1);
            drawList->AddText(ImVec2(x + 2, canvasPos.y + 2), IM_COL32(150, 150, 150, 255), beatLabel);
        }
    }
    
    // Draw real-time notes being played (live visualization)
    {
        std::lock_guard<std::mutex> lock(notesMutex_);
        for (int noteNum = 0; noteNum < 128; ++noteNum) {
            if (notesPlaying_[noteNum]) {
                if (noteNum >= lowestNote && noteNum < lowestNote + totalNotes) {
                    int noteIndex = totalNotes - 1 - (noteNum - lowestNote);
                    float noteY = canvasPos.y + noteIndex * noteHeight;
                    
                    // Highlight the piano key
                    drawList->AddRectFilled(ImVec2(canvasPos.x, noteY), 
                                           ImVec2(canvasPos.x + pianoKeyWidth, noteY + noteHeight),
                                           IM_COL32(100, 200, 100, 200));  // Green highlight for active keys
                }
            }
        }
    }
    
    // Draw existing notes from clips
    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
    float beatsPerSecond = bpm_ / 60.0f;
    float samplesPerBeat = sampleRate / beatsPerSecond;
    
    struct NoteRect {
        float startBeat, endBeat;
        uint8_t noteNum, velocity;
        size_t clipIndex, eventIndex;
    };
    std::vector<NoteRect> noteRects;
    
    for (size_t clipIdx = 0; clipIdx < track.clips.size(); ++clipIdx) {
        const auto& clip = track.clips[clipIdx];
        const auto& events = clip->getEvents();
        float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
        
        // Build note rectangles
        std::map<uint8_t, std::pair<float, size_t>> activeNotes;  // noteNum -> (startBeat, eventIndex)
        
        for (size_t evtIdx = 0; evtIdx < events.size(); ++evtIdx) {
            const auto& event = events[evtIdx];
            float beatPos = static_cast<float>(event.timestamp) / samplesPerBeat + clipStartBeat;
            
            if (event.message.isNoteOn()) {
                uint8_t noteNum = event.message.getNoteNumber();
                activeNotes[noteNum] = {beatPos, evtIdx};
            } else if (event.message.isNoteOff()) {
                uint8_t noteNum = event.message.getNoteNumber();
                if (activeNotes.find(noteNum) != activeNotes.end()) {
                    float startBeat = activeNotes[noteNum].first;
                    size_t startEventIdx = activeNotes[noteNum].second;
                    float endBeat = beatPos;
                    uint8_t velocity = events[startEventIdx].message.getVelocity();
                    
                    noteRects.push_back({startBeat, endBeat, noteNum, velocity, clipIdx, startEventIdx});
                    activeNotes.erase(noteNum);
                }
            }
        }
    }
    
    // Draw all notes
    for (const auto& note : noteRects) {
        if (note.noteNum >= lowestNote && note.noteNum < lowestNote + totalNotes) {
            // Check if this note is selected and being dragged
            bool isSelected = selectedNotes_.find({note.clipIndex, note.eventIndex}) != selectedNotes_.end();
            bool isBeingDragged = isSelected && draggedNote_.isDragging;
            
            // Calculate position (with drag offset if applicable)
            float displayStartBeat = note.startBeat;
            uint8_t displayNoteNum = note.noteNum;
            
            if (isBeingDragged) {
                displayStartBeat += draggedNote_.currentBeatDelta;
                displayNoteNum = std::clamp(static_cast<int>(note.noteNum) + draggedNote_.currentNoteDelta, 0, 127);
            }
            
            // Check if display note is in visible range
            if (displayNoteNum >= lowestNote && displayNoteNum < lowestNote + totalNotes) {
                int noteIndex = totalNotes - 1 - (displayNoteNum - lowestNote);
                float noteY = canvasPos.y + noteIndex * noteHeight;
                float noteX = gridStart + displayStartBeat * pixelsPerBeat;
                float noteW = (note.endBeat - note.startBeat) * pixelsPerBeat;
                
                ImU32 noteColor = isSelected ? 
                    IM_COL32(255, 200, 100, 255) :  // Orange for selected notes
                    IM_COL32(100 + note.velocity / 2, 150 + note.velocity / 3, 200 + note.velocity / 4, 255);
                
                // If being dragged, make it slightly transparent
                if (isBeingDragged) {
                    noteColor = (noteColor & 0x00FFFFFF) | 0xCC000000;  // Set alpha to 0xCC (80%)
                }
                
                drawList->AddRectFilled(ImVec2(noteX, noteY + 1), 
                                       ImVec2(noteX + noteW, noteY + noteHeight - 1),
                                       noteColor);
                drawList->AddRect(ImVec2(noteX, noteY + 1), 
                                 ImVec2(noteX + noteW, noteY + noteHeight - 1),
                                 isSelected ? IM_COL32(255, 255, 100, 255) : IM_COL32(255, 255, 255, 200),
                                 0.0f, 0, isSelected ? 2.0f : 1.0f);
            }
        }
    }
    
    // Handle note dragging and interaction
    ImVec2 mousePos = ImGui::GetMousePos();
    bool mouseOverCanvas = (mousePos.x >= gridStart && mousePos.x < gridStart + gridWidth &&
                           mousePos.y >= canvasPos.y && mousePos.y < canvasPos.y + canvasSize.y);
    
    // Calculate hovered note and beat
    int hoveredNoteIndex = -1;
    float hoveredBeat = 0.0f;
    if (mouseOverCanvas) {
        hoveredNoteIndex = static_cast<int>((mousePos.y - canvasPos.y) / noteHeight);
        pianoRollHoverNote_ = lowestNote + (totalNotes - 1 - hoveredNoteIndex);
        hoveredBeat = (mousePos.x - gridStart) / pixelsPerBeat;
    }
    
    // Check if mouse is over any note (only when not in draw mode and not drawing)
    int hoveredNoteIdx = -1;
    bool hoverOnLeftEdge = false;
    bool hoverOnRightEdge = false;
    const float edgeThreshold = 8.0f;  // Pixels from edge to trigger resize
    
    if (mouseOverCanvas && !draggedNote_.isDragging && !resizingNote_.isResizing && !pencilToolActive_ && !drawingNote_.isDrawing) {
        for (int i = noteRects.size() - 1; i >= 0; --i) {
            const auto& note = noteRects[i];
            if (note.noteNum >= lowestNote && note.noteNum < lowestNote + totalNotes) {
                int noteIndex = totalNotes - 1 - (note.noteNum - lowestNote);
                float noteY = canvasPos.y + noteIndex * noteHeight;
                float noteX = gridStart + note.startBeat * pixelsPerBeat;
                float noteW = (note.endBeat - note.startBeat) * pixelsPerBeat;
                
                if (mousePos.x >= noteX && mousePos.x <= noteX + noteW &&
                    mousePos.y >= noteY && mousePos.y <= noteY + noteHeight) {
                    hoveredNoteIdx = i;
                    
                    // Check if near edges
                    if (mousePos.x <= noteX + edgeThreshold) {
                        hoverOnLeftEdge = true;
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    } else if (mousePos.x >= noteX + noteW - edgeThreshold) {
                        hoverOnRightEdge = true;
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    } else {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    }
                    break;
                }
            }
        }
    }
    
    // Start box selection, note resizing, or note dragging (only when not in draw mode)
    if (ImGui::IsMouseClicked(0) && mouseOverCanvas && !pencilToolActive_ && !drawingNote_.isDrawing) {
        if (hoveredNoteIdx >= 0 && (hoverOnLeftEdge || hoverOnRightEdge)) {
            // Start resizing note
            resizingNote_.isResizing = true;
            resizingNote_.isLeftEdge = hoverOnLeftEdge;
            resizingNote_.clipIndex = noteRects[hoveredNoteIdx].clipIndex;
            resizingNote_.eventIndex = noteRects[hoveredNoteIdx].eventIndex;
            resizingNote_.originalStartBeat = noteRects[hoveredNoteIdx].startBeat;
            resizingNote_.originalEndBeat = noteRects[hoveredNoteIdx].endBeat;
            resizingNote_.noteNum = noteRects[hoveredNoteIdx].noteNum;
        } else if (hoveredNoteIdx >= 0) {
            // Check if clicked note is already in selection
            std::pair<size_t, size_t> clickedNote = {noteRects[hoveredNoteIdx].clipIndex, noteRects[hoveredNoteIdx].eventIndex};
            bool noteAlreadySelected = (selectedNotes_.find(clickedNote) != selectedNotes_.end());
            
            // If shift not held and note is not already selected, clear selection
            if (!ImGui::GetIO().KeyShift && !noteAlreadySelected) {
                selectedNotes_.clear();
            }
            
            // Add this note to selection
            selectedNotes_.insert(clickedNote);
            
            // Start dragging
            draggedNote_.isDragging = true;
            draggedNote_.clipIndex = noteRects[hoveredNoteIdx].clipIndex;
            draggedNote_.eventIndex = noteRects[hoveredNoteIdx].eventIndex;
            draggedNote_.startBeat = noteRects[hoveredNoteIdx].startBeat;
            draggedNote_.startNote = noteRects[hoveredNoteIdx].noteNum;
            draggedNote_.noteDuration = noteRects[hoveredNoteIdx].endBeat - noteRects[hoveredNoteIdx].startBeat;
            draggedNote_.currentBeatDelta = 0.0f;
            draggedNote_.currentNoteDelta = 0;
            // Store where within the note the user clicked (offset from note's start position)
            draggedNote_.clickOffsetBeat = hoveredBeat - noteRects[hoveredNoteIdx].startBeat;
            draggedNote_.clickOffsetNote = pianoRollHoverNote_ - noteRects[hoveredNoteIdx].noteNum;
        } else {
            // Start box selection in empty space
            boxSelection_.isSelecting = true;
            boxSelection_.startX = mousePos.x;
            boxSelection_.startY = mousePos.y;
            boxSelection_.currentX = mousePos.x;
            boxSelection_.currentY = mousePos.y;
            
            // Clear selection if shift not held
            if (!ImGui::GetIO().KeyShift) {
                selectedNotes_.clear();
            }
        }
    }
    
    // Handle box selection
    if (boxSelection_.isSelecting) {
        if (ImGui::IsMouseDragging(0)) {
            boxSelection_.currentX = mousePos.x;
            boxSelection_.currentY = mousePos.y;
            
            // Update selection based on box
            selectedNotes_.clear();
            
            // Calculate box bounds
            float boxMinX = std::min(boxSelection_.startX, boxSelection_.currentX);
            float boxMaxX = std::max(boxSelection_.startX, boxSelection_.currentX);
            float boxMinY = std::min(boxSelection_.startY, boxSelection_.currentY);
            float boxMaxY = std::max(boxSelection_.startY, boxSelection_.currentY);
            
            // Check which notes are in the box
            for (const auto& note : noteRects) {
                if (note.noteNum >= lowestNote && note.noteNum < lowestNote + totalNotes) {
                    int noteIndex = totalNotes - 1 - (note.noteNum - lowestNote);
                    float noteY = canvasPos.y + noteIndex * noteHeight;
                    float noteX = gridStart + note.startBeat * pixelsPerBeat;
                    float noteW = (note.endBeat - note.startBeat) * pixelsPerBeat;
                    
                    // Check if note overlaps with selection box
                    if (noteX + noteW >= boxMinX && noteX <= boxMaxX &&
                        noteY + noteHeight >= boxMinY && noteY <= boxMaxY) {
                        selectedNotes_.insert({note.clipIndex, note.eventIndex});
                    }
                }
            }
        } else if (ImGui::IsMouseReleased(0)) {
            boxSelection_.isSelecting = false;
        }
        
        // Draw dotted selection box
        if (boxSelection_.isSelecting) {
            ImVec2 boxMin(std::min(boxSelection_.startX, boxSelection_.currentX),
                         std::min(boxSelection_.startY, boxSelection_.currentY));
            ImVec2 boxMax(std::max(boxSelection_.startX, boxSelection_.currentX),
                         std::max(boxSelection_.startY, boxSelection_.currentY));
            
            // Draw dotted outline
            const float dashLength = 5.0f;
            const float gapLength = 3.0f;
            const float totalLength = dashLength + gapLength;
            
            // Top and bottom
            for (float x = boxMin.x; x < boxMax.x; x += totalLength) {
                float endX = std::min(x + dashLength, boxMax.x);
                drawList->AddLine(ImVec2(x, boxMin.y), ImVec2(endX, boxMin.y), IM_COL32(255, 255, 255, 200), 2.0f);
                drawList->AddLine(ImVec2(x, boxMax.y), ImVec2(endX, boxMax.y), IM_COL32(255, 255, 255, 200), 2.0f);
            }
            
            // Left and right
            for (float y = boxMin.y; y < boxMax.y; y += totalLength) {
                float endY = std::min(y + dashLength, boxMax.y);
                drawList->AddLine(ImVec2(boxMin.x, y), ImVec2(boxMin.x, endY), IM_COL32(255, 255, 255, 200), 2.0f);
                drawList->AddLine(ImVec2(boxMax.x, y), ImVec2(boxMax.x, endY), IM_COL32(255, 255, 255, 200), 2.0f);
            }
            
            // Semi-transparent fill
            drawList->AddRectFilled(boxMin, boxMax, IM_COL32(255, 255, 255, 30));
        }
    }
    
    // Handle dragging (moves all selected notes together)
    if (draggedNote_.isDragging) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        
        if (ImGui::IsMouseDragging(0) || ImGui::IsMouseDown(0)) {
            if (mouseOverCanvas) {
                float newBeat = hoveredBeat;
                uint8_t newNote = pianoRollHoverNote_;
                
                // Apply the click offset so the note doesn't jump
                // The note's start should be at (cursor - offset)
                float targetNoteBeat = newBeat - draggedNote_.clickOffsetBeat;
                int targetNoteNum = newNote - draggedNote_.clickOffsetNote;
                
                // Snap to grid if enabled (snap the note's start position, not the cursor)
                if (gridSnapEnabled_) {
                    targetNoteBeat = snapToGrid(targetNoteBeat);
                }
                
                // Calculate deltas from the originally clicked note (but don't apply yet)
                draggedNote_.currentBeatDelta = targetNoteBeat - draggedNote_.startBeat;
                draggedNote_.currentNoteDelta = targetNoteNum - draggedNote_.startNote;
            }
        } else if (ImGui::IsMouseReleased(0)) {
            // Now apply the changes to ALL selected notes
            for (const auto& [clipIdx, eventIdx] : selectedNotes_) {
                if (clipIdx < track.clips.size()) {
                    auto& clip = track.clips[clipIdx];
                    auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
                    
                    if (eventIdx < events.size() && events[eventIdx].message.isNoteOn()) {
                        auto& noteOnEvent = events[eventIdx];
                        
                        // Get original note data
                        uint8_t oldNote = noteOnEvent.message.getNoteNumber();
                        int64_t oldTimestamp = noteOnEvent.timestamp;
                        
                        // Calculate new note number (clamped to valid MIDI range)
                        uint8_t newNoteNum = std::clamp(static_cast<int>(oldNote) + draggedNote_.currentNoteDelta, 0, 127);
                        
                        // Calculate new timestamp
                        float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
                        float oldBeat = static_cast<float>(oldTimestamp) / samplesPerBeat + clipStartBeat;
                        float adjustedNewBeat = oldBeat + draggedNote_.currentBeatDelta;
                        int64_t newTimestamp = static_cast<int64_t>((adjustedNewBeat - clipStartBeat) * samplesPerBeat);
                        
                        // Update note-on event
                        noteOnEvent.timestamp = std::max(static_cast<int64_t>(0), newTimestamp);
                        noteOnEvent.message = MidiMessage(MidiMessageType::NoteOn, 1, newNoteNum, noteOnEvent.message.getVelocity());
                        
                        // Find and update corresponding note-off
                        for (size_t i = eventIdx + 1; i < events.size(); ++i) {
                            if (events[i].message.isNoteOff() && events[i].message.getNoteNumber() == oldNote) {
                                // Calculate duration and apply to new position
                                int64_t duration = events[i].timestamp - oldTimestamp;
                                int64_t newOffTimestamp = newTimestamp + duration;
                                events[i].timestamp = std::max(static_cast<int64_t>(0), newOffTimestamp);
                                events[i].message = MidiMessage(MidiMessageType::NoteOff, 1, newNoteNum, 0);
                                break;
                            }
                        }
                    }
                }
            }
            markDirty();
            
            // Now perform overlap detection and cutting
            // After dragging, check if dragged notes overlap with other notes and split them
            // Rebuild noteRects to get updated positions after dragging
            std::vector<NoteRect> updatedNoteRects;
            for (size_t clipIdx = 0; clipIdx < track.clips.size(); ++clipIdx) {
                auto& clip = track.clips[clipIdx];
                const auto& events = clip->getEvents();
                float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
                
                std::map<uint8_t, std::pair<float, size_t>> activeNotes;  // noteNum -> (startBeat, eventIndex)
                
                for (size_t evtIdx = 0; evtIdx < events.size(); ++evtIdx) {
                    const auto& event = events[evtIdx];
                    float beatPos = static_cast<float>(event.timestamp) / samplesPerBeat + clipStartBeat;
                    
                    if (event.message.isNoteOn()) {
                        uint8_t noteNum = event.message.getNoteNumber();
                        activeNotes[noteNum] = {beatPos, evtIdx};
                    } else if (event.message.isNoteOff()) {
                        uint8_t noteNum = event.message.getNoteNumber();
                        if (activeNotes.find(noteNum) != activeNotes.end()) {
                            float startBeat = activeNotes[noteNum].first;
                            size_t startEventIdx = activeNotes[noteNum].second;
                            float endBeat = beatPos;
                            
                            uint8_t velocity = events[startEventIdx].message.getVelocity();
                            updatedNoteRects.push_back({startBeat, endBeat, noteNum, velocity, clipIdx, startEventIdx});
                            activeNotes.erase(noteNum);
                        }
                    }
                }
            }
            
            // Collect all cutting operations to perform (to avoid index issues when modifying events)
            struct CutOperation {
                size_t clipIndex;
                size_t noteOnEventIndex;
                float cutStartBeat;  // Start of the cut region (overlapping region)
                float cutEndBeat;    // End of the cut region (overlapping region)
            };
            std::vector<CutOperation> cutsToPerform;
            
            // For each dragged note, check for overlaps with non-selected notes
            for (const auto& draggedRect : updatedNoteRects) {
                // Check if this note is in the selection
                bool isDragged = (selectedNotes_.find({draggedRect.clipIndex, draggedRect.eventIndex}) != selectedNotes_.end());
                
                if (isDragged) {
                    // Check all other notes for overlap
                    for (const auto& otherRect : updatedNoteRects) {
                        // Skip if it's the same note or also selected
                        if (draggedRect.clipIndex == otherRect.clipIndex && 
                            draggedRect.eventIndex == otherRect.eventIndex) {
                            continue;
                        }
                        
                        bool isOtherSelected = (selectedNotes_.find({otherRect.clipIndex, otherRect.eventIndex}) != selectedNotes_.end());
                        if (isOtherSelected) {
                            continue;
                        }
                        
                        // Check if they're on the same note number and overlap
                        if (otherRect.noteNum == draggedRect.noteNum) {
                            bool overlaps = (otherRect.startBeat < draggedRect.endBeat && otherRect.endBeat > draggedRect.startBeat);
                            
                            if (overlaps) {
                                // Queue a cut operation - delete the overlapping region from the underlying note
                                float cutStart = std::max(otherRect.startBeat, draggedRect.startBeat);
                                float cutEnd = std::min(otherRect.endBeat, draggedRect.endBeat);
                                cutsToPerform.push_back({otherRect.clipIndex, otherRect.eventIndex, cutStart, cutEnd});
                                std::cout << "Queued cut: underlying note " << otherRect.startBeat << "-" << otherRect.endBeat 
                                          << " will have region " << cutStart << "-" << cutEnd 
                                          << " removed (dragged note: " << draggedRect.startBeat << "-" << draggedRect.endBeat << ")" << std::endl;
                            }
                        }
                    }
                }
            }
            
            // Apply all cuts (process in reverse order to maintain indices)
            std::sort(cutsToPerform.begin(), cutsToPerform.end(), 
                     [](const CutOperation& a, const CutOperation& b) {
                         if (a.clipIndex != b.clipIndex) return a.clipIndex > b.clipIndex;
                         return a.noteOnEventIndex > b.noteOnEventIndex;
                     });
            
            for (const auto& cut : cutsToPerform) {
                if (cut.clipIndex < track.clips.size()) {
                    auto& clip = track.clips[cut.clipIndex];
                    auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
                    float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
                    
                    if (cut.noteOnEventIndex < events.size() && events[cut.noteOnEventIndex].message.isNoteOn()) {
                        uint8_t noteNum = events[cut.noteOnEventIndex].message.getNoteNumber();
                        uint8_t velocity = events[cut.noteOnEventIndex].message.getVelocity();
                        
                        // Find the note-off event
                        size_t noteOffIdx = cut.noteOnEventIndex + 1;
                        for (; noteOffIdx < events.size(); ++noteOffIdx) {
                            if (events[noteOffIdx].message.isNoteOff() && 
                                events[noteOffIdx].message.getNoteNumber() == noteNum) {
                                break;
                            }
                        }
                        
                        if (noteOffIdx < events.size()) {
                            // Get the original note's bounds
                            float originalStartBeat = static_cast<float>(events[cut.noteOnEventIndex].timestamp) / samplesPerBeat + clipStartBeat;
                            float originalEndBeat = static_cast<float>(events[noteOffIdx].timestamp) / samplesPerBeat + clipStartBeat;
                            
                            // Determine what to do based on overlap type
                            if (cut.cutStartBeat <= originalStartBeat + 0.001f && cut.cutEndBeat >= originalEndBeat - 0.001f) {
                                // Case 1: Completely overlapped - delete the entire note
                                events.erase(events.begin() + noteOffIdx);
                                events.erase(events.begin() + cut.noteOnEventIndex);
                                std::cout << "Deleted note: " << originalStartBeat << "-" << originalEndBeat << " (completely overlapped)" << std::endl;
                            } else if (cut.cutStartBeat <= originalStartBeat + 0.001f) {
                                // Case 2: Overlap at start - shorten from left (move start forward)
                                int64_t newStartTimestamp = static_cast<int64_t>((cut.cutEndBeat - clipStartBeat) * samplesPerBeat);
                                events[cut.noteOnEventIndex].timestamp = newStartTimestamp;
                                std::cout << "Shortened note from left: " << originalStartBeat << "-" << originalEndBeat 
                                          << " to " << cut.cutEndBeat << "-" << originalEndBeat << std::endl;
                            } else if (cut.cutEndBeat >= originalEndBeat - 0.001f) {
                                // Case 3: Overlap at end - shorten from right (move end backward)
                                int64_t newEndTimestamp = static_cast<int64_t>((cut.cutStartBeat - clipStartBeat) * samplesPerBeat);
                                events[noteOffIdx].timestamp = newEndTimestamp;
                                std::cout << "Shortened note from right: " << originalStartBeat << "-" << originalEndBeat 
                                          << " to " << originalStartBeat << "-" << cut.cutStartBeat << std::endl;
                            } else {
                                // Case 4: Overlap in middle - split into two notes (cut out the middle)
                                // First note: originalStart to cutStart
                                int64_t firstNoteOffTimestamp = static_cast<int64_t>((cut.cutStartBeat - clipStartBeat) * samplesPerBeat);
                                events[noteOffIdx].timestamp = firstNoteOffTimestamp;
                                
                                // Second note: cutEnd to originalEnd
                                int64_t secondNoteOnTimestamp = static_cast<int64_t>((cut.cutEndBeat - clipStartBeat) * samplesPerBeat);
                                int64_t secondNoteOffTimestamp = static_cast<int64_t>((originalEndBeat - clipStartBeat) * samplesPerBeat);
                                
                                MidiMessage secondNoteOn(MidiMessageType::NoteOn, 1, noteNum, velocity);
                                MidiMessage secondNoteOff(MidiMessageType::NoteOff, 1, noteNum, 0);
                                
                                // Insert after the note-off we just modified
                                events.insert(events.begin() + noteOffIdx + 1, {secondNoteOnTimestamp, secondNoteOn});
                                events.insert(events.begin() + noteOffIdx + 2, {secondNoteOffTimestamp, secondNoteOff});
                                
                                std::cout << "Cut middle from note: " << originalStartBeat << "-" << originalEndBeat 
                                          << " into " << originalStartBeat << "-" << cut.cutStartBeat 
                                          << " and " << cut.cutEndBeat << "-" << originalEndBeat << std::endl;
                            }
                            
                            markDirty();
                        } else {
                            std::cerr << "Warning: Could not find note-off for note at index " << cut.noteOnEventIndex << std::endl;
                        }
                    }
                }
            }
            
            draggedNote_.isDragging = false;
        }
    }
    
    // Handle note resizing
    if (resizingNote_.isResizing) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        
        if (ImGui::IsMouseDragging(0) || ImGui::IsMouseDown(0)) {
            if (mouseOverCanvas) {
                float currentBeat = hoveredBeat;
                if (gridSnapEnabled_) {
                    currentBeat = snapToGrid(currentBeat);
                }
                
                // Update the note length
                if (resizingNote_.clipIndex < track.clips.size()) {
                    auto& clip = track.clips[resizingNote_.clipIndex];
                    auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
                    
                    if (resizingNote_.eventIndex < events.size() && events[resizingNote_.eventIndex].message.isNoteOn()) {
                        auto& noteOnEvent = events[resizingNote_.eventIndex];
                        float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
                        
                        float newStartBeat = resizingNote_.originalStartBeat;
                        float newEndBeat = resizingNote_.originalEndBeat;
                        
                        if (resizingNote_.isLeftEdge) {
                            // Resize from left edge (move start time)
                            newStartBeat = std::min(currentBeat, resizingNote_.originalEndBeat - snapToGrid(0.25f));
                            
                            int64_t newTimestamp = static_cast<int64_t>((newStartBeat - clipStartBeat) * samplesPerBeat);
                            noteOnEvent.timestamp = std::max(static_cast<int64_t>(0), newTimestamp);
                        } else {
                            // Resize from right edge (move end time)
                            newEndBeat = std::max(currentBeat, resizingNote_.originalStartBeat + snapToGrid(0.25f));
                            
                            // Find and update note-off
                            uint8_t noteNum = noteOnEvent.message.getNoteNumber();
                            for (size_t i = resizingNote_.eventIndex + 1; i < events.size(); ++i) {
                                if (events[i].message.isNoteOff() && events[i].message.getNoteNumber() == noteNum) {
                                    int64_t newOffTimestamp = static_cast<int64_t>((newEndBeat - clipStartBeat) * samplesPerBeat);
                                    events[i].timestamp = std::max(static_cast<int64_t>(0), newOffTimestamp);
                                    break;
                                }
                            }
                        }
                        
                        markDirty();
                    }
                }
            }
        } else if (ImGui::IsMouseReleased(0)) {
            resizingNote_.isResizing = false;
        }
    }
    
    // Right-click context menu
    if (mouseOverCanvas && ImGui::IsMouseClicked(1)) {
        showContextMenu_ = true;
        ImGui::OpenPopup("PianoRollContextMenu");
    }
    
    if (ImGui::BeginPopup("PianoRollContextMenu")) {
        ImGui::Text("Grid Division");
        ImGui::Separator();
        
        GridDivision divisions[] = {
            GridDivision::Whole, GridDivision::Half, GridDivision::Quarter, 
            GridDivision::Eighth, GridDivision::Sixteenth, GridDivision::ThirtySecond,
            GridDivision::QuarterTriplet, GridDivision::EighthTriplet, GridDivision::SixteenthTriplet
        };
        
        for (const auto& div : divisions) {
            if (ImGui::MenuItem(getGridDivisionName(div), nullptr, currentGridDivision_ == div)) {
                currentGridDivision_ = div;
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::MenuItem("Quantize Notes", "Q")) {
            quantizeSelectedTrack();
        }
        
        ImGui::EndPopup();
    } else {
        showContextMenu_ = false;
    }
    
    // Draw tool - click and drag to create notes
    if (pencilToolActive_ && mouseOverCanvas && !draggedNote_.isDragging && !boxSelection_.isSelecting) {
        if (ImGui::IsMouseClicked(0)) {
            // Start drawing a new note
            float newBeat = hoveredBeat;
            if (gridSnapEnabled_) {
                newBeat = snapToGrid(newBeat);
            }
            
            // Get or create a clip to add the note to
            if (track.clips.empty()) {
                auto newClip = std::make_shared<MidiClip>("Piano Roll");
                newClip->setStartTime(0);
                track.clips.push_back(newClip);
            }
            
            drawingNote_.isDrawing = true;
            drawingNote_.startBeat = newBeat;
            drawingNote_.noteNum = pianoRollHoverNote_;
            drawingNote_.clipIndex = 0;
        }
    }
    
    // Handle note drawing
    if (drawingNote_.isDrawing) {
        if (ImGui::IsMouseDragging(0) || ImGui::IsMouseDown(0)) {
            // Continue drawing - visualize the note being drawn
            float currentBeat = hoveredBeat;
            if (gridSnapEnabled_) {
                currentBeat = snapToGrid(currentBeat);
            }
            
            // Draw preview of note being created
            float noteDuration = std::max(snapToGrid(0.25f), currentBeat - drawingNote_.startBeat);
            
            if (drawingNote_.noteNum >= lowestNote && drawingNote_.noteNum < lowestNote + totalNotes) {
                int noteIndex = totalNotes - 1 - (drawingNote_.noteNum - lowestNote);
                float noteY = canvasPos.y + noteIndex * noteHeight;
                float noteX = gridStart + drawingNote_.startBeat * pixelsPerBeat;
                float noteW = noteDuration * pixelsPerBeat;
                
                // Draw semi-transparent preview
                drawList->AddRectFilled(ImVec2(noteX, noteY + 1), 
                                       ImVec2(noteX + noteW, noteY + noteHeight - 1),
                                       IM_COL32(150, 255, 150, 150));
                drawList->AddRect(ImVec2(noteX, noteY + 1), 
                                 ImVec2(noteX + noteW, noteY + noteHeight - 1),
                                 IM_COL32(200, 255, 200, 255), 0.0f, 0, 2.0f);
            }
        }
        
        if (ImGui::IsMouseReleased(0)) {
            // Finish drawing the note
            float endBeat = hoveredBeat;
            if (gridSnapEnabled_) {
                endBeat = snapToGrid(endBeat);
            }
            
            // Ensure minimum note duration
            float noteDuration = std::max(snapToGrid(0.25f), endBeat - drawingNote_.startBeat);
            
            // Safely access the track and clip
            if (selectedTrackIndex_ < tracks_.size()) {
                auto& currentTrack = tracks_[selectedTrackIndex_];
                if (drawingNote_.clipIndex < currentTrack.clips.size()) {
                    auto& clip = currentTrack.clips[drawingNote_.clipIndex];
                    
                    // Convert beats to samples
                    float clipStartBeat = static_cast<float>(clip->getStartTime()) / samplesPerBeat;
                    int64_t noteOnTimestamp = static_cast<int64_t>((drawingNote_.startBeat - clipStartBeat) * samplesPerBeat);
                    int64_t noteOffTimestamp = static_cast<int64_t>((drawingNote_.startBeat + noteDuration - clipStartBeat) * samplesPerBeat);
                    
                    // Add note-on and note-off events
                    MidiMessage noteOnMsg(MidiMessageType::NoteOn, 1, drawingNote_.noteNum, 80);
                    MidiMessage noteOffMsg(MidiMessageType::NoteOff, 1, drawingNote_.noteNum, 0);
                    
                    clip->addEvent(noteOnTimestamp, noteOnMsg);
                    clip->addEvent(noteOffTimestamp, noteOffMsg);
                    
                    markDirty();
                    
                    std::cout << "Added note: " << static_cast<int>(drawingNote_.noteNum) 
                             << " at beat " << drawingNote_.startBeat 
                             << " duration " << noteDuration << " beats" << std::endl;
                }
            }
            
            drawingNote_.isDrawing = false;
        }
    }
    
    // Set draw tool cursor when active
    if (pencilToolActive_ && mouseOverCanvas && !drawingNote_.isDrawing) {
        // Hide the default cursor and draw custom icon
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        
        // Draw the draw icon at the mouse cursor if available
        // Position so the bottom-leftmost pixel (tip) aligns exactly with the cursor
        if (drawIconTexture_) {
            // The tip is at (drawIconTipOffsetX_, drawIconTipOffsetY_) from top-left
            // To align tip with cursor: iconPos + tipOffset = mousePos
            ImVec2 iconPos(mousePos.x - drawIconTipOffsetX_, mousePos.y - drawIconTipOffsetY_);
            drawList->AddImage(drawIconTexture_, 
                              iconPos, 
                              ImVec2(iconPos.x + drawIconWidth_, iconPos.y + drawIconHeight_));
        }
    }
    
    // Handle 'Q' key for quantize
    if (pianoRollActive_ && ImGui::IsKeyPressed(ImGuiKey_Q) && !ImGui::GetIO().WantTextInput) {
        quantizeSelectedTrack();
    }
    
    // Handle Ctrl+A for select all
    if (pianoRollActive_ && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        selectedNotes_.clear();
        for (const auto& note : noteRects) {
            selectedNotes_.insert({note.clipIndex, note.eventIndex});
        }
        std::cout << "Selected all " << selectedNotes_.size() << " notes" << std::endl;
    }
    
    // Handle Delete key to delete selected notes
    if (pianoRollActive_ && ImGui::IsKeyPressed(ImGuiKey_Delete) && !selectedNotes_.empty()) {
        // Collect all events to delete (we need to sort by clip and reverse order)
        std::map<size_t, std::vector<size_t>> notesToDelete;  // clipIndex -> list of eventIndices
        
        for (const auto& [clipIdx, eventIdx] : selectedNotes_) {
            notesToDelete[clipIdx].push_back(eventIdx);
        }
        
        // Delete events from each clip (in reverse order to maintain indices)
        for (auto& [clipIdx, eventIndices] : notesToDelete) {
            if (clipIdx < track.clips.size()) {
                auto& clip = track.clips[clipIdx];
                auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
                
                // Sort indices in descending order
                std::sort(eventIndices.begin(), eventIndices.end(), std::greater<size_t>());
                
                // For each note-on event, also find and remove the corresponding note-off
                std::set<size_t> toRemove;
                for (size_t noteOnIdx : eventIndices) {
                    if (noteOnIdx < events.size() && events[noteOnIdx].message.isNoteOn()) {
                        toRemove.insert(noteOnIdx);
                        
                        // Find corresponding note-off
                        uint8_t noteNum = events[noteOnIdx].message.getNoteNumber();
                        for (size_t i = noteOnIdx + 1; i < events.size(); ++i) {
                            if (events[i].message.isNoteOff() && 
                                events[i].message.getNoteNumber() == noteNum) {
                                toRemove.insert(i);
                                break;
                            }
                        }
                    }
                }
                
                // Remove events in reverse order
                std::vector<size_t> sortedToRemove(toRemove.begin(), toRemove.end());
                std::sort(sortedToRemove.begin(), sortedToRemove.end(), std::greater<size_t>());
                
                for (size_t idx : sortedToRemove) {
                    if (idx < events.size()) {
                        events.erase(events.begin() + idx);
                    }
                }
            }
        }
        
        std::cout << "Deleted " << selectedNotes_.size() << " notes" << std::endl;
        selectedNotes_.clear();
        markDirty();
    }
    
    // Make canvas interactive
    ImGui::SetCursorScreenPos(canvasPos);
    ImGui::InvisibleButton("piano_roll_canvas", canvasSize);
    
    ImGui::End();
#endif
}

void* MainWindow::loadSVGToTexture(const char* filepath, int& width, int& height, int targetSize, int& tipOffsetX, int& tipOffsetY) {
#ifdef PAN_USE_GUI
    // Parse SVG file
    NSVGimage* image = nsvgParseFromFile(filepath, "px", 96.0f);
    if (!image) {
        std::cerr << "Failed to load SVG: " << filepath << std::endl;
        return nullptr;
    }
    
    // Calculate scale to fit targetSize
    float scale = targetSize / std::max(image->width, image->height);
    width = (int)(image->width * scale);
    height = (int)(image->height * scale);
    
    // Create rasterizer
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    if (!rast) {
        std::cerr << "Failed to create SVG rasterizer" << std::endl;
        nsvgDelete(image);
        return nullptr;
    }
    
    // Allocate buffer for RGBA image
    unsigned char* img = (unsigned char*)malloc(width * height * 4);
    if (!img) {
        std::cerr << "Failed to allocate image buffer" << std::endl;
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return nullptr;
    }
    
    // Rasterize SVG
    nsvgRasterize(rast, image, 0, 0, scale, img, width, height, width * 4);
    
    // Find bottom-leftmost non-alpha pixel
    // Scan from bottom to top, and for each row, find the leftmost pixel
    tipOffsetX = width;  // Start with rightmost position (will be updated)
    tipOffsetY = -1;     // Start with invalid position
    
    // Scan from bottom to top
    for (int y = height - 1; y >= 0; --y) {
        // Find leftmost pixel in this row
        for (int x = 0; x < width; ++x) {
            unsigned char* pixel = img + (y * width + x) * 4;
            unsigned char a = pixel[3];
            
            if (a > 0) {  // Non-transparent pixel found
                // This is the leftmost pixel in this row
                // If we haven't found a pixel yet, or this row is lower (higher y), use it
                if (tipOffsetY == -1 || y > tipOffsetY || (y == tipOffsetY && x < tipOffsetX)) {
                    tipOffsetX = x;
                    tipOffsetY = y;
                }
                break;  // Found leftmost pixel in this row, move to next row up
            }
        }
    }
    
    // If no pixel found, default to bottom-left corner
    if (tipOffsetY == -1) {
        tipOffsetX = 0;
        tipOffsetY = height - 1;
    }
    
    // Apply ivory tint to the image (replace dark colors with ivory)
    // Ivory color: RGB(250, 250, 240)
    for (int i = 0; i < width * height; ++i) {
        unsigned char* pixel = img + (i * 4);
        unsigned char r = pixel[0];
        unsigned char g = pixel[1];
        unsigned char b = pixel[2];
        unsigned char a = pixel[3];
        
        // If pixel is not transparent and is dark (likely part of the icon)
        if (a > 0 && (r < 128 || g < 128 || b < 128)) {
            // Apply ivory tint while preserving alpha
            pixel[0] = 250;  // R
            pixel[1] = 250;  // G
            pixel[2] = 240;  // B
            // Keep alpha as is
        }
    }
    
    // Create OpenGL texture
    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    
    // Setup filtering parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Upload pixels to texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    
    // Cleanup
    free(img);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    
    return (void*)(intptr_t)textureID;
#else
    return nullptr;
#endif
}

void MainWindow::loadSVGIcons() {
#ifdef PAN_USE_GUI
    // Load folder icon (no tip offset needed)
    int dummyX, dummyY;
    folderIconTexture_ = loadSVGToTexture("svg/folder.svg", folderIconWidth_, folderIconHeight_, 16, dummyX, dummyY);
    if (folderIconTexture_) {
        std::cout << "Successfully loaded folder icon: " << folderIconWidth_ << "x" << folderIconHeight_ << std::endl;
    } else {
        std::cerr << "Warning: Failed to load folder icon" << std::endl;
    }
    
    // Load draw/pencil icon (find bottom-leftmost pixel)
    drawIconTexture_ = loadSVGToTexture("svg/draw.svg", drawIconWidth_, drawIconHeight_, 16, drawIconTipOffsetX_, drawIconTipOffsetY_);
    if (drawIconTexture_) {
        std::cout << "Successfully loaded draw icon: " << drawIconWidth_ << "x" << drawIconHeight_ 
                  << ", tip at (" << drawIconTipOffsetX_ << ", " << drawIconTipOffsetY_ << ")" << std::endl;
    } else {
        std::cerr << "Warning: Failed to load draw icon" << std::endl;
    }
#endif
}

} // namespace pan




