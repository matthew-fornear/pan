#include "pan/gui/main_window.h"
#include "pan/audio/reverb.h"
#include "pan/audio/chorus.h"
#include "pan/audio/distortion.h"
#include "pan/audio/eq8.h"
#include "pan/audio/sidechain_pump.h"
#include "pan/audio/wow_flutter.h"
#include "pan/audio/beat_repeat.h"
#include "pan/audio/bit_noise_texture.h"
#include "pan/audio/resonator_bank.h"
#include "pan/audio/sampler.h"
#include <iostream>
#include <filesystem>
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

// Flag to auto-switch to Effects tab when an effect is added
static bool g_switchToEffectsTab = false;

Track::Track() 
    : isRecording(false)
    , isSolo(false)
    , isMuted(false)
    , volumeDb(0.0f)
    , pan(0.0f)
    , name("")
    , waveformSet(false)
    , colorIndex(0)
    , peakLevel(0.0f)
    , peakHold(0.0f)
    , peakHoldTime(0.0)
    , waveformBuffer(WAVEFORM_BUFFER_SIZE, 0.0f)
    , waveformBufferWritePos(0)
    , waveformBufferMutex(std::make_unique<std::mutex>())
{
    // Track starts empty - drag instruments/samples from browser to load
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

// Global pointer for GLFW callbacks
static MainWindow* g_mainWindowInstance = nullptr;

// GLFW drop callback for OS file drag-and-drop
static void glfw_drop_callback(GLFWwindow* window, int count, const char** paths) {
    std::cout << "[Drop] GLFW drop callback triggered with " << count << " files" << std::endl;
    if (g_mainWindowInstance && count > 0) {
        std::lock_guard<std::mutex> lock(g_mainWindowInstance->droppedFilesMutex_);
        for (int i = 0; i < count; i++) {
            std::string path(paths[i]);
            std::cout << "[Drop] File " << i << ": " << path << std::endl;
            std::string ext = std::filesystem::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            // Accept WAV and MP3 files
            if (ext == ".wav" || ext == ".mp3") {
                g_mainWindowInstance->droppedFiles_.push_back(path);
                std::cout << "[Drop] Accepted: " << path << std::endl;
            } else {
                std::cout << "[Drop] Rejected (only .wav/.mp3 supported): " << path << std::endl;
            }
        }
    } else {
        std::cout << "[Drop] No main window instance!" << std::endl;
    }
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
    , effectsScrollY_(0.0f)
    , masterPeakL_(0.0f)
    , masterPeakR_(0.0f)
    , masterPeakHoldL_(0.0f)
    , masterPeakHoldR_(0.0f)
    , masterPeakHoldTime_(0.0)
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
    GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, "PANDAW", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return false;
    }
    
    window_ = window;
    
    // Set up OS file drop callback
    g_mainWindowInstance = this;
    glfwSetDropCallback(window, glfw_drop_callback);
    
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
    
    // Load custom fonts for better typography
    
    // Try to load a modern sans-serif font (common on Linux systems)
    const char* fontPaths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        nullptr
    };
    
    bool fontLoaded = false;
    for (int i = 0; fontPaths[i] != nullptr; ++i) {
        struct stat buffer;
        if (stat(fontPaths[i], &buffer) == 0) {
            // Font file exists - load it at 15px for regular text
            io.Fonts->AddFontFromFileTTF(fontPaths[i], 15.0f);
            std::cout << "Loaded font: " << fontPaths[i] << std::endl;
            fontLoaded = true;
            break;
        }
    }
    
    if (!fontLoaded) {
        std::cout << "Using default ImGui font (no system fonts found)" << std::endl;
    }
    
    // Setup Dear ImGui style - Ableton Live inspired dark theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Ableton-inspired color palette
    ImVec4 bgDarkest(0.071f, 0.071f, 0.071f, 1.0f);    // #121212 - Darkest background
    ImVec4 bgDark(0.098f, 0.098f, 0.098f, 1.0f);       // #191919 - Dark background
    ImVec4 bgMid(0.137f, 0.137f, 0.137f, 1.0f);        // #232323 - Mid background
    ImVec4 bgLight(0.180f, 0.180f, 0.180f, 1.0f);      // #2E2E2E - Light background
    ImVec4 borderColor(0.220f, 0.220f, 0.220f, 1.0f);  // #383838 - Borders
    ImVec4 textPrimary(0.878f, 0.878f, 0.878f, 1.0f);  // #E0E0E0 - Primary text
    ImVec4 textSecondary(0.600f, 0.600f, 0.600f, 1.0f);// #999999 - Secondary text
    ImVec4 accentOrange(1.0f, 0.584f, 0.0f, 1.0f);     // #FF9500 - Ableton orange (playhead/accent)O
    ImVec4 accentGreen(0.518f, 0.839f, 0.310f, 1.0f);  // #84D64F - Play/active green
    ImVec4 mutedYellow(0.690f, 0.569f, 0.180f, 1.0f);  // #B0912E - Solo yellow
    
    // Style adjustments for cleaner look
    style.WindowRounding = 0.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;
    style.TabRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 8.0f;
    
    // Window colors
    style.Colors[ImGuiCol_WindowBg] = bgDark;
    style.Colors[ImGuiCol_ChildBg] = bgDarkest;
    style.Colors[ImGuiCol_PopupBg] = bgMid;
    style.Colors[ImGuiCol_Border] = borderColor;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    
    // Text colors
    style.Colors[ImGuiCol_Text] = textPrimary;
    style.Colors[ImGuiCol_TextDisabled] = textSecondary;
    
    // Frame/button colors
    style.Colors[ImGuiCol_FrameBg] = bgDarkest;
    style.Colors[ImGuiCol_FrameBgHovered] = bgLight;
    style.Colors[ImGuiCol_FrameBgActive] = bgMid;
    
    // Button colors
    style.Colors[ImGuiCol_Button] = bgLight;
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = bgMid;
    
    // Header colors (collapsing headers, tree nodes)
    style.Colors[ImGuiCol_Header] = bgMid;
    style.Colors[ImGuiCol_HeaderHovered] = bgLight;
    style.Colors[ImGuiCol_HeaderActive] = accentOrange;
    
    // Title bar
    style.Colors[ImGuiCol_TitleBg] = bgDarkest;
    style.Colors[ImGuiCol_TitleBgActive] = bgDark;
    style.Colors[ImGuiCol_TitleBgCollapsed] = bgDarkest;
    
    // Scrollbar
    style.Colors[ImGuiCol_ScrollbarBg] = bgDarkest;
    style.Colors[ImGuiCol_ScrollbarGrab] = bgLight;
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.30f, 0.30f, 1.0f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    
    // Slider
    style.Colors[ImGuiCol_SliderGrab] = accentOrange;
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
    
    // Checkbox
    style.Colors[ImGuiCol_CheckMark] = accentOrange;
    
    // Separator
    style.Colors[ImGuiCol_Separator] = borderColor;
    style.Colors[ImGuiCol_SeparatorHovered] = accentOrange;
    style.Colors[ImGuiCol_SeparatorActive] = accentOrange;
    
    // Resize grip
    style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ResizeGripHovered] = accentOrange;
    style.Colors[ImGuiCol_ResizeGripActive] = accentOrange;
    
    // Tab
    style.Colors[ImGuiCol_Tab] = bgDark;
    style.Colors[ImGuiCol_TabHovered] = bgLight;
    style.Colors[ImGuiCol_TabActive] = bgMid;
    style.Colors[ImGuiCol_TabUnfocused] = bgDarkest;
    style.Colors[ImGuiCol_TabUnfocusedActive] = bgDark;
    
    // Docking
    style.Colors[ImGuiCol_DockingPreview] = ImVec4(accentOrange.x, accentOrange.y, accentOrange.z, 0.4f);
    style.Colors[ImGuiCol_DockingEmptyBg] = bgDarkest;
    
    // Table
    style.Colors[ImGuiCol_TableHeaderBg] = bgMid;
    style.Colors[ImGuiCol_TableBorderStrong] = borderColor;
    style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    // Load SVG icons after OpenGL context is created
    loadSVGIcons();
    
    // Initialize instrument presets
    initializeInstrumentPresets();
    
    // Load user-saved presets
    loadUserPresets();
    
    // Load samples from samples directory
    loadSamplesFromDirectory();
    
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
                                // Route to sampler if track has one, otherwise to synth
                                if (track.hasSampler && track.sampler) {
                                    if (event.message.getType() == MidiMessageType::NoteOn) {
                                        track.sampler->noteOn(event.message.getNoteNumber(), event.message.getVelocity());
                                    } else if (event.message.getType() == MidiMessageType::NoteOff) {
                                        track.sampler->noteOff(event.message.getNoteNumber());
                                    }
                                } else if (track.synth) {
                                    track.synth->processMidiMessage(event.message);
                                }
                            }
                        }
                    }
                }
            }
        }
        
        // Check if any track is soloed
        bool anySolo = false;
        for (const auto& track : tracks_) {
            if (track.isSolo) {
                anySolo = true;
                break;
            }
        }
        
        // Mix all recording tracks (armed tracks get live MIDI input)
        for (auto& track : tracks_) {
            if (track.synth || track.sampler) {
                // Skip muted tracks, or non-soloed tracks when solo is active
                bool shouldPlay = !track.isMuted && (!anySolo || track.isSolo);
                
                AudioBuffer trackBuffer(output.getNumChannels(), numFrames);
                
                // Use sampler for audio if track has sampler, otherwise use synth
                if (track.hasSampler && track.sampler) {
                    float* leftOut = trackBuffer.getWritePointer(0);
                    float* rightOut = trackBuffer.getNumChannels() > 1 ? trackBuffer.getWritePointer(1) : leftOut;
                    track.sampler->process(leftOut, rightOut, numFrames);
                } else if (track.synth) {
                track.synth->generateAudio(trackBuffer, numFrames);
                }
                
                // Apply effects chain
                for (auto& effect : track.effects) {
                    if (effect && effect->isEnabled()) {
                        effect->process(trackBuffer, numFrames);
                    }
                }
                
                // Capture waveform samples for visualization (use first channel, after effects)
                // Also calculate peak level for metering
                if (trackBuffer.getNumChannels() > 0) {
                    const float* trackSamples = trackBuffer.getReadPointer(0);
                    float maxSample = 0.0f;
                    for (size_t i = 0; i < numFrames; ++i) {
                        track.addWaveformSample(trackSamples[i]);
                        float absSample = std::abs(trackSamples[i]);
                        if (absSample > maxSample) maxSample = absSample;
                    }
                    // Smooth peak level (fast attack, slow decay)
                    if (maxSample > track.peakLevel) {
                        track.peakLevel = maxSample;
                    } else {
                        track.peakLevel = track.peakLevel * 0.95f;  // Decay
                    }
                }
                
                // Mix into output (only if track should play)
                if (shouldPlay) {
                    float gain = std::pow(10.0f, track.volumeDb / 20.0f);
                    float pan = std::clamp(track.pan, -1.0f, 1.0f);
                    float angle = (pan + 1.0f) * 0.25f * static_cast<float>(M_PI); // 0..pi/2
                    float lGain = gain * std::cos(angle);
                    float rGain = gain * std::sin(angle);
                    
                    const float* trackL = trackBuffer.getReadPointer(0);
                    const float* trackR = trackBuffer.getNumChannels() > 1 ? trackBuffer.getReadPointer(1) : trackL;
                    
                    float* outL = output.getWritePointer(0);
                    float* outR = output.getNumChannels() > 1 ? output.getWritePointer(1) : outL;
                    
                    for (size_t i = 0; i < numFrames; ++i) {
                        outL[i] += trackL[i] * lGain;
                        if (output.getNumChannels() > 1) {
                            outR[i] += trackR[i] * rGain;
                        }
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
        
        // Calculate master output levels for metering
        float maxL = 0.0f, maxR = 0.0f;
        if (output.getNumChannels() >= 2) {
            const float* leftSamples = output.getReadPointer(0);
            const float* rightSamples = output.getReadPointer(1);
            for (size_t i = 0; i < numFrames; ++i) {
                float absL = std::abs(leftSamples[i]);
                float absR = std::abs(rightSamples[i]);
                if (absL > maxL) maxL = absL;
                if (absR > maxR) maxR = absR;
            }
        } else if (output.getNumChannels() >= 1) {
            const float* samples = output.getReadPointer(0);
            for (size_t i = 0; i < numFrames; ++i) {
                float absSample = std::abs(samples[i]);
                if (absSample > maxL) maxL = absSample;
            }
            maxR = maxL;
        }
        
        // Smooth peak levels (fast attack, slow decay)
        if (maxL > masterPeakL_) masterPeakL_ = maxL;
        else masterPeakL_ = masterPeakL_ * 0.95f;
        
        if (maxR > masterPeakR_) masterPeakR_ = maxR;
        else masterPeakR_ = masterPeakR_ * 0.95f;
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
                    if (track.isRecording) {
                        // Route to sampler if track has one, otherwise to synth
                        if (track.hasSampler && track.sampler) {
                            if (msg.getType() == MidiMessageType::NoteOn && msg.getVelocity() > 0) {
                                track.sampler->noteOn(msg.getNoteNumber(), msg.getVelocity());
                            } else if (msg.getType() == MidiMessageType::NoteOff || 
                                      (msg.getType() == MidiMessageType::NoteOn && msg.getVelocity() == 0)) {
                                track.sampler->noteOff(msg.getNoteNumber());
                            }
                        } else if (track.synth) {
                        track.synth->processMidiMessage(msg);
                        }
                        
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

void MainWindow::processDroppedFiles() {
#ifdef PAN_USE_GUI
    std::vector<std::string> filesToProcess;
    {
        std::lock_guard<std::mutex> lock(droppedFilesMutex_);
        filesToProcess = std::move(droppedFiles_);
        droppedFiles_.clear();
    }
    
    if (!filesToProcess.empty()) {
        std::cout << "[ProcessDrop] Processing " << filesToProcess.size() << " files" << std::endl;
    }
    
    for (const auto& filePath : filesToProcess) {
        std::cout << "[ProcessDrop] Processing: " << filePath << std::endl;
        // Check if it's a supported audio file
        std::string ext = std::filesystem::path(filePath).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == ".wav" || ext == ".mp3") {
            // Import the sample
            if (importSample(filePath)) {
                // If we have a selected track with sampler, load into it
                if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                    auto& track = tracks_[selectedTrackIndex_];
                    
                    // Find the imported sample
                    std::string filename = std::filesystem::path(filePath).filename().string();
                    for (size_t i = 0; i < userSamples_.size(); i++) {
                        if (std::filesystem::path(userSamples_[i].path).filename().string() == filename) {
                            // If track has sampler, load sample into it
                            if (track.hasSampler) {
                                track.samplerSamplePath = userSamples_[i].path;
                                track.samplerWaveform = userSamples_[i].waveformDisplay;
                                track.instrumentName = "Sampler: " + userSamples_[i].name;
                            } else {
                                // Create a sampler track with this sample
                                track.hasSampler = true;
                                track.samplerSamplePath = userSamples_[i].path;
                                track.samplerWaveform = userSamples_[i].waveformDisplay;
                                track.oscillators.clear();
                                track.instrumentName = "Sampler: " + userSamples_[i].name;
                            }
                            // Create sampler instance and load sample
                            if (!track.sampler) {
                                double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                                track.sampler = std::make_shared<Sampler>(sampleRate);
                            }
                            track.sampler->loadSample(userSamples_[i].path);
                            markDirty();
                            break;
                        }
                    }
                }
            }
        }
    }
#endif
}

void MainWindow::renderUI() {
#ifdef PAN_USE_GUI
    // Update timeline if playing
    updateTimeline();
    
    // Process any files dropped from OS
    processDroppedFiles();
    
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
        
        // Split vertically: top ~50% for tracks, bottom ~50% for piano roll + components
        // Generous bottom section to prevent content falling off screen
        ImGuiID dock_id_top;
        ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Down, 0.50f, &dock_id_bottom_static, &dock_id_top);
        
        // Split top into left (Sample Library) and right (tracks with timeline)
        // Use 0.1f (10%) for Sample Library to make it much more compact
        ImGui::DockBuilderSplitNode(dock_id_top, ImGuiDir_Left, 0.1f, &dock_id_left, &dock_id_right);
        
        // Split bottom into piano roll (top 65%) and components (bottom 35%)
        // Components get more room (35% of bottom = ~17.5% of screen)
        ImGui::DockBuilderSplitNode(dock_id_bottom_static, ImGuiDir_Down, 0.35f, &dock_id_components, &dock_id_piano_roll);
        
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
    
    // Reset cursor to arrow at start of frame - individual windows can override
    // This ensures cursor doesn't get "stuck" in None state when moving between windows
    if (pencilToolActive_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
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
    
    // Handle Spacebar for play/pause
    if (ImGui::IsKeyPressed(ImGuiKey_Space) && !ImGui::GetIO().WantTextInput) {
        isPlaying_ = !isPlaying_;
        isCountingIn_ = false;  // Cancel count-in if active
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
    
    // Ableton-style dark sidebar
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.098f, 0.098f, 0.098f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.18f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
    
    ImGui::Begin("Sample Library", nullptr, ImGuiWindowFlags_None);
    
    // Title - "BROWSER" centered
    float windowWidth = ImGui::GetWindowWidth();
    const char* browserTitle = "BROWSER";
    float titleWidth = ImGui::CalcTextSize(browserTitle).x;
    ImGui::SetCursorPosX((windowWidth - titleWidth) * 0.5f);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", browserTitle);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    
    // Basic Waves section
    if (ImGui::CollapsingHeader("Sounds", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
        Waveform waveforms[] = { 
            Waveform::Sine, 
            Waveform::Square, 
            Waveform::Sawtooth, 
            Waveform::Triangle 
        };
        
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        
        for (int i = 0; i < 4; ++i) {
            ImGui::PushID(i);
            
            if (ImGui::Button(waveNames[i], ImVec2(-1, 22))) {
                // Click to select
            }
            
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                ImGui::SetDragDropPayload("WAVEFORM", &waveforms[i], sizeof(Waveform));
                ImGui::Text("Dragging %s", waveNames[i]);
                ImGui::EndDragDropSource();
            }
            
            ImGui::PopID();
        }
        
        ImGui::PopStyleColor(2);
    }
    
    // Instruments section with sub-categories
    if (ImGui::CollapsingHeader("Instruments", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Group instruments by category
        std::map<std::string, std::vector<size_t>> categoryMap;
        for (size_t i = 0; i < instrumentPresets_.size(); ++i) {
            categoryMap[instrumentPresets_[i].category].push_back(i);
        }
        
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
        
        // Render each category
        for (const auto& [category, indices] : categoryMap) {
            if (ImGui::TreeNode(category.c_str())) {
                for (size_t idx : indices) {
                    const auto& preset = instrumentPresets_[idx];
                    ImGui::PushID(static_cast<int>(idx + 1000));
                    
                    if (ImGui::Button(preset.name.c_str(), ImVec2(-1, 22))) {
                        // Click to select
                    }
                    
                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                        ImGui::SetDragDropPayload("INSTRUMENT", &idx, sizeof(size_t));
                        ImGui::Text("Dragging %s", preset.name.c_str());
                        ImGui::EndDragDropSource();
                    }
                    
                    ImGui::PopID();
                }
                ImGui::TreePop();
            }
        }
        
        ImGui::PopStyleColor(2);
    }
    
    // Sampler section - drag Simpler to tracks to create sampler instrument
    if (ImGui::CollapsingHeader("Sampler", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.2f, 0.15f, 1.0f));
        
        // Draggable Sampler instrument - left aligned like other items
        ImGui::PushID("sampler_drag");
        // Use Selectable for consistent left-alignment with tree items
        if (ImGui::Selectable("Sampler", false, 0, ImVec2(0, 22))) {
            // Click to add to selected track
            if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                tracks_[selectedTrackIndex_].hasSampler = true;
                tracks_[selectedTrackIndex_].samplerSamplePath = "";
                tracks_[selectedTrackIndex_].samplerWaveform.clear();
                tracks_[selectedTrackIndex_].oscillators.clear();  // Clear default oscillators
                tracks_[selectedTrackIndex_].instrumentName = "Sampler";
                markDirty();
            }
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            int dummy = 0;
            ImGui::SetDragDropPayload("SIMPLER", &dummy, sizeof(int));
            ImGui::Text("Add Sampler");
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Samples:");
        
        if (userSamples_.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "  No samples loaded");
            ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.35f, 1.0f), "  Drop WAV/MP3 onto window");
        } else {
            for (size_t i = 0; i < userSamples_.size(); ++i) {
                const auto& sample = userSamples_[i];
                ImGui::PushID(static_cast<int>(i + 8000));
                
                // Draw waveform preview
                if (!sample.waveformDisplay.empty()) {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImVec2 pos = ImGui::GetCursorScreenPos();
                    float width = ImGui::GetContentRegionAvail().x;
                    float height = 24.0f;
                    
                    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                                           IM_COL32(25, 25, 25, 255), 2.0f);
                    
                    float centerY = pos.y + height / 2.0f;
                    float xStep = width / sample.waveformDisplay.size();
                    ImU32 waveColor = IM_COL32(255, 149, 0, 180);
                    
                    for (size_t j = 0; j < sample.waveformDisplay.size(); ++j) {
                        float x = pos.x + j * xStep;
                        float amp = sample.waveformDisplay[j] * (height / 2.0f - 2.0f);
                        drawList->AddLine(ImVec2(x, centerY - amp), ImVec2(x, centerY + amp), waveColor);
                    }
                    
                    // Truncate sample name to fit within the waveform box
                    std::string truncatedName = sample.name;
                    float maxNameWidth = width - 8.0f;
                    ImVec2 nameSize = ImGui::CalcTextSize(truncatedName.c_str());
                    if (nameSize.x > maxNameWidth && truncatedName.length() > 8) {
                        while (nameSize.x > maxNameWidth && truncatedName.length() > 8) {
                            truncatedName = truncatedName.substr(0, truncatedName.length() - 4) + "...";
                            nameSize = ImGui::CalcTextSize(truncatedName.c_str());
                        }
                    }
                    drawList->AddText(ImVec2(pos.x + 4, pos.y + 4), 
                                     IM_COL32(255, 255, 255, 200), truncatedName.c_str());
                    
                    // Use InvisibleButton for proper item ID (needed for context menu)
                    ImGui::InvisibleButton("##sampleWave", ImVec2(width, height));
                } else {
                    // Truncate button text too
                    std::string truncatedName = sample.name;
                    float maxNameWidth = ImGui::GetContentRegionAvail().x - 16.0f;
                    ImVec2 nameSize = ImGui::CalcTextSize(truncatedName.c_str());
                    if (nameSize.x > maxNameWidth && truncatedName.length() > 8) {
                        while (nameSize.x > maxNameWidth && truncatedName.length() > 8) {
                            truncatedName = truncatedName.substr(0, truncatedName.length() - 4) + "...";
                            nameSize = ImGui::CalcTextSize(truncatedName.c_str());
                        }
                    }
                    if (ImGui::Button(truncatedName.c_str(), ImVec2(-1, 22))) {
                        // Click action
                    }
                }
                
                // Right-click context menu for deletion (use explicit ID for safety)
                if (ImGui::BeginPopupContextItem("##sampleCtx")) {
                    if (ImGui::MenuItem("Delete Sample")) {
                        std::filesystem::remove(sample.path);
                        userSamples_.erase(userSamples_.begin() + i);
                        ImGui::EndPopup();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::EndPopup();
                }
                
                // Drag and drop source
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    ImGui::SetDragDropPayload("SAMPLE", &i, sizeof(size_t));
                    ImGui::Text("Load %s", sample.name.c_str());
                    ImGui::EndDragDropSource();
                }
                
                ImGui::PopID();
                ImGui::Spacing();
            }
        }
        
        ImGui::PopStyleColor(2);
    }
    
    // Effects section - with expandable preset lists like Instruments
    if (ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.25f, 0.35f, 1.0f));  // Blue tint
        
        // Reverb with presets
        if (ImGui::TreeNode("Reverb")) {
            const char* reverbPresets[] = { "Room", "Hall", "Chamber", "Plate", "Cathedral", "Ambient" };
            for (int i = 0; i < 6; ++i) {
                ImGui::PushID(10000 + i);
                if (ImGui::Button(reverbPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto newReverb = std::make_shared<Reverb>(sampleRate);
                        newReverb->loadPreset(static_cast<Reverb::Preset>(i));
                        tracks_[selectedTrackIndex_].effects.push_back(newReverb);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                // Drag drop source for preset
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = { 0, i }; // effectType=0 (Reverb), presetIdx=i
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Add Reverb - %s", reverbPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Chorus with presets
        if (ImGui::TreeNode("Chorus")) {
            const char* chorusPresets[] = { "Subtle", "Classic", "Deep", "Detune", "Vibrato" };
            for (int i = 0; i < 5; ++i) {
                ImGui::PushID(10100 + i);
                if (ImGui::Button(chorusPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto newChorus = std::make_shared<Chorus>(sampleRate);
                        newChorus->loadPreset(static_cast<Chorus::Preset>(i));
                        tracks_[selectedTrackIndex_].effects.push_back(newChorus);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = { 1, i }; // effectType=1 (Chorus), presetIdx=i
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Add Chorus - %s", chorusPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Distortion with presets
        if (ImGui::TreeNode("Distortion")) {
            const char* distortionPresets[] = { "Warm", "Crunch", "Heavy", "Fuzz", "Screamer" };
            for (int i = 0; i < 5; ++i) {
                ImGui::PushID(10200 + i);
                if (ImGui::Button(distortionPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto newDistortion = std::make_shared<Distortion>(sampleRate);
                        newDistortion->loadPreset(static_cast<Distortion::Preset>(i));
                        tracks_[selectedTrackIndex_].effects.push_back(newDistortion);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = { 2, i }; // effectType=2 (Distortion), presetIdx=i
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Add Distortion - %s", distortionPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Sidechain Pump presets
        if (ImGui::TreeNode("Sidechain Pump")) {
            const char* pumpPresets[] = { "Subtle", "Classic", "Hard", "Extreme" };
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(10300 + i);
                if (ImGui::Button(pumpPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sr = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto fx = std::make_shared<SidechainPump>(sr);
                        // Set preset params (depth in dB, attack/release in ms)
                        if (i == 0) { fx->setDepth(-6.0f); fx->setAttackMs(20.0f); fx->setReleaseMs(150.0f); }
                        else if (i == 1) { fx->setDepth(-12.0f); fx->setAttackMs(10.0f); fx->setReleaseMs(200.0f); }
                        else if (i == 2) { fx->setDepth(-24.0f); fx->setAttackMs(5.0f); fx->setReleaseMs(250.0f); }
                        else { fx->setDepth(-36.0f); fx->setAttackMs(2.0f); fx->setReleaseMs(300.0f); }
                        tracks_[selectedTrackIndex_].effects.push_back(fx);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = {4, i};
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Sidechain Pump - %s", pumpPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Wow/Flutter Tape presets
        if (ImGui::TreeNode("Wow/Flutter")) {
            const char* wowPresets[] = { "Subtle", "Vintage", "Warped", "Broken" };
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(10400 + i);
                if (ImGui::Button(wowPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sr = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto fx = std::make_shared<WowFlutter>(sr);
                        if (i == 0) { fx->setWowDepthMs(1.0f); fx->setFlutterDepthMs(0.2f); }
                        else if (i == 1) { fx->setWowDepthMs(2.5f); fx->setFlutterDepthMs(0.4f); }
                        else if (i == 2) { fx->setWowDepthMs(4.0f); fx->setFlutterDepthMs(0.8f); }
                        else { fx->setWowDepthMs(5.5f); fx->setFlutterDepthMs(1.2f); }
                        tracks_[selectedTrackIndex_].effects.push_back(fx);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = {5, i};
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Wow/Flutter - %s", wowPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Beat Repeat presets
        if (ImGui::TreeNode("Beat Repeat")) {
            const char* brPresets[] = { "1/8 Stutter", "1/16 Glitch", "Triplet", "Random" };
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(10500 + i);
                if (ImGui::Button(brPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sr = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto fx = std::make_shared<BeatRepeat>(sr);
                        if (i == 0) fx->setIntervalMs(250.0f);
                        else if (i == 1) fx->setIntervalMs(125.0f);
                        else if (i == 2) fx->setIntervalMs(167.0f);
                        else fx->setIntervalMs(100.0f);
                        tracks_[selectedTrackIndex_].effects.push_back(fx);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = {6, i};
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Beat Repeat - %s", brPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Bit/Noise Texture presets
        if (ImGui::TreeNode("Bit/Noise Texture")) {
            const char* bitPresets[] = { "Lo-Fi", "8-Bit", "Vinyl", "Crushed" };
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(10600 + i);
                if (ImGui::Button(bitPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sr = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto fx = std::make_shared<BitNoiseTexture>(sr);
                        if (i == 0) { fx->setBits(12); fx->setNoise(0.02f); }
                        else if (i == 1) { fx->setBits(8); fx->setNoise(0.0f); }
                        else if (i == 2) { fx->setBits(16); fx->setNoise(0.05f); }
                        else { fx->setBits(4); fx->setNoise(0.1f); }
                        tracks_[selectedTrackIndex_].effects.push_back(fx);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = {7, i};
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Bit/Noise - %s", bitPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // Resonator Bank presets
        if (ImGui::TreeNode("Resonator Bank")) {
            const char* resPresets[] = { "Metallic", "Warm", "Plucked", "Ambient" };
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(10700 + i);
                if (ImGui::Button(resPresets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sr = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto fx = std::make_shared<ResonatorBank>(sr);
                        if (i == 0) { fx->setDecay(0.3f); fx->setMix(0.5f); }
                        else if (i == 1) { fx->setDecay(0.6f); fx->setMix(0.4f); }
                        else if (i == 2) { fx->setDecay(0.2f); fx->setMix(0.6f); }
                        else { fx->setDecay(0.9f); fx->setMix(0.3f); }
                        tracks_[selectedTrackIndex_].effects.push_back(fx);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = {8, i};
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Resonator - %s", resPresets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        // EQ8 with presets
        if (ImGui::TreeNode("EQ8")) {
            const char* eq8Presets[] = { "Flat", "Bass Boost", "Presence", "Scooped", "Bright", "Warm", "Lo Cut" };
            for (int i = 0; i < 7; ++i) {
                ImGui::PushID(10300 + i);
                if (ImGui::Button(eq8Presets[i], ImVec2(-1, 22))) {
                    if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        auto newEQ8 = std::make_shared<EQ8>(sampleRate);
                        newEQ8->loadPreset(static_cast<EQ8::Preset>(i));
                        tracks_[selectedTrackIndex_].effects.push_back(newEQ8);
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    int payload[2] = { 3, i }; // effectType=3 (EQ8), presetIdx=i
                    ImGui::SetDragDropPayload("EFFECT_PRESET", payload, sizeof(payload));
                    ImGui::Text("Add EQ8 - %s", eq8Presets[i]);
                    ImGui::EndDragDropSource();
                }
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        
        ImGui::PopStyleColor(2);
    }
    
    // User Presets section
    if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (userPresets_.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "  No saved presets");
        } else {
            for (size_t i = 0; i < userPresets_.size(); ++i) {
                const auto& preset = userPresets_[i];
                ImGui::PushID(static_cast<int>(i + 5000));
                
                // Ableton-style selectable item
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button(preset.name.c_str(), ImVec2(-1, 0))) {
                    // Click to select
                }
                ImGui::PopStyleColor(2);
                
                // Right-click context menu for deletion
                if (ImGui::BeginPopupContextItem("##presetCtx")) {
                    if (ImGui::MenuItem("Delete")) {
                        userPresets_.erase(userPresets_.begin() + i);
                        saveUserPresetsToFile();
                        markDirty();
                        ImGui::EndPopup();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::EndPopup();
                }
                
                // Setup drag source
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    ImGui::SetDragDropPayload("USER_PRESET", &i, sizeof(size_t));
                    ImGui::Text("Dragging %s", preset.name.c_str());
                    ImGui::EndDragDropSource();
                }
                
                ImGui::PopID();
            }
        }
    }
    
    ImGui::End();
    ImGui::PopStyleColor(3);  // Pop library-specific colors
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
            
            // Show track name at top - Ableton style
            if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                // Track indicator with color (24 colors, starting with orange)
                static const ImU32 trackColors[24] = {
                    IM_COL32(255, 149, 0, 255), IM_COL32(255, 94, 94, 255),
                    IM_COL32(255, 94, 153, 255), IM_COL32(219, 103, 186, 255),
                    IM_COL32(179, 102, 219, 255), IM_COL32(138, 103, 219, 255),
                    IM_COL32(102, 128, 219, 255), IM_COL32(51, 153, 219, 255),
                    IM_COL32(51, 179, 204, 255), IM_COL32(0, 199, 140, 255),
                    IM_COL32(79, 199, 102, 255), IM_COL32(132, 214, 79, 255),
                    IM_COL32(255, 179, 102, 255), IM_COL32(255, 128, 128, 255),
                    IM_COL32(255, 153, 187, 255), IM_COL32(230, 153, 204, 255),
                    IM_COL32(204, 153, 230, 255), IM_COL32(179, 153, 230, 255),
                    IM_COL32(153, 170, 230, 255), IM_COL32(128, 191, 230, 255),
                    IM_COL32(128, 217, 217, 255), IM_COL32(102, 217, 191, 255),
                    IM_COL32(153, 230, 153, 255), IM_COL32(255, 230, 102, 255)
                };
                
                // Color indicator dot
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 dotPos = ImGui::GetCursorScreenPos();
                int colorIdx = tracks_[selectedTrackIndex_].colorIndex % 24;
                drawList->AddCircleFilled(ImVec2(dotPos.x + 6, dotPos.y + 8), 5.0f, trackColors[colorIdx]);
                ImGui::Dummy(ImVec2(16, 0));
                    ImGui::SameLine();
                
                // Track label with double-click rename support
                char trackLabel[64];
                if (!tracks_[selectedTrackIndex_].name.empty()) {
                    snprintf(trackLabel, sizeof(trackLabel), "%s", tracks_[selectedTrackIndex_].name.c_str());
                } else {
                    snprintf(trackLabel, sizeof(trackLabel), "Track %zu", selectedTrackIndex_ + 1);
            }
            
                // Check if renaming this track from components panel
                if (renamingTrackIndex_ == static_cast<int>(selectedTrackIndex_)) {
                    ImGui::PushItemWidth(150);
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
                    
                    if (ImGui::InputText("##comp_rename", trackRenameBuffer_, sizeof(trackRenameBuffer_), 
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                        tracks_[selectedTrackIndex_].name = trackRenameBuffer_;
                        renamingTrackIndex_ = -1;
                        markDirty();
                    }
                    
                    if (ImGui::IsItemVisible() && !ImGui::IsItemActive()) {
                        ImGui::SetKeyboardFocusHere(-1);
                    }
                    
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || 
                        (ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered())) {
                        renamingTrackIndex_ = -1;
                    }
                    
                    ImGui::PopStyleColor();
                    ImGui::PopItemWidth();
                } else {
                    // Clickable text with hover effect
                    ImVec2 textPos = ImGui::GetCursorScreenPos();
                    ImVec2 textSize = ImGui::CalcTextSize(trackLabel);
                    ImVec2 textMin = textPos;
                    ImVec2 textMax = ImVec2(textPos.x + textSize.x + 8.0f, textPos.y + textSize.y + 4.0f);
                    
                    ImGui::InvisibleButton("##track_label_btn", ImVec2(textSize.x + 8.0f, textSize.y + 4.0f));
                    bool labelHovered = ImGui::IsItemHovered();
                    
                    if (labelHovered) {
                        drawList->AddRectFilled(textMin, textMax, IM_COL32(60, 60, 60, 100), 2.0f);
                    }
                    
                    drawList->AddText(ImVec2(textPos.x + 4.0f, textPos.y + 2.0f), 
                                     IM_COL32(200, 200, 200, 255), trackLabel);
                    
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                        renamingTrackIndex_ = static_cast<int>(selectedTrackIndex_);
                        strncpy(trackRenameBuffer_, trackLabel, sizeof(trackRenameBuffer_) - 1);
                        trackRenameBuffer_[sizeof(trackRenameBuffer_) - 1] = '\0';
                    }
                }
                
                // "Save Preset" button
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
                if (ImGui::Button("Save Preset", ImVec2(90, 20))) {
                    ImGui::OpenPopup("SavePresetDialog");
                }
                ImGui::PopStyleColor();
                
                // Save preset dialog
                if (ImGui::BeginPopupModal("SavePresetDialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    static char presetName[64] = "";
                    ImGui::Text("Enter preset name:");
                    ImGui::InputText("##presetname", presetName, sizeof(presetName));
                    ImGui::Spacing();
                    
                    if (ImGui::Button("Save", ImVec2(120, 0))) {
                        if (strlen(presetName) > 0) {
                            saveUserPreset(presetName, tracks_[selectedTrackIndex_].oscillators);
                            presetName[0] = '\0';  // Clear input
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                        presetName[0] = '\0';  // Clear input
                        ImGui::CloseCurrentPopup();
                    }
                    
                    ImGui::EndPopup();
                }
                
                ImGui::Separator();
                ImGui::Spacing();
                
                // Render unified instrument/sampler panel
                auto& oscillators = tracks_[selectedTrackIndex_].oscillators;
                bool hasSampler = tracks_[selectedTrackIndex_].hasSampler;
                bool hasInstrument = !oscillators.empty() || hasSampler;
                
                if (hasInstrument) {
                    if (hasSampler) {
                        // Render unified Sampler panel with tabs for Sample and synth controls
                        renderSamplerPanel(selectedTrackIndex_);
                    } else {
                        // Render synth instrument panel
                        renderInstrumentPanel(selectedTrackIndex_);
                    }
                } else {
                    // Empty state - show drag hint
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::Text("Drag an instrument from the browser to load");
                    ImGui::PopStyleColor();
                }
            } else {
                ImGui::Text("No tracks available");
            }
            
            ImGui::EndChild();
            
            // Drop target for the entire components area
            if (ImGui::BeginDragDropTarget()) {
                // Accept single waveform
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WAVEFORM")) {
                    if (payload->DataSize == sizeof(Waveform)) {
                        Waveform droppedWave = *(Waveform*)payload->Data;
                        // Add to selected track
                        if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                            tracks_[selectedTrackIndex_].oscillators.push_back(Oscillator(droppedWave, 1.0f, 0.5f));
                            tracks_[selectedTrackIndex_].synth->setOscillators(tracks_[selectedTrackIndex_].oscillators);
                            
                            // Set instrument name based on waveform
                            const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
                            tracks_[selectedTrackIndex_].instrumentName = waveNames[static_cast<int>(droppedWave)];
                            
                            markDirty();
                        }
                    }
                }
                // Accept instrument preset
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INSTRUMENT")) {
                    if (payload->DataSize == sizeof(size_t)) {
                        size_t presetIndex = *(size_t*)payload->Data;
                        if (presetIndex < instrumentPresets_.size() && !tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                            const auto& preset = instrumentPresets_[presetIndex];
                            // If sampler is active, remove it when loading a synth preset
                            tracks_[selectedTrackIndex_].hasSampler = false;
                            tracks_[selectedTrackIndex_].samplerSamplePath.clear();
                            tracks_[selectedTrackIndex_].samplerWaveform.clear();
                            tracks_[selectedTrackIndex_].sampler.reset();
                            // Replace all oscillators with the preset's oscillators
                            tracks_[selectedTrackIndex_].oscillators = preset.oscillators;
                            tracks_[selectedTrackIndex_].synth->setOscillators(tracks_[selectedTrackIndex_].oscillators);
                            // Apply envelope settings
                            tracks_[selectedTrackIndex_].synth->setEnvelope(preset.envelope);
                            tracks_[selectedTrackIndex_].waveformSet = true;
                            tracks_[selectedTrackIndex_].instrumentName = preset.name;
                            markDirty();
                            std::cout << "Loaded preset '" << preset.name << "' onto selected track" << std::endl;
                        }
                    }
                }
                // Accept user preset
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("USER_PRESET")) {
                    if (payload->DataSize == sizeof(size_t)) {
                        size_t presetIndex = *(size_t*)payload->Data;
                        if (presetIndex < userPresets_.size() && !tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                            const auto& preset = userPresets_[presetIndex];
                            // Remove sampler if present
                            tracks_[selectedTrackIndex_].hasSampler = false;
                            tracks_[selectedTrackIndex_].samplerSamplePath.clear();
                            tracks_[selectedTrackIndex_].samplerWaveform.clear();
                            tracks_[selectedTrackIndex_].sampler.reset();
                            // Replace all oscillators with the preset's oscillators
                            tracks_[selectedTrackIndex_].oscillators = preset.oscillators;
                            tracks_[selectedTrackIndex_].synth->setOscillators(tracks_[selectedTrackIndex_].oscillators);
                            tracks_[selectedTrackIndex_].synth->setEnvelope(preset.envelope);  // Apply envelope
                            tracks_[selectedTrackIndex_].waveformSet = true;
                            tracks_[selectedTrackIndex_].instrumentName = preset.name;
                            markDirty();
                            std::cout << "Loaded user preset '" << preset.name << "' onto selected track" << std::endl;
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            
            ImGui::EndTabItem();
        }
        
        // Effects tab - shows effects from selected track
        ImGuiTabItemFlags effectsTabFlags = g_switchToEffectsTab ? ImGuiTabItemFlags_SetSelected : 0;
        if (g_switchToEffectsTab) g_switchToEffectsTab = false;  // Reset flag after use
        if (ImGui::BeginTabItem("Effects", nullptr, effectsTabFlags)) {
            // Ensure selectedTrackIndex_ is valid
            if (selectedTrackIndex_ >= tracks_.size() && !tracks_.empty()) {
                selectedTrackIndex_ = tracks_.size() - 1;
            }
            
            // Show track name at top
            if (!tracks_.empty() && selectedTrackIndex_ < tracks_.size()) {
                // Track indicator with color
                static const ImU32 trackColorsEff[24] = {
                    IM_COL32(255, 149, 0, 255), IM_COL32(255, 94, 94, 255),
                    IM_COL32(255, 94, 153, 255), IM_COL32(219, 103, 186, 255),
                    IM_COL32(179, 102, 219, 255), IM_COL32(138, 103, 219, 255),
                    IM_COL32(102, 128, 219, 255), IM_COL32(51, 153, 219, 255),
                    IM_COL32(51, 179, 204, 255), IM_COL32(0, 199, 140, 255),
                    IM_COL32(79, 199, 102, 255), IM_COL32(132, 214, 79, 255),
                    IM_COL32(255, 179, 102, 255), IM_COL32(255, 128, 128, 255),
                    IM_COL32(255, 153, 187, 255), IM_COL32(230, 153, 204, 255),
                    IM_COL32(204, 153, 230, 255), IM_COL32(179, 153, 230, 255),
                    IM_COL32(153, 170, 230, 255), IM_COL32(128, 191, 230, 255),
                    IM_COL32(128, 217, 217, 255), IM_COL32(102, 217, 191, 255),
                    IM_COL32(153, 230, 153, 255), IM_COL32(255, 230, 102, 255)
                };
                
                ImDrawList* effDrawList = ImGui::GetWindowDrawList();
                ImVec2 dotPosEff = ImGui::GetCursorScreenPos();
                int colorIdxEff = tracks_[selectedTrackIndex_].colorIndex % 24;
                effDrawList->AddCircleFilled(ImVec2(dotPosEff.x + 6, dotPosEff.y + 8), 5.0f, trackColorsEff[colorIdxEff]);
                ImGui::Dummy(ImVec2(16, 0));
                ImGui::SameLine();
                
                char trackLabel[64];
                if (!tracks_[selectedTrackIndex_].name.empty()) {
                    snprintf(trackLabel, sizeof(trackLabel), "%s - Effects", tracks_[selectedTrackIndex_].name.c_str());
                } else {
                    snprintf(trackLabel, sizeof(trackLabel), "Track %zu - Effects", selectedTrackIndex_ + 1);
                }
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", trackLabel);
                
                ImGui::Separator();
                
                // Hint text for drag & drop
                if (tracks_[selectedTrackIndex_].effects.empty()) {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "  Drag effects from Browser to add");
                    ImGui::Spacing();
                }
                
                ImGui::Separator();
                ImGui::Spacing();
                
                // Create scrollable child window for effects
                ImVec2 effectsRegion = ImGui::GetContentRegionAvail();
                ImGui::BeginChild("EffectsScrollRegion", ImVec2(effectsRegion.x, effectsRegion.y), false);
                
                // Set scroll position if we just added an effect
                if (effectsScrollY_ > 0.0f) {
                    ImGui::SetScrollY(effectsScrollY_);
                    effectsScrollY_ = 0.0f;  // Reset after applying
                }
                
                // Render effect boxes from selected track
                auto& effects = tracks_[selectedTrackIndex_].effects;
                for (size_t effectIdx = 0; effectIdx < effects.size(); ++effectIdx) {
                    ImGui::PushID(static_cast<int>(selectedTrackIndex_ * 10000 + effectIdx));
                    renderEffectBox(selectedTrackIndex_, effectIdx, effects[effectIdx]);
                    ImGui::PopID();
                    ImGui::SameLine();
                }
                
                ImGui::EndChild();
                
                // Drop target for effects - covers entire Effects tab area
                if (ImGui::BeginDragDropTarget()) {
                    // Handle preset drops (effectType + presetIdx)
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("EFFECT_PRESET")) {
                        if (payload->DataSize == sizeof(int) * 2) {
                            int* data = (int*)payload->Data;
                            int effectType = data[0];
                            int presetIdx = data[1];
                            double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                            std::shared_ptr<Effect> newEffect;
                            
                            switch (effectType) {
                                case 0: {
                                    auto reverb = std::make_shared<Reverb>(sampleRate);
                                    reverb->loadPreset(static_cast<Reverb::Preset>(presetIdx));
                                    newEffect = reverb;
                                    break;
                                }
                                case 1: {
                                    auto chorus = std::make_shared<Chorus>(sampleRate);
                                    chorus->loadPreset(static_cast<Chorus::Preset>(presetIdx));
                                    newEffect = chorus;
                                    break;
                                }
                                case 2: {
                                    auto distortion = std::make_shared<Distortion>(sampleRate);
                                    distortion->loadPreset(static_cast<Distortion::Preset>(presetIdx));
                                    newEffect = distortion;
                                    break;
                                }
                                case 3: {
                                    auto eq8 = std::make_shared<EQ8>(sampleRate);
                                    eq8->loadPreset(static_cast<EQ8::Preset>(presetIdx));
                                    newEffect = eq8;
                                    break;
                                }
                                case 4: {
                                    auto pump = std::make_shared<SidechainPump>(sampleRate);
                                    if (presetIdx == 0) { pump->setDepth(-6.0f); pump->setAttackMs(20.0f); pump->setReleaseMs(150.0f); }
                                    else if (presetIdx == 1) { pump->setDepth(-12.0f); pump->setAttackMs(10.0f); pump->setReleaseMs(200.0f); }
                                    else if (presetIdx == 2) { pump->setDepth(-24.0f); pump->setAttackMs(5.0f); pump->setReleaseMs(250.0f); }
                                    else { pump->setDepth(-36.0f); pump->setAttackMs(2.0f); pump->setReleaseMs(300.0f); }
                                    newEffect = pump;
                                    break;
                                }
                                case 5: {
                                    auto wow = std::make_shared<WowFlutter>(sampleRate);
                                    if (presetIdx == 0) { wow->setWowDepthMs(1.0f); wow->setFlutterDepthMs(0.2f); }
                                    else if (presetIdx == 1) { wow->setWowDepthMs(2.5f); wow->setFlutterDepthMs(0.4f); }
                                    else if (presetIdx == 2) { wow->setWowDepthMs(4.0f); wow->setFlutterDepthMs(0.8f); }
                                    else { wow->setWowDepthMs(5.5f); wow->setFlutterDepthMs(1.2f); }
                                    newEffect = wow;
                                    break;
                                }
                                case 6: {
                                    auto br = std::make_shared<BeatRepeat>(sampleRate);
                                    if (presetIdx == 0) br->setIntervalMs(250.0f);
                                    else if (presetIdx == 1) br->setIntervalMs(125.0f);
                                    else if (presetIdx == 2) br->setIntervalMs(167.0f);
                                    else br->setIntervalMs(100.0f);
                                    newEffect = br;
                                    break;
                                }
                                case 7: {
                                    auto bit = std::make_shared<BitNoiseTexture>(sampleRate);
                                    if (presetIdx == 0) { bit->setBits(12); bit->setNoise(0.02f); }
                                    else if (presetIdx == 1) { bit->setBits(8); bit->setNoise(0.0f); }
                                    else if (presetIdx == 2) { bit->setBits(16); bit->setNoise(0.05f); }
                                    else { bit->setBits(4); bit->setNoise(0.1f); }
                                    newEffect = bit;
                                    break;
                                }
                                case 8: {
                                    auto res = std::make_shared<ResonatorBank>(sampleRate);
                                    if (presetIdx == 0) { res->setDecay(0.3f); res->setMix(0.5f); }
                                    else if (presetIdx == 1) { res->setDecay(0.6f); res->setMix(0.4f); }
                                    else if (presetIdx == 2) { res->setDecay(0.2f); res->setMix(0.6f); }
                                    else { res->setDecay(0.9f); res->setMix(0.3f); }
                                    newEffect = res;
                                    break;
                                }
                            }
                            
                            if (newEffect && selectedTrackIndex_ < tracks_.size()) {
                                tracks_[selectedTrackIndex_].effects.push_back(newEffect);
                                g_switchToEffectsTab = true;
                                markDirty();
                                effectsScrollY_ = 9999.0f;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
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
    
    const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
    const char* waveName = waveNames[static_cast<int>(osc.waveform)];
    
    // Ableton-style device box
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    
    ImGui::BeginChild(ImGui::GetID("component_box"), ImVec2(180, 140), true, ImGuiWindowFlags_None);
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 boxPos = ImGui::GetWindowPos();
    float boxWidth = ImGui::GetWindowWidth();
    
    // Header bar with waveform name
    float headerHeight = 24.0f;
    drawList->AddRectFilled(boxPos, ImVec2(boxPos.x + boxWidth, boxPos.y + headerHeight),
                           IM_COL32(45, 45, 45, 255), 4.0f, ImDrawFlags_RoundCornersTop);
    
    // Title
    drawList->AddText(ImVec2(boxPos.x + 8, boxPos.y + 4), IM_COL32(200, 200, 200, 255), waveName);
    
    // Close button (X)
    auto& oscillators = tracks_[trackIndex].oscillators;
    if (oscillators.size() > 1) {
        ImVec2 closePos(boxPos.x + boxWidth - 20, boxPos.y + 4);
        ImVec2 mousePos = ImGui::GetMousePos();
        bool closeHovered = (mousePos.x >= closePos.x - 4 && mousePos.x <= closePos.x + 16 &&
                            mousePos.y >= closePos.y - 2 && mousePos.y <= closePos.y + 18);
        
        if (closeHovered) {
            drawList->AddRectFilled(ImVec2(closePos.x - 4, closePos.y - 2),
                                   ImVec2(closePos.x + 12, closePos.y + 16),
                                   IM_COL32(255, 94, 94, 100), 2.0f);
        }
        drawList->AddText(closePos, IM_COL32(120, 120, 120, 255), "x");
        
        if (closeHovered && ImGui::IsMouseClicked(0)) {
        oscillators.erase(oscillators.begin() + oscIndex);
        tracks_[trackIndex].synth->setOscillators(oscillators);
            markDirty();
        ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
        ImGui::PopID();
        return;
    }
    }
    
    // Skip past header
    ImGui::Dummy(ImVec2(0, headerHeight - 8));
    ImGui::Spacing();
    
    // Ableton-style slider colors
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.584f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.30f, 0.70f, 0.95f, 1.0f));
    
    // Waveform selector - compact
    ImGui::SetNextItemWidth(100);
    int currentWave = static_cast<int>(osc.waveform);
    if (ImGui::Combo("##wave", &currentWave, waveNames, 4)) {
        osc.waveform = static_cast<Waveform>(currentWave);
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Wave");
    
    // Frequency multiplier
    ImGui::SetNextItemWidth(100);
    float freqMult = osc.frequencyMultiplier;
    if (ImGui::SliderFloat("##freq", &freqMult, 0.1f, 4.0f, "%.2f")) {
        osc.frequencyMultiplier = freqMult;
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Freq");
    
    // Amplitude
    ImGui::SetNextItemWidth(100);
    float amp = osc.amplitude;
    if (ImGui::SliderFloat("##amp", &amp, 0.0f, 1.0f, "%.2f")) {
        osc.amplitude = amp;
        tracks_[trackIndex].synth->setOscillators(oscillators);
        markDirty();
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Amp");
    
    ImGui::PopStyleColor(4);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
#endif
}

// Interactive envelope display - returns true if envelope was modified
static bool DrawInteractiveEnvelope(ImDrawList* drawList, ImVec2 pos, float width, float height,
                                    float& attack, float& decay, float& sustain, float& release) {
    bool modified = false;
    static int draggingPoint = -1; // 0=attack, 1=decay/sustain, 2=release
    
    // Background
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(20, 25, 30, 255));
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(50, 80, 120, 255));
    
    // Grid lines
    for (int i = 1; i < 4; i++) {
        float y = pos.y + height * i / 4.0f;
        drawList->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + width, y), IM_COL32(35, 45, 55, 60));
    }
    
    // Fixed time scale for better visualization
    float maxTime = 3.0f; // 3 seconds total view
    float sustainPhase = 0.4f; // sustain phase duration in view
    float scale = width / maxTime;
    
    // Calculate envelope points
    float x0 = pos.x + 2;
    float y0 = pos.y + height - 2;
    float x1 = pos.x + std::min(attack, maxTime * 0.3f) * scale;
    float y1 = pos.y + 4;
    float x2 = x1 + std::min(decay, maxTime * 0.3f) * scale;
    float y2 = pos.y + height * (1.0f - sustain) + 4;
    float x3 = x2 + sustainPhase * scale;
    float y3 = y2;
    float x4 = std::min(x3 + std::min(release, maxTime * 0.4f) * scale, pos.x + width - 2);
    float y4 = pos.y + height - 4;
    
    // Draw envelope curve
    ImU32 envColor = IM_COL32(80, 180, 220, 255);
    ImU32 envColorDim = IM_COL32(60, 140, 180, 200);
    drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), envColor, 2.0f);
    drawList->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), envColor, 2.0f);
    drawList->AddLine(ImVec2(x2, y2), ImVec2(x3, y3), envColorDim, 2.0f);
    drawList->AddLine(ImVec2(x3, y3), ImVec2(x4, y4), envColor, 2.0f);
    
    // Interactive points
    ImVec2 mousePos = ImGui::GetMousePos();
    bool mouseInArea = mousePos.x >= pos.x && mousePos.x <= pos.x + width &&
                       mousePos.y >= pos.y && mousePos.y <= pos.y + height;
    
    // Point 1: Attack (controls attack time)
    ImVec2 p1 = ImVec2(x1, y1);
    bool hover1 = (mousePos.x >= p1.x - 8 && mousePos.x <= p1.x + 8 && 
                   mousePos.y >= p1.y - 8 && mousePos.y <= p1.y + 8);
    drawList->AddCircleFilled(p1, hover1 ? 6 : 4, hover1 ? IM_COL32(255, 200, 100, 255) : envColor);
    
    // Point 2: Decay/Sustain (controls decay time and sustain level)
    ImVec2 p2 = ImVec2(x2, y2);
    bool hover2 = (mousePos.x >= p2.x - 8 && mousePos.x <= p2.x + 8 && 
                   mousePos.y >= p2.y - 8 && mousePos.y <= p2.y + 8);
    drawList->AddCircleFilled(p2, hover2 ? 6 : 4, hover2 ? IM_COL32(255, 200, 100, 255) : envColor);
    
    // Point 3: Release (controls release time)
    ImVec2 p3 = ImVec2(x4, y4);
    bool hover3 = (mousePos.x >= p3.x - 8 && mousePos.x <= p3.x + 8 && 
                   mousePos.y >= p3.y - 8 && mousePos.y <= p3.y + 8);
    drawList->AddCircleFilled(p3, hover3 ? 6 : 4, hover3 ? IM_COL32(255, 200, 100, 255) : envColor);
    
    // Handle dragging
    if (ImGui::IsMouseClicked(0) && mouseInArea) {
        if (hover1) draggingPoint = 0;
        else if (hover2) draggingPoint = 1;
        else if (hover3) draggingPoint = 2;
    }
    
    if (!ImGui::IsMouseDown(0)) {
        draggingPoint = -1;
    }
    
    if (draggingPoint >= 0 && ImGui::IsMouseDragging(0)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        
        if (draggingPoint == 0) {
            // Attack: horizontal = time
            attack = std::clamp(attack + delta.x / scale, 0.001f, 2.0f);
            modified = true;
        } else if (draggingPoint == 1) {
            // Decay/Sustain: horizontal = decay time, vertical = sustain level
            decay = std::clamp(decay + delta.x / scale, 0.001f, 2.0f);
            sustain = std::clamp(sustain - delta.y / height, 0.0f, 1.0f);
            modified = true;
        } else if (draggingPoint == 2) {
            // Release: horizontal = time
            release = std::clamp(release + delta.x / scale, 0.001f, 5.0f);
            modified = true;
        }
    }
    
    // Labels on envelope
    char buf[16];
    snprintf(buf, sizeof(buf), "A:%.0fms", attack * 1000);
    drawList->AddText(ImVec2(pos.x + 4, pos.y + 2), IM_COL32(100, 100, 100, 255), buf);
    snprintf(buf, sizeof(buf), "S:%.0f%%", sustain * 100);
    drawList->AddText(ImVec2(x2 + 4, y2 - 12), IM_COL32(100, 100, 100, 255), buf);
    
    return modified;
}

void MainWindow::renderInstrumentPanel(size_t trackIndex) {
#ifdef PAN_USE_GUI
    if (trackIndex >= tracks_.size() || !tracks_[trackIndex].synth) return;
    
    auto& synth = tracks_[trackIndex].synth;
    auto& env = synth->getEnvelope();
    auto& oscillators = tracks_[trackIndex].oscillators;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    float panelWidth = ImGui::GetContentRegionAvail().x / 3.0f; // 1/3rd of components panel width
    float panelHeight = 140.0f; // Fixed compact height like Ableton
    
    // Colors
    ImU32 panelBg = IM_COL32(52, 52, 54, 255);
    ImU32 sectionBg = IM_COL32(42, 42, 44, 255);
    ImU32 headerBg = IM_COL32(38, 38, 40, 255);
    ImU32 textDim = IM_COL32(110, 110, 110, 255);
    ImU32 textBright = IM_COL32(190, 190, 190, 255);
    ImU32 accentColor = IM_COL32(220, 130, 50, 255);
    
    drawList->AddRectFilled(startPos, ImVec2(startPos.x + panelWidth, startPos.y + panelHeight), panelBg);
    
    // Compact header bar
    float headerHeight = 20.0f;
    drawList->AddRectFilled(startPos, ImVec2(startPos.x + panelWidth, startPos.y + headerHeight), headerBg);
    
    // Power indicator + name
    drawList->AddCircleFilled(ImVec2(startPos.x + 10, startPos.y + headerHeight/2), 4, accentColor);
    std::string title = tracks_[trackIndex].instrumentName.empty() ? "Synth" : tracks_[trackIndex].instrumentName;
    drawList->AddText(ImVec2(startPos.x + 20, startPos.y + 3), textBright, title.c_str());
    
    // Quick controls in header
    float hdrX = startPos.x + 150;
    
    // Filter toggle
    ImGui::SetCursorScreenPos(ImVec2(hdrX, startPos.y + 2));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 2));
    bool filtOn = env.filter.enabled;
    if (ImGui::Checkbox("Flt", &filtOn)) env.filter.enabled = filtOn;
    ImGui::SameLine();
    bool lfoOn = env.lfo1.enabled;
    if (ImGui::Checkbox("LFO", &lfoOn)) env.lfo1.enabled = lfoOn;
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    
    // Content area
    ImVec2 contentPos = ImVec2(startPos.x + 2, startPos.y + headerHeight + 2);
    float contentHeight = panelHeight - headerHeight - 4;
    
    // === COMPACT LAYOUT: Oscillators | Envelope | Controls ===
    
    // Left: Oscillator mini-panels (stacked vertically, very compact)
    float oscWidth = 100.0f;
    float oscRowH = contentHeight / std::max((int)oscillators.size(), 2);
    
    for (int op = 0; op < (int)oscillators.size() && op < 4; op++) {
        ImVec2 opPos = ImVec2(contentPos.x, contentPos.y + op * oscRowH);
        drawList->AddRectFilled(opPos, ImVec2(opPos.x + oscWidth, opPos.y + oscRowH - 1), sectionBg, 2.0f);
        
        ImGui::SetCursorScreenPos(ImVec2(opPos.x + 2, opPos.y + 2));
        ImGui::PushID(op * 100);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.2f, 1.0f));
        ImGui::PushItemWidth(40);
        
        float freq = oscillators[op].frequencyMultiplier;
        if (ImGui::DragFloat("##f", &freq, 0.1f, 0.1f, 16.0f, "%.1f")) {
            oscillators[op].frequencyMultiplier = freq;
        }
        ImGui::SameLine();
        float amp = oscillators[op].amplitude;
        if (ImGui::DragFloat("##a", &amp, 0.01f, 0.0f, 1.0f, "%.2f")) {
            oscillators[op].amplitude = amp;
        }
        
        // Mini level bar
        float barH = oscRowH - 6;
        float barFill = amp * barH;
        drawList->AddRectFilled(ImVec2(opPos.x + oscWidth - 8, opPos.y + 2 + barH - barFill),
                               ImVec2(opPos.x + oscWidth - 3, opPos.y + 2 + barH),
                               IM_COL32(200, 90, 60, 255));
        
        ImGui::PopItemWidth();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
    }
    
    // Center: Interactive Envelope
    float envX = contentPos.x + oscWidth + 4;
    float envWidth = panelWidth - oscWidth - 200;
    float envHeight = contentHeight - 34;
    
    // Draw interactive envelope
    DrawInteractiveEnvelope(drawList, ImVec2(envX, contentPos.y), envWidth, envHeight,
                           env.ampEnvelope.attack, env.ampEnvelope.decay,
                           env.ampEnvelope.sustain, env.ampEnvelope.release);
    
    // Parameter inputs below envelope (compact row)
    ImGui::SetCursorScreenPos(ImVec2(envX, contentPos.y + envHeight + 2));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.2f, 1.0f));
    ImGui::PushItemWidth(50);
    
    float attackMs = env.ampEnvelope.attack * 1000;
    if (ImGui::DragFloat("A", &attackMs, 1.0f, 0.0f, 5000.0f, "%.0f")) {
        env.ampEnvelope.attack = attackMs / 1000.0f;
    }
    ImGui::SameLine();
    float decayMs = env.ampEnvelope.decay * 1000;
    if (ImGui::DragFloat("D", &decayMs, 1.0f, 0.0f, 5000.0f, "%.0f")) {
        env.ampEnvelope.decay = decayMs / 1000.0f;
    }
    ImGui::SameLine();
    float sus = env.ampEnvelope.sustain * 100;
    if (ImGui::DragFloat("S", &sus, 1.0f, 0.0f, 100.0f, "%.0f%%")) {
        env.ampEnvelope.sustain = sus / 100.0f;
    }
    ImGui::SameLine();
    float releaseMs = env.ampEnvelope.release * 1000;
    if (ImGui::DragFloat("R", &releaseMs, 1.0f, 0.0f, 10000.0f, "%.0f")) {
        env.ampEnvelope.release = releaseMs / 1000.0f;
    }
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(2);
    
    // Right: Filter/LFO controls (compact vertical stack)
    float rightX = startPos.x + panelWidth - 190;
    float ctrlH = contentHeight / 3.0f;
    
    // Filter section
    ImVec2 filtPos = ImVec2(rightX, contentPos.y);
    drawList->AddRectFilled(filtPos, ImVec2(filtPos.x + 185, filtPos.y + ctrlH - 1), sectionBg, 2.0f);
    drawList->AddCircleFilled(ImVec2(filtPos.x + 6, filtPos.y + 8), 3, env.filter.enabled ? accentColor : IM_COL32(60, 60, 60, 255));
    drawList->AddText(ImVec2(filtPos.x + 12, filtPos.y + 2), textDim, "Filter");
    
    ImGui::SetCursorScreenPos(ImVec2(filtPos.x + 50, filtPos.y + 2));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.2f, 1.0f));
    ImGui::PushItemWidth(45);
    float freq = env.filter.cutoff * 20000;
    if (ImGui::DragFloat("Hz", &freq, 10.0f, 20.0f, 20000.0f, "%.0f")) {
        env.filter.cutoff = freq / 20000.0f;
    }
    ImGui::SameLine();
    float res = env.filter.resonance * 100;
    if (ImGui::DragFloat("Q", &res, 1.0f, 0.0f, 100.0f, "%.0f")) {
        env.filter.resonance = res / 100.0f;
    }
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(2);
    
    // LFO section
    ImVec2 lfoPos = ImVec2(rightX, contentPos.y + ctrlH);
    drawList->AddRectFilled(lfoPos, ImVec2(lfoPos.x + 185, lfoPos.y + ctrlH - 1), sectionBg, 2.0f);
    drawList->AddCircleFilled(ImVec2(lfoPos.x + 6, lfoPos.y + 8), 3, env.lfo1.enabled ? accentColor : IM_COL32(60, 60, 60, 255));
    drawList->AddText(ImVec2(lfoPos.x + 12, lfoPos.y + 2), textDim, "LFO");
    
    ImGui::SetCursorScreenPos(ImVec2(lfoPos.x + 40, lfoPos.y + 2));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.2f, 1.0f));
    ImGui::PushItemWidth(40);
    float rate = env.lfo1.rate;
    if (ImGui::DragFloat("Rt", &rate, 0.1f, 0.01f, 50.0f, "%.1f")) {
        env.lfo1.rate = rate;
    }
    ImGui::SameLine();
    float depth = env.lfo1.depth * 100;
    if (ImGui::DragFloat("Dp", &depth, 1.0f, 0.0f, 100.0f, "%.0f")) {
        env.lfo1.depth = depth / 100.0f;
    }
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(2);
    
    // Pitch section
    ImVec2 pitchPos = ImVec2(rightX, contentPos.y + ctrlH * 2);
    drawList->AddRectFilled(pitchPos, ImVec2(pitchPos.x + 185, pitchPos.y + ctrlH - 1), sectionBg, 2.0f);
    drawList->AddCircleFilled(ImVec2(pitchPos.x + 6, pitchPos.y + 8), 3, env.pitchEnvelope.enabled ? accentColor : IM_COL32(60, 60, 60, 255));
    drawList->AddText(ImVec2(pitchPos.x + 12, pitchPos.y + 2), textDim, "Pitch");
    
    ImGui::SetCursorScreenPos(ImVec2(pitchPos.x + 45, pitchPos.y + 2));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.55f, 0.2f, 1.0f));
    ImGui::PushItemWidth(40);
    float pamt = (env.pitchEnvelope.startMultiplier - 1.0f) * 100;
    if (ImGui::DragFloat("St", &pamt, 1.0f, -100.0f, 300.0f, "%.0f")) {
        env.pitchEnvelope.startMultiplier = 1.0f + pamt / 100.0f;
    }
    ImGui::SameLine();
    float pdec = env.pitchEnvelope.decayTime * 1000;
    if (ImGui::DragFloat("Tm", &pdec, 1.0f, 1.0f, 500.0f, "%.0f")) {
        env.pitchEnvelope.decayTime = pdec / 1000.0f;
    }
    ImGui::PopItemWidth();
    ImGui::PopStyleColor(2);
    
    // Move cursor past the panel and add Dummy to extend bounds
    ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + panelHeight + 4));
    ImGui::Dummy(ImVec2(panelWidth, 1));
#endif
}

void MainWindow::renderSamplerPanel(size_t trackIndex) {
#ifdef PAN_USE_GUI
    if (trackIndex >= tracks_.size() || !tracks_[trackIndex].synth) return;
    
    auto& track = tracks_[trackIndex];
    auto& params = track.samplerParams;
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 startPos = ImGui::GetCursorScreenPos();
    
    // Compact panel - 1/3 width like synth instrument panel
    float panelWidth = ImGui::GetContentRegionAvail().x / 3.0f;
    float panelHeight = 200.0f;  // extra room for knob labels
    
    // Ableton-style colors
    ImU32 panelBg = IM_COL32(50, 50, 52, 255);
    ImU32 headerBg = IM_COL32(35, 35, 38, 255);
    ImU32 waveformBg = IM_COL32(22, 24, 28, 255);
    ImU32 textDim = IM_COL32(130, 130, 135, 255);
    ImU32 textBright = IM_COL32(220, 220, 220, 255);
    ImU32 accentColor = IM_COL32(255, 170, 70, 255);
    ImU32 accentOrange = IM_COL32(255, 149, 0, 255);
    ImU32 buttonOn = IM_COL32(200, 120, 40, 255);
    ImU32 buttonOff = IM_COL32(50, 50, 52, 255);
    
    // Helper: set slice markers based on BPM grid (1/1,1/2,1/4,1/8,1/16)
    auto setSliceGridFromTempo = [&](int denom) {
        params.sliceMarkers.clear();
        int slicesPerBar = 0;
        switch (denom) {
            case 1:  slicesPerBar = 1;  break; // whole note per bar
            case 2:  slicesPerBar = 2;  break; // half notes
            case 4:  slicesPerBar = 4;  break; // quarter notes
            case 8:  slicesPerBar = 8;  break; // eighth notes
            case 16: slicesPerBar = 16; break; // sixteenth notes
            default: slicesPerBar = 4;  break;
        }
        
        double durationSec = (track.sampler && track.sampler->getSampleDuration() > 0.0)
            ? track.sampler->getSampleDuration()
            : 0.0;
        if (durationSec <= 0.0) {
            // Fall back to uniform division
            for (int s = 1; s < slicesPerBar; ++s) params.sliceMarkers.push_back(static_cast<float>(s) / slicesPerBar);
            return;
        }
        
        // beat duration (quarter note) seconds
        double beatSec = 60.0 / std::max(1.0f, bpm_);
        // note length in beats
        double beatsPerNote = (denom == 1 ? 4.0 : denom == 2 ? 2.0 : denom == 4 ? 1.0 : denom == 8 ? 0.5 : 0.25);
        double stepSec = beatSec * beatsPerNote;
        int totalSlices = std::max(1, static_cast<int>(std::floor(durationSec / stepSec)));
        for (int s = 1; s < totalSlices; ++s) {
            double t = s * stepSec;
            float rel = static_cast<float>(t / durationSec);
            if (rel > 0.0f && rel < 1.0f) params.sliceMarkers.push_back(rel);
        }
    };
    
    // Panel background with subtle border
    drawList->AddRectFilled(startPos, ImVec2(startPos.x + panelWidth, startPos.y + panelHeight), panelBg, 3.0f);
    drawList->AddRect(startPos, ImVec2(startPos.x + panelWidth, startPos.y + panelHeight), 
                     IM_COL32(70, 70, 72, 255), 3.0f);
    
    // === HEADER ===
    float headerHeight = 24.0f;
    drawList->AddRectFilled(startPos, ImVec2(startPos.x + panelWidth, startPos.y + headerHeight), headerBg, 3.0f);
    
    // Power indicator
    drawList->AddCircleFilled(ImVec2(startPos.x + 10, startPos.y + 10), 4, accentColor);
    
    // Sample name (truncated)
    std::string title = track.samplerSamplePath.empty() ? "Sampler" 
        : std::filesystem::path(track.samplerSamplePath).stem().string();
    float maxTitleW = panelWidth * 0.35f;
    ImVec2 titleSz = ImGui::CalcTextSize(title.c_str());
    if (titleSz.x > maxTitleW && title.length() > 6) {
        while (titleSz.x > maxTitleW && title.length() > 6) {
            title = title.substr(0, title.length() - 4) + "...";
            titleSz = ImGui::CalcTextSize(title.c_str());
        }
    }
    drawList->AddText(ImVec2(startPos.x + 20, startPos.y + 4), textBright, title.c_str());
    
    // Mode buttons (Classic / 1-Shot / Slice) - right of title
    float modeX = startPos.x + 20 + maxTitleW + 6;
    const char* modes[] = {"Classic", "1-Shot", "Slice"};
    for (int m = 0; m < 3; ++m) {
        bool active = static_cast<int>(params.mode) == m;
        ImVec2 ms = ImGui::CalcTextSize(modes[m]);
        float bw = ms.x + 12;
        ImVec2 bMin(modeX, startPos.y + 2);
        ImVec2 bMax(modeX + bw, startPos.y + headerHeight - 2);
        drawList->AddRectFilled(bMin, bMax, active ? IM_COL32(255, 170, 70, 255) : IM_COL32(60, 60, 64, 255), 3.0f);
        drawList->AddRect(bMin, bMax, IM_COL32(80, 80, 85, 255), 3.0f);
        drawList->AddText(ImVec2(modeX + 6, startPos.y + 4), active ? IM_COL32(0,0,0,255) : textDim, modes[m]);
        ImGui::SetCursorScreenPos(bMin);
        char id[32]; snprintf(id, sizeof(id), "##modebtn_%d", m);
        ImGui::InvisibleButton(id, ImVec2(bw, headerHeight - 4));
        if (ImGui::IsItemClicked()) {
            params.mode = static_cast<SamplerMode>(m);
            if (track.sampler) track.sampler->setMode(params.mode);
            // Auto-slice when entering Slice mode if not custom
            if (params.mode == SamplerMode::Slice && !params.sliceCustom) {
                setSliceGridFromTempo(params.sliceGridSlices);
            }
            markDirty();
        }
        modeX += bw + 4;
    }
    
    // Close button
    ImVec2 closePos(startPos.x + panelWidth - 16, startPos.y + 5);
    drawList->AddText(closePos, textDim, "x");
    ImGui::SetCursorScreenPos(closePos);
    ImGui::InvisibleButton("##closeSampler", ImVec2(14, 14));
    if (ImGui::IsItemClicked()) {
        track.hasSampler = false;
        markDirty();
    }
    
    // === CONTENT ===
    float contentY = startPos.y + headerHeight + 4;
    float contentX = startPos.x + 4;
    float contentW = panelWidth - 8;
    
    // Waveform area - takes most of the panel
    float waveH = 90.0f;
    ImVec2 waveMin(contentX, contentY);
    ImVec2 waveMax(contentX + contentW, contentY + waveH);
    
    drawList->AddRectFilled(waveMin, waveMax, waveformBg, 2.0f);
    drawList->AddRect(waveMin, waveMax, IM_COL32(60, 80, 110, 255), 2.0f);
    
    // Draw waveform
    if (!track.samplerWaveform.empty()) {
        float centerY = waveMin.y + waveH / 2.0f;
        float xStep = contentW / track.samplerWaveform.size();
        
        size_t startIdx = static_cast<size_t>(params.startPos * track.samplerWaveform.size());
        size_t endIdx = static_cast<size_t>((params.startPos + params.length) * track.samplerWaveform.size());
        endIdx = std::min(endIdx, track.samplerWaveform.size());
        
        for (size_t j = 0; j < track.samplerWaveform.size(); ++j) {
            float x = waveMin.x + j * xStep;
            float amp = track.samplerWaveform[j] * (waveH / 2.0f - 4.0f);
            bool inRegion = (j >= startIdx && j < endIdx);
            ImU32 waveCol = inRegion ? IM_COL32(255, 175, 70, 255) : IM_COL32(100, 80, 50, 180);
            drawList->AddLine(ImVec2(x, centerY - amp), ImVec2(x, centerY + amp), waveCol);
        }
        
        // Start/end markers
        float startX = waveMin.x + params.startPos * contentW;
        float endX = waveMin.x + (params.startPos + params.length) * contentW;
        drawList->AddLine(ImVec2(startX, waveMin.y), ImVec2(startX, waveMax.y), accentOrange, 2.0f);
        drawList->AddLine(ImVec2(endX, waveMin.y), ImVec2(endX, waveMax.y), accentOrange, 2.0f);
        
        // Draggable handles (Classic mode only)
        auto drawHandle = [&](float x, bool isStart) {
            ImVec2 p1(x, waveMin.y);
            ImVec2 p2(x, waveMax.y);
            drawList->AddLine(p1, p2, accentOrange, 2.0f);
            float triW = 6.0f, triH = 8.0f;
            if (isStart) {
                drawList->AddTriangleFilled(ImVec2(x, waveMin.y - 1), ImVec2(x + triW, waveMin.y - triH), ImVec2(x - triW, waveMin.y - triH), accentOrange);
            } else {
                drawList->AddTriangleFilled(ImVec2(x, waveMin.y - 1), ImVec2(x + triW, waveMin.y - triH), ImVec2(x - triW, waveMin.y - triH), accentOrange);
            }
        };
        if (params.mode == SamplerMode::Classic) {
            drawHandle(startX, true);
            drawHandle(endX, false);
            // Drag logic
            ImGui::SetCursorScreenPos(ImVec2(startX - 6, waveMin.y));
            ImGui::InvisibleButton("##dragStart", ImVec2(12, waveH));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                float rel = (ImGui::GetIO().MousePos.x - waveMin.x) / contentW;
                rel = std::clamp(rel, 0.0f, params.startPos + params.length - 0.01f);
                params.length -= (rel - params.startPos);
                params.startPos = rel;
                markDirty();
            ImGui::GetIO().WantCaptureMouse = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        } else if (ImGui::IsItemHovered()) {
            // keep cursor normal
            }
            ImGui::SetCursorScreenPos(ImVec2(endX - 6, waveMin.y));
            ImGui::InvisibleButton("##dragEnd", ImVec2(12, waveH));
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
                float rel = (ImGui::GetIO().MousePos.x - waveMin.x) / contentW;
                rel = std::clamp(rel, params.startPos + 0.01f, 1.0f);
                params.length = rel - params.startPos;
                markDirty();
            ImGui::GetIO().WantCaptureMouse = true;
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        } else if (ImGui::IsItemHovered()) {
            // keep cursor normal
            }
        }
        
        // Slice markers (Slice mode)
        if (params.mode == SamplerMode::Slice) {
            for (size_t i = 0; i < params.sliceMarkers.size(); ++i) {
                float x = waveMin.x + params.sliceMarkers[i] * contentW;
                drawList->AddLine(ImVec2(x, waveMin.y), ImVec2(x, waveMax.y), IM_COL32(180, 120, 255, 255), 2.0f);
            }
        }
    } else {
        const char* dropText = "Drop sample here";
        ImVec2 txtSz = ImGui::CalcTextSize(dropText);
        drawList->AddText(ImVec2(waveMin.x + (contentW - txtSz.x) / 2, waveMin.y + (waveH - txtSz.y) / 2),
                         textDim, dropText);
    }
    
    // Drop target
    ImGui::SetCursorScreenPos(waveMin);
    ImGui::InvisibleButton("##waveformDrop", ImVec2(contentW, waveH));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SAMPLE")) {
            if (payload->DataSize == sizeof(size_t)) {
                size_t sampleIdx = *(size_t*)payload->Data;
                if (sampleIdx < userSamples_.size()) {
                    track.samplerSamplePath = userSamples_[sampleIdx].path;
                    track.samplerWaveform = userSamples_[sampleIdx].waveformDisplay;
                    track.instrumentName = "Sampler: " + userSamples_[sampleIdx].name;
                    if (!track.sampler) {
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        track.sampler = std::make_shared<Sampler>(sampleRate);
                    }
                    track.sampler->loadSample(userSamples_[sampleIdx].path);
                    markDirty();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    // === CONTROLS ROW ===
    float ctrlY = contentY + waveH + 12;
    float ctrlW = contentW / 5.0f;  // 5 main controls
    float ctrlX = contentX;
    char valBuf[32];
    
    // Helper to draw a compact knob-style control
    auto drawKnob = [&](const char* label, const char* value, float x, float y, float w,
                        const char* id, float& param, float minV, float maxV, float sens, bool vertical = true) {
        // Knob circle
        float knobR = 12.0f;
        float knobCX = x + w / 2;
        float knobCY = y + 16;
        
        // Draw knob background
        drawList->AddCircleFilled(ImVec2(knobCX, knobCY), knobR, IM_COL32(40, 40, 42, 255));
        drawList->AddCircle(ImVec2(knobCX, knobCY), knobR, IM_COL32(80, 80, 85, 255), 0, 1.5f);
        
        // Draw knob indicator (arc)
        float normalized = (param - minV) / (maxV - minV);
        float startAngle = 2.35f;  // ~135 degrees
        float endAngle = startAngle + normalized * 4.7f;  // ~270 degree range
        drawList->PathArcTo(ImVec2(knobCX, knobCY), knobR - 2, startAngle, endAngle, 12);
        drawList->PathStroke(accentColor, false, 3.0f);
        
        // Knob notch
        float notchAngle = startAngle + normalized * 4.7f;
        float notchX = knobCX + std::cos(notchAngle) * (knobR - 4);
        float notchY = knobCY + std::sin(notchAngle) * (knobR - 4);
        drawList->AddCircleFilled(ImVec2(notchX, notchY), 2.0f, textBright);
        
        // Label below knob
        ImVec2 labelSz = ImGui::CalcTextSize(label);
        drawList->AddText(ImVec2(knobCX - labelSz.x / 2, knobCY + knobR + 4), textDim, label);
        
        // Value below label
        ImVec2 valSz = ImGui::CalcTextSize(value);
        drawList->AddText(ImVec2(knobCX - valSz.x / 2, knobCY + knobR + 16), accentColor, value);
        
        // Interaction
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::InvisibleButton(id, ImVec2(w, 48));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            ImGui::GetIO().WantCaptureMouse = true;
            float delta = vertical ? ImGui::GetIO().MouseDelta.y * -sens : ImGui::GetIO().MouseDelta.x * sens;
            param = std::clamp(param + delta, minV, maxV);
            markDirty();
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        }
        // Keep cursor normal unless dragging
    };
    
    // Gain knob
    snprintf(valBuf, sizeof(valBuf), "%.0fdB", params.gain);
    drawKnob("Gain", valBuf, ctrlX, ctrlY, ctrlW, "##gain", params.gain, -60.0f, 12.0f, 0.3f);
    ctrlX += ctrlW;
    
    // Attack knob
    snprintf(valBuf, sizeof(valBuf), "%.0fms", params.attack * 1000);
    drawKnob("Attack", valBuf, ctrlX, ctrlY, ctrlW, "##attack", params.attack, 0.0f, 5.0f, 0.005f);
    ctrlX += ctrlW;
    
    // Decay knob
    snprintf(valBuf, sizeof(valBuf), "%.0fms", params.decay * 1000);
    drawKnob("Decay", valBuf, ctrlX, ctrlY, ctrlW, "##decay", params.decay, 0.0f, 5.0f, 0.005f);
    ctrlX += ctrlW;
    
    // Sustain knob
    snprintf(valBuf, sizeof(valBuf), "%.0f%%", params.sustain * 100);
    drawKnob("Sustain", valBuf, ctrlX, ctrlY, ctrlW, "##sustain", params.sustain, 0.0f, 1.0f, 0.005f);
    ctrlX += ctrlW;
    
    // Release knob
    snprintf(valBuf, sizeof(valBuf), "%.0fms", params.release * 1000);
    drawKnob("Release", valBuf, ctrlX, ctrlY, ctrlW, "##release", params.release, 0.0f, 5.0f, 0.005f);
    
    // === MODE AND TOGGLE BUTTONS (right side of waveform) ===
    // These overlay on the right edge of waveform area
    float btnX = waveMax.x - 48;
    float btnY = waveMin.y + 4;
    float btnW = 42;
    float btnH = 14;
    
    // Move LOOP/SNAP under waveform, left aligned
    float toggleY = contentY + waveH + 2;
    float toggleW = 46;
    // Loop button
    ImVec2 loopMin(contentX, toggleY);
    ImVec2 loopMax(contentX + toggleW, toggleY + btnH);
    drawList->AddRectFilled(loopMin, loopMax, params.loopEnabled ? buttonOn : buttonOff, 2.0f);
    drawList->AddRect(loopMin, loopMax, IM_COL32(80, 80, 85, 255), 2.0f);
    ImVec2 loopTxtSz = ImGui::CalcTextSize("LOOP");
    drawList->AddText(ImVec2(loopMin.x + (toggleW - loopTxtSz.x) / 2, toggleY + 1), 
                     params.loopEnabled ? IM_COL32(255, 255, 255, 255) : textDim, "LOOP");
    ImGui::SetCursorScreenPos(loopMin);
    ImGui::InvisibleButton("##loopBtn", ImVec2(toggleW, btnH));
    if (ImGui::IsItemClicked()) { params.loopEnabled = !params.loopEnabled; markDirty(); }
    // Snap button
    ImVec2 snapMin(loopMax.x + 4, toggleY);
    ImVec2 snapMax(loopMax.x + 4 + toggleW, toggleY + btnH);
    drawList->AddRectFilled(snapMin, snapMax, params.snapEnabled ? buttonOn : buttonOff, 2.0f);
    drawList->AddRect(snapMin, snapMax, IM_COL32(80, 80, 85, 255), 2.0f);
    ImVec2 snapTxtSz = ImGui::CalcTextSize("SNAP");
    drawList->AddText(ImVec2(snapMin.x + (toggleW - snapTxtSz.x) / 2, toggleY + 1), 
                     params.snapEnabled ? IM_COL32(255, 255, 255, 255) : textDim, "SNAP");
    ImGui::SetCursorScreenPos(snapMin);
    ImGui::InvisibleButton("##snapBtn", ImVec2(toggleW, btnH));
    if (ImGui::IsItemClicked()) { params.snapEnabled = !params.snapEnabled; markDirty(); }
    
    // Slice mode dropdown for beat slicing
    if (params.mode == SamplerMode::Slice) {
        const char* sliceOpts[] = {"1/1", "1/2", "1/4", "1/8", "1/16", "Custom"};
        int optCount = 6;
        int currentIdx = params.sliceCustom ? optCount - 1 : 
                         (params.sliceGridSlices == 2 ? 1 :
                          params.sliceGridSlices == 4 ? 2 :
                          params.sliceGridSlices == 8 ? 3 :
                          params.sliceGridSlices == 16 ? 4 : 0);
        ImGui::SetCursorScreenPos(ImVec2(snapMax.x + 8, toggleY - 2));
        ImGui::PushItemWidth(100);
        if (ImGui::BeginCombo("##sliceGrid", sliceOpts[currentIdx])) {
            for (int i = 0; i < optCount; ++i) {
                bool sel = i == currentIdx;
                if (ImGui::Selectable(sliceOpts[i], sel)) {
                    if (i == optCount - 1) {
                        params.sliceCustom = true;
                    } else {
                        params.sliceCustom = false;
                        params.sliceGridSlices = (i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 4 : i == 3 ? 8 : 16);
                        setSliceGridFromTempo(params.sliceGridSlices);
                    }
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        
        // Right-click add/remove slice markers when custom
        // Do not hide cursor on hover
        if (params.sliceCustom && ImGui::IsMouseHoveringRect(waveMin, waveMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            float mouseX = ImGui::GetIO().MousePos.x;
            float rel = (mouseX - waveMin.x) / contentW;
            rel = std::clamp(rel, 0.01f, 0.99f);
            // If near existing marker, remove it
            bool removed = false;
            for (auto it = params.sliceMarkers.begin(); it != params.sliceMarkers.end(); ++it) {
                if (std::abs(*it - rel) < 0.02f) { params.sliceMarkers.erase(it); removed = true; break; }
            }
            if (!removed) params.sliceMarkers.push_back(rel);
            std::sort(params.sliceMarkers.begin(), params.sliceMarkers.end());
            markDirty();
        }
    }
    
    // Sync params to sampler
    if (track.sampler) {
        track.sampler->getParams() = params;
    }
    
    ImGui::SetCursorScreenPos(ImVec2(startPos.x, startPos.y + panelHeight + 4));
    ImGui::Dummy(ImVec2(panelWidth, 1));
#endif
}

void MainWindow::renderEffectBox(size_t trackIndex, size_t effectIndex, std::shared_ptr<Effect> effect) {
#ifdef PAN_USE_GUI
    if (!effect) return;
    
    ImGui::PushID(static_cast<int>(effectIndex + 20000 * trackIndex));
    
    auto reverb = std::dynamic_pointer_cast<Reverb>(effect);
    auto chorus = std::dynamic_pointer_cast<Chorus>(effect);
    auto distortion = std::dynamic_pointer_cast<Distortion>(effect);
    auto eq8 = std::dynamic_pointer_cast<EQ8>(effect);
    auto pump = std::dynamic_pointer_cast<SidechainPump>(effect);
    auto wow = std::dynamic_pointer_cast<WowFlutter>(effect);
    auto beatRepeat = std::dynamic_pointer_cast<BeatRepeat>(effect);
    auto bitNoise = std::dynamic_pointer_cast<BitNoiseTexture>(effect);
    auto resonator = std::dynamic_pointer_cast<ResonatorBank>(effect);
    
    // Ableton-style effect device box
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    
    // EQ8 and WowFlutter need more space
    float boxHeight = eq8 ? 320.0f : (wow ? 340.0f : 220.0f);
    float boxWidth2 = eq8 ? 240.0f : (wow ? 230.0f : 200.0f);
    ImGui::BeginChild(ImGui::GetID("effect_box"), ImVec2(boxWidth2, boxHeight), true, ImGuiWindowFlags_None);
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 boxPos = ImGui::GetWindowPos();
    float boxWidth = ImGui::GetWindowWidth();
    
    // Header bar
    float headerHeight = 24.0f;
    bool enabled = effect->isEnabled();
    ImU32 headerColor = enabled ? IM_COL32(255, 149, 0, 255) : IM_COL32(45, 45, 45, 255);
    drawList->AddRectFilled(boxPos, ImVec2(boxPos.x + boxWidth, boxPos.y + headerHeight),
                           headerColor, 4.0f, ImDrawFlags_RoundCornersTop);
    
    // Effect name - store the string to prevent dangling pointer
    std::string effectNameStr = effect->getName();
    drawList->AddText(ImVec2(boxPos.x + 8, boxPos.y + 4), 
                     enabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 150, 150, 255), 
                     effectNameStr.c_str());
    
    // Enable/disable button (power icon simulation)
    ImVec2 powerPos(boxPos.x + boxWidth - 40, boxPos.y + 3);
    ImVec2 mousePos = ImGui::GetMousePos();
    bool powerHovered = (mousePos.x >= powerPos.x && mousePos.x <= powerPos.x + 18 &&
                        mousePos.y >= powerPos.y && mousePos.y <= powerPos.y + 18);
    
    ImU32 powerColor = enabled ? IM_COL32(255, 255, 255, 255) : IM_COL32(100, 100, 100, 255);
    drawList->AddCircle(ImVec2(powerPos.x + 9, powerPos.y + 9), 7.0f, powerColor, 0, 2.0f);
    drawList->AddLine(ImVec2(powerPos.x + 9, powerPos.y + 2), 
                     ImVec2(powerPos.x + 9, powerPos.y + 7), powerColor, 2.0f);
    
    if (powerHovered && ImGui::IsMouseClicked(0)) {
        effect->setEnabled(!enabled);
        markDirty();
    }
    
    // Close button
    ImVec2 closePos(boxPos.x + boxWidth - 20, boxPos.y + 4);
    bool closeHovered = (mousePos.x >= closePos.x - 4 && mousePos.x <= closePos.x + 16 &&
                        mousePos.y >= closePos.y - 2 && mousePos.y <= closePos.y + 18);
    
    if (closeHovered) {
        drawList->AddRectFilled(ImVec2(closePos.x - 4, closePos.y - 2),
                               ImVec2(closePos.x + 12, closePos.y + 16),
                               IM_COL32(255, 94, 94, 100), 2.0f);
    }
    drawList->AddText(closePos, IM_COL32(200, 200, 200, 255), "x");
    
    if (closeHovered && ImGui::IsMouseClicked(0)) {
        tracks_[trackIndex].effects.erase(tracks_[trackIndex].effects.begin() + effectIndex);
        markDirty();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        return;
    }
    
    // Skip past header
    ImGui::Dummy(ImVec2(0, headerHeight - 8));
    ImGui::Spacing();
    
    // Slider styling
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.07f, 0.07f, 0.07f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(1.0f, 0.584f, 0.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.30f, 0.70f, 0.95f, 1.0f));
    
    if (reverb) {
        // Preset selector with arrow dropdown
        ImGui::SetNextItemWidth(140);
        const char* currentPresetName = Reverb::getPresetName(reverb->getCurrentPreset());
        if (ImGui::BeginCombo("Preset##reverb", currentPresetName)) {
            for (int i = 0; i < 7; ++i) {
                Reverb::Preset preset = static_cast<Reverb::Preset>(i);
                const char* presetName = Reverb::getPresetName(preset);
                if (ImGui::Selectable(presetName, reverb->getCurrentPreset() == preset)) {
                    reverb->loadPreset(preset);
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        
        bool anyChanged = false;
        
        ImGui::SetNextItemWidth(120);
        float roomSize = reverb->getRoomSize();
        if (ImGui::SliderFloat("##room", &roomSize, 0.0f, 1.0f, "%.2f")) {
            reverb->setRoomSize(roomSize);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Size");
        
        ImGui::SetNextItemWidth(120);
        float wet = reverb->getWetLevel();
        if (ImGui::SliderFloat("##wet", &wet, 0.0f, 1.0f, "%.2f")) {
            reverb->setWetLevel(wet);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Wet");
        
        ImGui::SetNextItemWidth(120);
        float dry = reverb->getDryLevel();
        if (ImGui::SliderFloat("##dry", &dry, 0.0f, 1.0f, "%.2f")) {
            reverb->setDryLevel(dry);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Dry");
        
        ImGui::SetNextItemWidth(120);
        float damping = reverb->getDamping();
        if (ImGui::SliderFloat("##damp", &damping, 0.0f, 1.0f, "%.2f")) {
            reverb->setDamping(damping);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Damp");
        
        if (anyChanged && reverb->getCurrentPreset() != Reverb::Preset::Custom) {
            reverb->setCurrentPreset(Reverb::Preset::Custom);
        }
    } else if (chorus) {
        // Chorus preset selector with arrow dropdown
        ImGui::SetNextItemWidth(140);
        const char* currentPresetName = Chorus::getPresetName(chorus->getCurrentPreset());
        if (ImGui::BeginCombo("Preset##chorus", currentPresetName)) {
            for (int i = 0; i < 6; ++i) {
                Chorus::Preset preset = static_cast<Chorus::Preset>(i);
                const char* presetName = Chorus::getPresetName(preset);
                if (ImGui::Selectable(presetName, chorus->getCurrentPreset() == preset)) {
                    chorus->loadPreset(preset);
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        
        bool anyChanged = false;
        
        ImGui::SetNextItemWidth(120);
        float rate = chorus->getRate();
        if (ImGui::SliderFloat("##rate", &rate, 0.1f, 5.0f, "%.1f Hz")) {
            chorus->setRate(rate);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Rate");
        
        ImGui::SetNextItemWidth(120);
        float depth = chorus->getDepth();
        if (ImGui::SliderFloat("##depth", &depth, 0.0f, 10.0f, "%.1f ms")) {
            chorus->setDepth(depth);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Depth");
        
        ImGui::SetNextItemWidth(120);
        float delay = chorus->getDelay();
        if (ImGui::SliderFloat("##delay", &delay, 5.0f, 50.0f, "%.0f ms")) {
            chorus->setDelay(delay);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Delay");
        
        ImGui::SetNextItemWidth(120);
        float mix = chorus->getMix();
        if (ImGui::SliderFloat("##mix", &mix, 0.0f, 1.0f, "%.2f")) {
            chorus->setMix(mix);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Mix");
        
        if (anyChanged && chorus->getCurrentPreset() != Chorus::Preset::Custom) {
            chorus->setCurrentPreset(Chorus::Preset::Custom);
        }
    } else if (distortion) {
        // Distortion preset selector with arrow dropdown
        ImGui::SetNextItemWidth(140);
        const char* currentPresetName = Distortion::getPresetName(distortion->getCurrentPreset());
        if (ImGui::BeginCombo("Preset##dist", currentPresetName)) {
            for (int i = 0; i < 6; ++i) {
                Distortion::Preset preset = static_cast<Distortion::Preset>(i);
                const char* presetName = Distortion::getPresetName(preset);
                if (ImGui::Selectable(presetName, distortion->getCurrentPreset() == preset)) {
                    distortion->loadPreset(preset);
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        
        bool anyChanged = false;
        
        ImGui::SetNextItemWidth(120);
        float drive = distortion->getDrive();
        if (ImGui::SliderFloat("##drive", &drive, 1.0f, 100.0f, "%.0f")) {
            distortion->setDrive(drive);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Drive");
        
        ImGui::SetNextItemWidth(120);
        float tone = distortion->getTone();
        if (ImGui::SliderFloat("##tone", &tone, 0.0f, 1.0f, "%.2f")) {
            distortion->setTone(tone);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Tone");
        
        ImGui::SetNextItemWidth(120);
        float mix = distortion->getMix();
        if (ImGui::SliderFloat("##distmix", &mix, 0.0f, 1.0f, "%.2f")) {
            distortion->setMix(mix);
            anyChanged = true;
            markDirty();
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Mix");
        
        if (anyChanged && distortion->getCurrentPreset() != Distortion::Preset::Custom) {
            distortion->setCurrentPreset(Distortion::Preset::Custom);
        }
    } else if (eq8) {
        // EQ8 preset selector
        ImGui::SetNextItemWidth(140);
        const char* currentPresetName = EQ8::getPresetName(eq8->getCurrentPreset());
        if (ImGui::BeginCombo("Preset##eq8", currentPresetName)) {
            for (int i = 0; i < 7; ++i) {
                EQ8::Preset preset = static_cast<EQ8::Preset>(i);
                const char* presetName = EQ8::getPresetName(preset);
                if (ImGui::Selectable(presetName, eq8->getCurrentPreset() == preset)) {
                    eq8->loadPreset(preset);
                    markDirty();
                }
            }
            ImGui::EndCombo();
        }
        
        // Frequency response visualization
        ImVec2 graphPos = ImGui::GetCursorScreenPos();
        float graphWidth = boxWidth - 16;
        float graphHeight = 60.0f;
        
        // Graph background
        drawList->AddRectFilled(graphPos, 
                               ImVec2(graphPos.x + graphWidth, graphPos.y + graphHeight),
                               IM_COL32(20, 20, 22, 255), 4.0f);
        drawList->AddRect(graphPos, 
                         ImVec2(graphPos.x + graphWidth, graphPos.y + graphHeight),
                         IM_COL32(50, 50, 55, 255), 4.0f);
        
        // Draw 0dB line
        float centerY = graphPos.y + graphHeight / 2.0f;
        drawList->AddLine(ImVec2(graphPos.x, centerY), 
                         ImVec2(graphPos.x + graphWidth, centerY),
                         IM_COL32(50, 50, 60, 255));
        
        // Draw frequency markers
        const float freqMarkers[] = {100.0f, 1000.0f, 10000.0f};
        for (float freq : freqMarkers) {
            float logFreq = std::log10(freq / 20.0f) / std::log10(20000.0f / 20.0f);
            float x = graphPos.x + logFreq * graphWidth;
            drawList->AddLine(ImVec2(x, graphPos.y), ImVec2(x, graphPos.y + graphHeight),
                             IM_COL32(40, 40, 45, 255));
        }
        
        // Draw EQ curve
        ImVec2 prevPoint;
        for (int px = 0; px < static_cast<int>(graphWidth); px++) {
            float logX = static_cast<float>(px) / graphWidth;
            float freq = 20.0f * std::pow(20000.0f / 20.0f, logX);
            
            // Approximate combined gain at this frequency
            float totalGain = 0.0f;
            for (int b = 0; b < EQ8::NUM_BANDS; b++) {
                const auto& band = eq8->getBand(b);
                if (!band.enabled) continue;
                
                float bandGain = band.gain;
                float bandFreq = band.frequency;
                float q = band.q;
                
                // Simple bell curve approximation for visualization
                float octaveDist = std::log2(freq / bandFreq);
                float falloff = std::exp(-octaveDist * octaveDist * q);
                totalGain += bandGain * falloff;
            }
            
            // Map gain to Y (-24dB to +24dB)
            totalGain = std::clamp(totalGain, -24.0f, 24.0f);
            float y = centerY - (totalGain / 24.0f) * (graphHeight / 2.0f - 4.0f);
            
            ImVec2 point(graphPos.x + px, y);
            if (px > 0) {
                drawList->AddLine(prevPoint, point, IM_COL32(255, 149, 0, 255), 2.0f);
            }
            prevPoint = point;
        }
        
        // Draw band markers
        for (int b = 0; b < EQ8::NUM_BANDS; b++) {
            const auto& band = eq8->getBand(b);
            if (!band.enabled) continue;
            
            float logFreq = std::log10(band.frequency / 20.0f) / std::log10(20000.0f / 20.0f);
            float x = graphPos.x + logFreq * graphWidth;
            float gain = std::clamp(band.gain, -24.0f, 24.0f);
            float y = centerY - (gain / 24.0f) * (graphHeight / 2.0f - 4.0f);
            
            // Band dot
            drawList->AddCircleFilled(ImVec2(x, y), 5.0f, IM_COL32(230, 160, 60, 255));
            drawList->AddCircle(ImVec2(x, y), 5.0f, IM_COL32(255, 200, 100, 255), 0, 1.5f);
            
            // Band number
            char label[4];
            snprintf(label, sizeof(label), "%d", b + 1);
            drawList->AddText(ImVec2(x - 3, y - 14), IM_COL32(180, 180, 180, 255), label);
        }
        
        ImGui::Dummy(ImVec2(graphWidth, graphHeight + 4));
        
        // Compact band controls - show 2 selected bands at a time
        static int selectedBand = 0;
        
        // Band selector buttons
        ImGui::SetNextItemWidth(graphWidth);
        for (int b = 0; b < 8; b++) {
            bool isActive = (selectedBand == b);
            bool bandEnabled = eq8->getBand(b).enabled;
            
            ImGui::PushID(b);
            if (isActive) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.55f, 0.75f, 1.0f));
            } else if (!bandEnabled) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.22f, 1.0f));
            }
            
            char btnLabel[4];
            snprintf(btnLabel, sizeof(btnLabel), "%d", b + 1);
            if (ImGui::Button(btnLabel, ImVec2(18, 18))) {
                selectedBand = b;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
            
            if (b < 7) ImGui::SameLine(0, 2);
        }
        
        // Selected band controls
        auto& band = eq8->getBand(selectedBand);
        bool anyChanged = false;
        
        ImGui::Checkbox("##enable", &band.enabled);
        if (ImGui::IsItemEdited()) {
            eq8->updateCoefficients(selectedBand);
            anyChanged = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "Band %d", selectedBand + 1);
        
        // Frequency
        ImGui::SetNextItemWidth(100);
        float freq = band.frequency;
        if (ImGui::SliderFloat("##freq", &freq, 20.0f, 20000.0f, "%.0f Hz", ImGuiSliderFlags_Logarithmic)) {
            band.frequency = freq;
            eq8->updateCoefficients(selectedBand);
            anyChanged = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Freq");
        
        // Gain
        ImGui::SetNextItemWidth(100);
        float gain = band.gain;
        if (ImGui::SliderFloat("##gain", &gain, -24.0f, 24.0f, "%.1f dB")) {
            band.gain = gain;
            eq8->updateCoefficients(selectedBand);
            anyChanged = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Gain");
        
        // Q
        ImGui::SetNextItemWidth(100);
        float q = band.q;
        if (ImGui::SliderFloat("##q", &q, 0.1f, 18.0f, "%.1f Q")) {
            band.q = q;
            eq8->updateCoefficients(selectedBand);
            anyChanged = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Q");
        
        if (anyChanged) {
            eq8->setCurrentPreset(EQ8::Preset::Custom);
            markDirty();
        }
    } else if (pump) {
        ImGui::SetNextItemWidth(120);
        float rate = pump->getRateHz();
        if (ImGui::SliderFloat("Rate (Hz)", &rate, 0.1f, 8.0f, "%.2f")) { pump->setRateHz(rate); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float depth = pump->getDepthDb();
        if (ImGui::SliderFloat("Depth (dB)", &depth, -48.0f, 0.0f, "%.1f")) { pump->setDepth(depth); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float shape = pump->getShape();
        if (ImGui::SliderFloat("Shape", &shape, 0.2f, 3.0f, "%.2f")) { pump->setShape(shape); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float atk = pump->getAttackMs();
        if (ImGui::SliderFloat("Attack (ms)", &atk, 1.0f, 400.0f, "%.0f")) { pump->setAttackMs(atk); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float rel = pump->getReleaseMs();
        if (ImGui::SliderFloat("Release (ms)", &rel, 10.0f, 800.0f, "%.0f")) { pump->setReleaseMs(rel); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float mix = pump->getMix();
        if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) { pump->setMix(mix); markDirty(); }
    } else if (wow) {
        // Unique tape-machine aesthetic for WowFlutter
        ImVec2 contentPos = ImGui::GetCursorScreenPos();
        float contentWidth = ImGui::GetContentRegionAvail().x;
        
        // Tape reel visualization area - two spinning reels
        float reelAreaHeight = 70.0f;
        float reelRadius = 28.0f;
        float reelCenterY = contentPos.y + reelAreaHeight / 2.0f;
        float leftReelX = contentPos.x + contentWidth * 0.28f;
        float rightReelX = contentPos.x + contentWidth * 0.72f;
        
        // Background for tape deck section
        drawList->AddRectFilled(ImVec2(contentPos.x + 4, contentPos.y),
                               ImVec2(contentPos.x + contentWidth - 4, contentPos.y + reelAreaHeight),
                               IM_COL32(25, 22, 20, 255), 4.0f);
        
        // Animated rotation based on mix amount (simulates tape motion)
        static float tapeRotation = 0.0f;
        tapeRotation += wow->getMix() * 0.08f;
        if (tapeRotation > 6.28f) tapeRotation -= 6.28f;
        
        // Left reel (supply)
        drawList->AddCircleFilled(ImVec2(leftReelX, reelCenterY), reelRadius, IM_COL32(50, 45, 40, 255));
        drawList->AddCircle(ImVec2(leftReelX, reelCenterY), reelRadius, IM_COL32(90, 80, 70, 255), 0, 2.0f);
        drawList->AddCircleFilled(ImVec2(leftReelX, reelCenterY), 8.0f, IM_COL32(70, 65, 60, 255));
        // Reel spokes
        for (int spoke = 0; spoke < 3; ++spoke) {
            float angle = tapeRotation + spoke * 2.09f;
            float sx = leftReelX + cosf(angle) * 18.0f;
            float sy = reelCenterY + sinf(angle) * 18.0f;
            drawList->AddLine(ImVec2(leftReelX, reelCenterY), ImVec2(sx, sy), IM_COL32(80, 75, 65, 255), 3.0f);
        }
        
        // Right reel (take-up)
        drawList->AddCircleFilled(ImVec2(rightReelX, reelCenterY), reelRadius, IM_COL32(50, 45, 40, 255));
        drawList->AddCircle(ImVec2(rightReelX, reelCenterY), reelRadius, IM_COL32(90, 80, 70, 255), 0, 2.0f);
        drawList->AddCircleFilled(ImVec2(rightReelX, reelCenterY), 8.0f, IM_COL32(70, 65, 60, 255));
        for (int spoke = 0; spoke < 3; ++spoke) {
            float angle = -tapeRotation + spoke * 2.09f;
            float sx = rightReelX + cosf(angle) * 18.0f;
            float sy = reelCenterY + sinf(angle) * 18.0f;
            drawList->AddLine(ImVec2(rightReelX, reelCenterY), ImVec2(sx, sy), IM_COL32(80, 75, 65, 255), 3.0f);
        }
        
        // Tape path between reels - horizontal line at center level with wobble
        float tapeY = reelCenterY;  // Tape runs through the center of the reels
        float tapeStartX = leftReelX + reelRadius + 2.0f;   // Start at right edge of left reel
        float tapeEndX = rightReelX - reelRadius - 2.0f;     // End at left edge of right reel
        float wowDepth = wow->getWowDepthMs();
        
        // Draw tape as a thicker line with wobble effect
        for (float x = tapeStartX; x < tapeEndX; x += 2.0f) {
            float wobble = sinf((x - tapeStartX) * 0.15f + tapeRotation * 3.0f) * wowDepth * 0.8f;
            float flutter = sinf((x - tapeStartX) * 0.6f + tapeRotation * 12.0f) * wow->getFlutterDepthMs() * 1.5f;
            float y = tapeY + wobble + flutter;
            drawList->AddRectFilled(ImVec2(x, y - 1.5f), ImVec2(x + 2.0f, y + 1.5f), IM_COL32(90, 60, 45, 255));
        }
        
        // "TAPE" label
        ImVec2 tapeLabel = ImGui::CalcTextSize("TAPE");
        drawList->AddText(ImVec2(contentPos.x + (contentWidth - tapeLabel.x) / 2.0f, contentPos.y + 3.0f),
                         IM_COL32(120, 100, 80, 255), "TAPE");
        
        ImGui::Dummy(ImVec2(0, reelAreaHeight + 4.0f));
        
        // Control section with vintage style
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.11f, 0.10f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.6f, 0.45f, 0.3f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0.7f, 0.55f, 0.4f, 1.0f));
        
        ImGui::TextColored(ImVec4(0.7f, 0.6f, 0.5f, 1.0f), "WOW");
        ImGui::SetNextItemWidth(contentWidth - 16.0f);
        float wr = wow->getWowRate();
        if (ImGui::SliderFloat("##WowRate", &wr, 0.05f, 2.0f, "Rate: %.2f Hz")) { wow->setWowRate(wr); markDirty(); }
        ImGui::SetNextItemWidth(contentWidth - 16.0f);
        float wd = wow->getWowDepthMs();
        if (ImGui::SliderFloat("##WowDepth", &wd, 0.1f, 6.0f, "Depth: %.1f ms")) { wow->setWowDepthMs(wd); markDirty(); }
        
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f, 0.6f, 0.5f, 1.0f), "FLUTTER");
        ImGui::SetNextItemWidth(contentWidth - 16.0f);
        float fr = wow->getFlutterRate();
        if (ImGui::SliderFloat("##FlutterRate", &fr, 3.0f, 12.0f, "Rate: %.1f Hz")) { wow->setFlutterRate(fr); markDirty(); }
        ImGui::SetNextItemWidth(contentWidth - 16.0f);
        float fd = wow->getFlutterDepthMs();
        if (ImGui::SliderFloat("##FlutterDepth", &fd, 0.05f, 1.5f, "Depth: %.2f ms")) { wow->setFlutterDepthMs(fd); markDirty(); }
        
        ImGui::Spacing();
        ImGui::SetNextItemWidth(contentWidth - 16.0f);
        float sat = wow->getSaturation();
        if (ImGui::SliderFloat("##Sat", &sat, 0.0f, 1.0f, "Saturation: %.2f")) { wow->setSaturation(sat); markDirty(); }
        ImGui::SetNextItemWidth(contentWidth - 16.0f);
        float mix = wow->getMix();
        if (ImGui::SliderFloat("##Mix", &mix, 0.0f, 1.0f, "Mix: %.2f")) { wow->setMix(mix); markDirty(); }
        
        ImGui::PopStyleColor(3);
    } else if (beatRepeat) {
        ImGui::SetNextItemWidth(120);
        float interval = beatRepeat->getIntervalMs();
        if (ImGui::SliderFloat("Interval (ms)", &interval, 50.0f, 2000.0f, "%.0f")) { beatRepeat->setIntervalMs(interval); beatRepeat->reset(); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float gate = beatRepeat->getGateMs();
        if (ImGui::SliderFloat("Gate (ms)", &gate, 40.0f, 800.0f, "%.0f")) { beatRepeat->setGateMs(gate); beatRepeat->reset(); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float chance = beatRepeat->getChance();
        if (ImGui::SliderFloat("Chance", &chance, 0.0f, 1.0f, "%.2f")) { beatRepeat->setChance(chance); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float decay = beatRepeat->getDecay();
        if (ImGui::SliderFloat("Decay", &decay, 0.1f, 1.0f, "%.2f")) { beatRepeat->setDecay(decay); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float filt = beatRepeat->getFilter();
        if (ImGui::SliderFloat("Filter", &filt, 0.0f, 1.0f, "%.2f")) { beatRepeat->setFilter(filt); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float mix = beatRepeat->getMix();
        if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) { beatRepeat->setMix(mix); markDirty(); }
    } else if (bitNoise) {
        ImGui::SetNextItemWidth(120);
        int bits = bitNoise->getBits();
        if (ImGui::SliderInt("Bits", &bits, 4, 16)) { bitNoise->setBits(bits); markDirty(); }
        ImGui::SetNextItemWidth(120);
        int ds = bitNoise->getDownsample();
        if (ImGui::SliderInt("Downsample", &ds, 1, 16)) { bitNoise->setDownsample(ds); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float noise = bitNoise->getNoise();
        if (ImGui::SliderFloat("Noise", &noise, 0.0f, 0.5f, "%.2f")) { bitNoise->setNoise(noise); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float tilt = bitNoise->getTilt();
        if (ImGui::SliderFloat("Tilt", &tilt, -1.0f, 1.0f, "%.2f")) { bitNoise->setTilt(tilt); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float mix = bitNoise->getMix();
        if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) { bitNoise->setMix(mix); markDirty(); }
    } else if (resonator) {
        ImGui::SetNextItemWidth(120);
        float root = resonator->getRootHz();
        if (ImGui::SliderFloat("Root Hz", &root, 40.0f, 2000.0f, "%.0f")) { resonator->setRootHz(root); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float spread = resonator->getSpread();
        if (ImGui::SliderFloat("Spread (semi)", &spread, -12.0f, 24.0f, "%.1f")) { resonator->setSpread(spread); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float decay = resonator->getDecay();
        if (ImGui::SliderFloat("Decay", &decay, 0.1f, 0.999f, "%.3f")) { resonator->setDecay(decay); markDirty(); }
        ImGui::SetNextItemWidth(120);
        float mix = resonator->getMix();
        if (ImGui::SliderFloat("Mix", &mix, 0.0f, 1.0f, "%.2f")) { resonator->setMix(mix); markDirty(); }
    }
    
    ImGui::PopStyleColor(4);
    ImGui::EndChild();
    
    // Allow dropping a new preset to REPLACE this effect
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("EFFECT_PRESET")) {
            if (payload->DataSize == sizeof(int) * 2) {
                int* data = (int*)payload->Data;
                int effectType = data[0];
                int presetIdx = data[1];
                double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                std::shared_ptr<Effect> newEffect;
                
                switch (effectType) {
                    case 0: {
                        auto rev = std::make_shared<Reverb>(sampleRate);
                        rev->loadPreset(static_cast<Reverb::Preset>(presetIdx));
                        newEffect = rev;
                        break;
                    }
                    case 1: {
                        auto ch = std::make_shared<Chorus>(sampleRate);
                        ch->loadPreset(static_cast<Chorus::Preset>(presetIdx));
                        newEffect = ch;
                        break;
                    }
                    case 2: {
                        auto dist = std::make_shared<Distortion>(sampleRate);
                        dist->loadPreset(static_cast<Distortion::Preset>(presetIdx));
                        newEffect = dist;
                        break;
                    }
                    case 3: {
                        auto eq = std::make_shared<EQ8>(sampleRate);
                        eq->loadPreset(static_cast<EQ8::Preset>(presetIdx));
                        newEffect = eq;
                        break;
                    }
                    case 4: {
                        auto pump = std::make_shared<SidechainPump>(sampleRate);
                        if (presetIdx == 0) { pump->setDepth(-6.0f); pump->setAttackMs(20.0f); pump->setReleaseMs(150.0f); }
                        else if (presetIdx == 1) { pump->setDepth(-12.0f); pump->setAttackMs(10.0f); pump->setReleaseMs(200.0f); }
                        else if (presetIdx == 2) { pump->setDepth(-24.0f); pump->setAttackMs(5.0f); pump->setReleaseMs(250.0f); }
                        else { pump->setDepth(-36.0f); pump->setAttackMs(2.0f); pump->setReleaseMs(300.0f); }
                        newEffect = pump;
                        break;
                    }
                    case 5: {
                        auto wf = std::make_shared<WowFlutter>(sampleRate);
                        if (presetIdx == 0) { wf->setWowDepthMs(1.0f); wf->setFlutterDepthMs(0.2f); }
                        else if (presetIdx == 1) { wf->setWowDepthMs(2.5f); wf->setFlutterDepthMs(0.4f); }
                        else if (presetIdx == 2) { wf->setWowDepthMs(4.0f); wf->setFlutterDepthMs(0.8f); }
                        else { wf->setWowDepthMs(5.5f); wf->setFlutterDepthMs(1.2f); }
                        newEffect = wf;
                        break;
                    }
                    case 6: {
                        auto br = std::make_shared<BeatRepeat>(sampleRate);
                        if (presetIdx == 0) br->setIntervalMs(250.0f);
                        else if (presetIdx == 1) br->setIntervalMs(125.0f);
                        else if (presetIdx == 2) br->setIntervalMs(167.0f);
                        else br->setIntervalMs(100.0f);
                        newEffect = br;
                        break;
                    }
                    case 7: {
                        auto bit = std::make_shared<BitNoiseTexture>(sampleRate);
                        if (presetIdx == 0) { bit->setBits(12); bit->setNoise(0.02f); }
                        else if (presetIdx == 1) { bit->setBits(8); bit->setNoise(0.05f); }
                        else if (presetIdx == 2) { bit->setBits(6); bit->setNoise(0.08f); }
                        else { bit->setBits(4); bit->setNoise(0.15f); }
                        newEffect = bit;
                        break;
                    }
                    case 8: {
                        auto res = std::make_shared<ResonatorBank>(sampleRate);
                        if (presetIdx == 0) { res->setRootHz(220.0f); res->setSpread(12.0f); }
                        else if (presetIdx == 1) { res->setRootHz(110.0f); res->setSpread(7.0f); }
                        else if (presetIdx == 2) { res->setRootHz(440.0f); res->setSpread(5.0f); }
                        else { res->setRootHz(330.0f); res->setSpread(3.0f); }
                        newEffect = res;
                        break;
                    }
                }
                
                if (newEffect && effectIndex < tracks_[trackIndex].effects.size()) {
                    // REPLACE the effect at this index
                    tracks_[trackIndex].effects[effectIndex] = newEffect;
                    markDirty();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
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
    
    // Ableton-inspired track color palette (24 colors in HSL gradient, starting with ORANGE)
    static const ImU32 trackColors[24] = {
        // Row 1: Primary saturated colors (HSL gradient)
        IM_COL32(255, 149, 0, 255),   // 0: Orange (playhead color) - START HERE
        IM_COL32(255, 94, 94, 255),   // 1: Red
        IM_COL32(255, 94, 153, 255),  // 2: Rose
        IM_COL32(219, 103, 186, 255), // 3: Pink
        IM_COL32(179, 102, 219, 255), // 4: Magenta
        IM_COL32(138, 103, 219, 255), // 5: Purple
        IM_COL32(102, 128, 219, 255), // 6: Indigo
        IM_COL32(51, 153, 219, 255),  // 7: Blue
        IM_COL32(51, 179, 204, 255),  // 8: Cyan
        IM_COL32(0, 199, 140, 255),   // 9: Teal
        IM_COL32(79, 199, 102, 255),  // 10: Green
        IM_COL32(132, 214, 79, 255),  // 11: Lime
        // Row 2: Lighter/pastel variants
        IM_COL32(255, 179, 102, 255), // 12: Peach
        IM_COL32(255, 128, 128, 255), // 13: Light red
        IM_COL32(255, 153, 187, 255), // 14: Light rose
        IM_COL32(230, 153, 204, 255), // 15: Light pink
        IM_COL32(204, 153, 230, 255), // 16: Light magenta
        IM_COL32(179, 153, 230, 255), // 17: Light purple
        IM_COL32(153, 170, 230, 255), // 18: Light indigo
        IM_COL32(128, 191, 230, 255), // 19: Light blue
        IM_COL32(128, 217, 217, 255), // 20: Light cyan
        IM_COL32(102, 217, 191, 255), // 21: Light teal
        IM_COL32(153, 230, 153, 255), // 22: Light green
        IM_COL32(255, 230, 102, 255)  // 23: Light yellow
    };
    
    // Get window info for drawing
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImVec2 windowSize = ImGui::GetWindowSize();
    
    // Dark background
    drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y),
                           IM_COL32(18, 18, 18, 255));
    
    // Split into two columns: left for track controls, right for timeline
    ImGui::Columns(2, "track_columns", false);
    ImGui::SetColumnWidth(0, 280.0f);  // Narrower track header (Ableton-style)
    
    // Render all tracks
    for (size_t i = 0; i < tracks_.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        
        // Assign track color if not set (start with orange = index 0, cycle through 24 colors)
        if (tracks_[i].colorIndex == 0 && i > 0) {
            tracks_[i].colorIndex = static_cast<int>(i % 24);
        }
        
        float headerHeight = 54.0f;  // Ableton-style compact
        float columnWidth = ImGui::GetColumnWidth(0);
        ImVec2 headerStartPos = ImGui::GetCursorScreenPos();
        
        bool isSelected = (i == selectedTrackIndex_);
        ImVec2 mousePos = ImGui::GetMousePos();
        
        // Track header background - dark grey (reverted to match color scheme)
        ImU32 trackBg = isSelected ? IM_COL32(50, 50, 50, 255) : IM_COL32(35, 35, 35, 255);
        drawList->AddRectFilled(headerStartPos, 
            ImVec2(headerStartPos.x + columnWidth, headerStartPos.y + headerHeight),
                               trackBg);
        
        // Track color indicator - small circle on left like Ableton
        ImU32 trackColor = trackColors[tracks_[i].colorIndex % 24];
        float circleRadius = 5.0f;
        float circleX = headerStartPos.x + 10.0f;
        float circleY = headerStartPos.y + 14.0f;
        drawList->AddCircleFilled(ImVec2(circleX, circleY), circleRadius, trackColor);
        
        // Border separators
        drawList->AddLine(ImVec2(headerStartPos.x, headerStartPos.y),
                         ImVec2(headerStartPos.x + columnWidth, headerStartPos.y),
                         IM_COL32(25, 25, 25, 255));
        drawList->AddLine(ImVec2(headerStartPos.x + columnWidth - 1, headerStartPos.y),
                         ImVec2(headerStartPos.x + columnWidth - 1, headerStartPos.y + headerHeight),
                         IM_COL32(25, 25, 25, 255));
        
        // Selection highlight - subtle blue glow
        if (isSelected) {
            drawList->AddRect(headerStartPos,
                             ImVec2(headerStartPos.x + columnWidth, headerStartPos.y + headerHeight),
                             IM_COL32(255, 149, 0, 255), 0.0f, 0, 2.0f);
        }
        
        // Layout: Track name row, then I/O-like row, then buttons/controls row
        float leftPadding = 22.0f;  // After circle
        float contentStartX = headerStartPos.x + leftPadding;
        float meterWidth = 10.0f;
        float contentEndX = headerStartPos.x + columnWidth - meterWidth - 4.0f;
        float contentWidth = contentEndX - contentStartX;
        
        // Row 1: Track name (top row, simpler)
        char trackLabel[64];
        std::string displayLabel;
        if (!tracks_[i].name.empty()) {
            displayLabel = tracks_[i].name;
        } else if (!tracks_[i].instrumentName.empty()) {
            if (tracks_[i].hasSampler && tracks_[i].instrumentName.find("Sampler:") != std::string::npos) {
                displayLabel = std::filesystem::path(tracks_[i].samplerSamplePath).stem().string();
            } else {
                displayLabel = tracks_[i].instrumentName;
            }
        } else {
            displayLabel = "Track " + std::to_string(i + 1);
        }
        
        // Truncate label if too long
        float maxLabelWidth = contentWidth - 30.0f;
        ImVec2 labelSize = ImGui::CalcTextSize(displayLabel.c_str());
        if (labelSize.x > maxLabelWidth && displayLabel.length() > 8) {
            while (labelSize.x > maxLabelWidth && displayLabel.length() > 8) {
                displayLabel = displayLabel.substr(0, displayLabel.length() - 4) + "...";
                labelSize = ImGui::CalcTextSize(displayLabel.c_str());
            }
        }
        snprintf(trackLabel, sizeof(trackLabel), "%s", displayLabel.c_str());
        
        float labelX = contentStartX;
        float labelY = headerStartPos.y + 6.0f;
        ImVec2 labelMin(labelX - 2.0f, labelY - 1.0f);
        ImVec2 labelMax(labelX + labelSize.x + 2.0f, labelY + labelSize.y + 1.0f);
        
        bool labelHovered = (mousePos.x >= labelMin.x && mousePos.x <= labelMax.x &&
                            mousePos.y >= labelMin.y && mousePos.y <= labelMax.y);
        
        // Renaming
        if (renamingTrackIndex_ == static_cast<int>(i)) {
            ImGui::SetCursorScreenPos(ImVec2(contentStartX, labelY - 2.0f));
            ImGui::PushItemWidth(std::min(contentWidth, 140.0f));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            
            char inputId[32];
            snprintf(inputId, sizeof(inputId), "##rename_%zu", i);
            
            if (ImGui::InputText(inputId, trackRenameBuffer_, sizeof(trackRenameBuffer_), 
                                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                tracks_[i].name = trackRenameBuffer_;
                renamingTrackIndex_ = -1;
                markDirty();
            }
            
            if (ImGui::IsItemVisible() && !ImGui::IsItemActive()) {
                ImGui::SetKeyboardFocusHere(-1);
            }
            
            if (ImGui::IsKeyPressed(ImGuiKey_Escape) || 
                (ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered())) {
                renamingTrackIndex_ = -1;
            }
            
            ImGui::PopStyleColor(2);
            ImGui::PopItemWidth();
        } else {
            if (labelHovered) {
                drawList->AddRectFilled(labelMin, labelMax, IM_COL32(60, 60, 60, 150), 2.0f);
            }
            drawList->AddText(ImVec2(labelX, labelY), IM_COL32(200, 200, 200, 255), trackLabel);
        
            if (labelHovered && ImGui::IsMouseDoubleClicked(0)) {
                renamingTrackIndex_ = static_cast<int>(i);
                strncpy(trackRenameBuffer_, trackLabel, sizeof(trackRenameBuffer_) - 1);
                trackRenameBuffer_[sizeof(trackRenameBuffer_) - 1] = '\0';
            }
        }
        
        // Row 2: I/O style indicators (subtle)
        float ioRowY = headerStartPos.y + 20.0f;
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 0 ? ImGui::GetIO().Fonts->Fonts[0] : nullptr);
        drawList->AddText(ImVec2(contentStartX, ioRowY), IM_COL32(100, 100, 100, 255), 
                         tracks_[i].hasSampler ? "Smplr" : "Synth");
        ImGui::PopFont();
        
        // Row 3: S M ● buttons, then Vol/Pan displays
        static int editingVol = -1;
        static int editingPan = -1;
        static char volBuf[32] = {0};
        static char panBuf[32] = {0};
        
        float buttonRowY = headerStartPos.y + 36.0f;
        float buttonX = contentStartX;
        float smallBtnSize = 16.0f;
        float smallBtnSpacing = 1.0f;
        
        // Solo button (S) - yellow when active
        ImVec2 soloMin(buttonX, buttonRowY);
        ImVec2 soloMax(buttonX + smallBtnSize, buttonRowY + smallBtnSize);
        bool soloHovered = (mousePos.x >= soloMin.x && mousePos.x <= soloMax.x &&
                           mousePos.y >= soloMin.y && mousePos.y <= soloMax.y);
        
        ImU32 soloBg = tracks_[i].isSolo ? IM_COL32(200, 165, 50, 255) : 
                       (soloHovered ? IM_COL32(55, 55, 55, 255) : IM_COL32(40, 40, 40, 255));
        drawList->AddRectFilled(soloMin, soloMax, soloBg, 2.0f);
        ImVec2 sTxt = ImGui::CalcTextSize("S");
        drawList->AddText(ImVec2(buttonX + (smallBtnSize - sTxt.x) / 2, buttonRowY + (smallBtnSize - sTxt.y) / 2),
                         tracks_[i].isSolo ? IM_COL32(0, 0, 0, 255) : IM_COL32(150, 150, 150, 255), "S");
        
        if (soloHovered && ImGui::IsMouseClicked(0)) {
            tracks_[i].isSolo = !tracks_[i].isSolo;
        }
        buttonX += smallBtnSize + smallBtnSpacing;
        
        // Mute button - orange circle when active
        ImVec2 muteMin(buttonX, buttonRowY);
        ImVec2 muteMax(buttonX + smallBtnSize, buttonRowY + smallBtnSize);
        bool muteHovered = (mousePos.x >= muteMin.x && mousePos.x <= muteMax.x &&
                           mousePos.y >= muteMin.y && mousePos.y <= muteMax.y);
        
        ImU32 muteBg = muteHovered ? IM_COL32(55, 55, 55, 255) : IM_COL32(40, 40, 40, 255);
        drawList->AddRectFilled(muteMin, muteMax, muteBg, 2.0f);
        ImVec2 muteCenter(buttonX + smallBtnSize / 2, buttonRowY + smallBtnSize / 2);
        ImU32 muteColor = tracks_[i].isMuted ? IM_COL32(255, 149, 0, 255) : IM_COL32(80, 80, 80, 255);
        drawList->AddCircleFilled(muteCenter, 5.0f, muteColor);
        
        if (muteHovered && ImGui::IsMouseClicked(0)) {
            tracks_[i].isMuted = !tracks_[i].isMuted;
        }
        buttonX += smallBtnSize + smallBtnSpacing;
        
        // Arm/Record button - red when active
        ImVec2 armMin(buttonX, buttonRowY);
        ImVec2 armMax(buttonX + smallBtnSize, buttonRowY + smallBtnSize);
        bool armHovered = (mousePos.x >= armMin.x && mousePos.x <= armMax.x &&
                          mousePos.y >= armMin.y && mousePos.y <= armMax.y);
        
        ImU32 armBg = armHovered ? IM_COL32(55, 55, 55, 255) : IM_COL32(40, 40, 40, 255);
        drawList->AddRectFilled(armMin, armMax, armBg, 2.0f);
        ImVec2 armCenter(buttonX + smallBtnSize / 2, buttonRowY + smallBtnSize / 2);
        ImU32 armColor = tracks_[i].isRecording ? IM_COL32(255, 94, 94, 255) : IM_COL32(80, 80, 80, 255);
        drawList->AddCircleFilled(armCenter, 4.0f, armColor);
        
        if (armHovered && ImGui::IsMouseClicked(0)) {
            tracks_[i].isRecording = !tracks_[i].isRecording;
        }
        buttonX += smallBtnSize + 4.0f;
        
        // Volume display: numeric box with optional fill
        float volW = 38.0f;
        float volH = smallBtnSize;
        ImVec2 volMin(buttonX, buttonRowY);
        ImVec2 volMax(buttonX + volW, buttonRowY + volH);
        
        // Background - only show fill if volume is NOT at 0dB
        drawList->AddRectFilled(volMin, volMax, IM_COL32(25, 25, 25, 255), 2.0f);
        if (std::abs(tracks_[i].volumeDb) > 0.1f) {
            // Show green bar when adjusted from default
            float volNorm = std::clamp((tracks_[i].volumeDb + 60.0f) / 72.0f, 0.0f, 1.0f);
            float zeroNorm = 60.0f / 72.0f;  // Where 0dB is
            if (tracks_[i].volumeDb > 0.0f) {
                // Above 0: fill from 0 to current
                drawList->AddRectFilled(ImVec2(volMin.x + volW * zeroNorm, volMin.y + 2),
                                       ImVec2(volMin.x + volW * volNorm, volMax.y - 2),
                                       IM_COL32(120, 180, 90, 200));
            } else {
                // Below 0: fill from current to 0
                drawList->AddRectFilled(ImVec2(volMin.x + volW * volNorm, volMin.y + 2),
                                       ImVec2(volMin.x + volW * zeroNorm, volMax.y - 2),
                                       IM_COL32(100, 140, 80, 160));
            }
        }
        drawList->AddRect(volMin, volMax, IM_COL32(60, 60, 60, 255), 2.0f);
        
        // Vol text - show -inf for very low, otherwise dB
        char volText[16]; 
        if (tracks_[i].volumeDb <= -59.0f) {
            snprintf(volText, sizeof(volText), "-inf");
        } else {
            snprintf(volText, sizeof(volText), "%.0f", tracks_[i].volumeDb);
        }
        ImVec2 vtSz = ImGui::CalcTextSize(volText);
        drawList->AddText(ImVec2(volMin.x + (volW - vtSz.x) / 2, volMin.y + (volH - vtSz.y) / 2), 
                         IM_COL32(200, 200, 200, 255), volText);
        
        ImGui::SetCursorScreenPos(volMin);
        char volId[32]; snprintf(volId, sizeof(volId), "##vol_%zu", i);
        ImGui::InvisibleButton(volId, ImVec2(volW, volH));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            float delta = -ImGui::GetIO().MouseDelta.y * 0.5f;
            tracks_[i].volumeDb = std::clamp(tracks_[i].volumeDb + delta, -60.0f, 12.0f);
            markDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            editingVol = static_cast<int>(i);
            snprintf(volBuf, sizeof(volBuf), "%.1f", tracks_[i].volumeDb);
        }
        if (editingVol == static_cast<int>(i)) {
            ImGui::SetCursorScreenPos(volMin);
            ImGui::PushItemWidth(volW);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            if (ImGui::InputText("##volEdit", volBuf, sizeof(volBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                tracks_[i].volumeDb = std::clamp(static_cast<float>(std::atof(volBuf)), -60.0f, 12.0f);
                editingVol = -1;
                markDirty();
            }
            if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered()) editingVol = -1;
            ImGui::PopStyleColor();
            ImGui::PopItemWidth();
        }
        buttonX += volW + 2.0f;
        
        // Pan display: center-biased box
        float panW = 30.0f;
        ImVec2 panMin(buttonX, buttonRowY);
        ImVec2 panMax(buttonX + panW, buttonRowY + volH);
        
        drawList->AddRectFilled(panMin, panMax, IM_COL32(25, 25, 25, 255), 2.0f);
        float panCenterX = panMin.x + panW / 2.0f;
        
        // Only show pan fill if not centered
        if (std::abs(tracks_[i].pan) > 0.02f) {
            drawList->AddLine(ImVec2(panCenterX, panMin.y + 2), ImVec2(panCenterX, panMax.y - 2), IM_COL32(60, 60, 60, 255));
            if (tracks_[i].pan < 0.0f) {
                drawList->AddRectFilled(ImVec2(panCenterX + tracks_[i].pan * panW / 2, panMin.y + 2),
                                        ImVec2(panCenterX, panMax.y - 2), IM_COL32(100, 160, 220, 200), 2.0f);
            } else {
                drawList->AddRectFilled(ImVec2(panCenterX, panMin.y + 2),
                                        ImVec2(panCenterX + tracks_[i].pan * panW / 2, panMax.y - 2), IM_COL32(220, 160, 100, 200), 2.0f);
            }
        }
        drawList->AddRect(panMin, panMax, IM_COL32(60, 60, 60, 255), 2.0f);
        
        // Pan text - compact
        char panLbl[8];
        if (std::abs(tracks_[i].pan) < 0.02f) snprintf(panLbl, sizeof(panLbl), "C");
        else if (tracks_[i].pan < 0) snprintf(panLbl, sizeof(panLbl), "%dL", (int)(-tracks_[i].pan * 50));
        else snprintf(panLbl, sizeof(panLbl), "%dR", (int)(tracks_[i].pan * 50));
        ImVec2 ptSz = ImGui::CalcTextSize(panLbl);
        drawList->AddText(ImVec2(panMin.x + (panW - ptSz.x) / 2, panMin.y + (volH - ptSz.y) / 2),
                         IM_COL32(200, 200, 200, 255), panLbl);
        
        ImGui::SetCursorScreenPos(panMin);
        char panId[32]; snprintf(panId, sizeof(panId), "##pan_%zu", i);
        ImGui::InvisibleButton(panId, ImVec2(panW, volH));
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
            float delta = ImGui::GetIO().MouseDelta.x * 0.01f;
            tracks_[i].pan = std::clamp(tracks_[i].pan + delta, -1.0f, 1.0f);
            markDirty();
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
            editingPan = static_cast<int>(i);
            snprintf(panBuf, sizeof(panBuf), "%.2f", tracks_[i].pan);
        }
        if (editingPan == static_cast<int>(i)) {
            ImGui::SetCursorScreenPos(panMin);
            ImGui::PushItemWidth(panW);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            if (ImGui::InputText("##panEdit", panBuf, sizeof(panBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                tracks_[i].pan = std::clamp(static_cast<float>(std::atof(panBuf)), -1.0f, 1.0f);
                editingPan = -1;
                markDirty();
            }
            if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered()) editingPan = -1;
            ImGui::PopStyleColor();
            ImGui::PopItemWidth();
        }
        
        // Level meter (vertical, green segments)
        float meterHeight = headerHeight - 6.0f;
        float meterX = headerStartPos.x + columnWidth - meterWidth - 1.0f;
        float meterY = headerStartPos.y + 3.0f;
        meterWidth = 8.0f;
        
        // Meter background
        drawList->AddRectFilled(ImVec2(meterX, meterY), 
                               ImVec2(meterX + meterWidth, meterY + meterHeight),
                               IM_COL32(25, 25, 22, 255), 1.0f);
        
        // Get current level from waveform buffer
        float level = tracks_[i].peakLevel;
        
        // Update peak with decay
        auto now = std::chrono::steady_clock::now();
        double currentTime = std::chrono::duration<double>(now.time_since_epoch()).count();
        
        // Decay peak hold after 1 second
        if (currentTime - tracks_[i].peakHoldTime > 1.0) {
            tracks_[i].peakHold = std::max(0.0f, tracks_[i].peakHold - 0.02f);
        }
        
        if (level > tracks_[i].peakHold) {
            tracks_[i].peakHold = level;
            tracks_[i].peakHoldTime = currentTime;
        }
        
        // Draw segmented level fill (Ableton green)
        float levelHeight = level * meterHeight;
        if (levelHeight > 1.0f) {
            // Draw as segments
            int numSegments = (int)(levelHeight / 3.0f);
            for (int seg = 0; seg < numSegments; ++seg) {
                float segY = meterY + meterHeight - (seg + 1) * 3.0f;
                ImU32 segColor = IM_COL32(120, 200, 60, 255);  // Green
                float segNorm = (float)seg / (meterHeight / 3.0f);
                if (segNorm > 0.7f) segColor = IM_COL32(220, 180, 40, 255);  // Yellow
                if (segNorm > 0.9f) segColor = IM_COL32(220, 80, 60, 255);  // Red
                drawList->AddRectFilled(ImVec2(meterX + 1, segY),
                                       ImVec2(meterX + meterWidth - 1, segY + 2.0f),
                                       segColor);
            }
        }
        
        // Peak hold line
        if (tracks_[i].peakHold > 0.01f) {
            float peakY = meterY + meterHeight - (tracks_[i].peakHold * meterHeight);
            drawList->AddRectFilled(ImVec2(meterX + 1, peakY - 1), ImVec2(meterX + meterWidth - 1, peakY + 1),
                             IM_COL32(255, 255, 255, 220));
        }
        
        // Close button - small X in top right, blends with header
        bool closeClicked = false;
        if (tracks_.size() > 1) {
            float closeX = headerStartPos.x + columnWidth - meterWidth - 18.0f;
            float closeY = headerStartPos.y + 2.0f;
            float closeSize = 14.0f;
            
            ImVec2 closeMin(closeX, closeY);
            ImVec2 closeMax(closeX + closeSize, closeY + closeSize);
            bool closeHovered = (mousePos.x >= closeMin.x && mousePos.x <= closeMax.x &&
                                mousePos.y >= closeMin.y && mousePos.y <= closeMax.y);
            
            if (closeHovered) {
                drawList->AddRectFilled(closeMin, closeMax, IM_COL32(200, 80, 80, 180), 2.0f);
            }
            drawList->AddText(ImVec2(closeX + 3, closeY), 
                             closeHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(120, 120, 110, 255), "x");
            
            if (closeHovered && ImGui::IsMouseClicked(0)) {
                closeClicked = true;
            }
            
            if (closeClicked) {
                tracks_.erase(tracks_.begin() + i);
                if (selectedTrackIndex_ >= tracks_.size() && !tracks_.empty()) {
                    selectedTrackIndex_ = tracks_.size() - 1;
                }
                markDirty();
                ImGui::PopID();
                ImGui::NextColumn();
                ImGui::NextColumn();
                continue;
            }
        }
        
        // Reset cursor to header start and consume exact header height - avoids gaps
        ImGui::SetCursorScreenPos(headerStartPos);
        ImGui::Dummy(ImVec2(columnWidth, headerHeight));
        
        // Track selection on click (check we're not on any button)
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
            bool clickedButton = soloHovered || muteHovered || armHovered;
            if (!clickedButton) {
                selectedTrackIndex_ = i;
            }
        }
        
        // Right-click context menu for track
        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
            selectedTrackIndex_ = i;
            ImGui::OpenPopup("track_context_menu");
        }
        
        if (ImGui::BeginPopup("track_context_menu")) {
            // Rename option
            if (ImGui::MenuItem("Rename Track")) {
                renamingTrackIndex_ = static_cast<int>(i);
                if (!tracks_[i].name.empty()) {
                    strncpy(trackRenameBuffer_, tracks_[i].name.c_str(), sizeof(trackRenameBuffer_) - 1);
                } else {
                    snprintf(trackRenameBuffer_, sizeof(trackRenameBuffer_), "Track %zu", i + 1);
                }
                trackRenameBuffer_[sizeof(trackRenameBuffer_) - 1] = '\0';
            }
            
            ImGui::Separator();
            
            // Color picker submenu
            if (ImGui::BeginMenu("Track Color")) {
                // Nice 6x4 color grid (24 colors in HSL gradient)
                static const ImU32 colorPalette[24] = {
                    // Row 1: Saturated (warm to cool)
                    IM_COL32(255, 149, 0, 255),   IM_COL32(255, 94, 94, 255),
                    IM_COL32(255, 94, 153, 255),  IM_COL32(219, 103, 186, 255),
                    IM_COL32(179, 102, 219, 255), IM_COL32(138, 103, 219, 255),
                    // Row 2: Saturated (cool to warm)
                    IM_COL32(102, 128, 219, 255), IM_COL32(51, 153, 219, 255),
                    IM_COL32(51, 179, 204, 255),  IM_COL32(0, 199, 140, 255),
                    IM_COL32(79, 199, 102, 255),  IM_COL32(132, 214, 79, 255),
                    // Row 3: Pastels (warm to cool)
                    IM_COL32(255, 179, 102, 255), IM_COL32(255, 128, 128, 255),
                    IM_COL32(255, 153, 187, 255), IM_COL32(230, 153, 204, 255),
                    IM_COL32(204, 153, 230, 255), IM_COL32(179, 153, 230, 255),
                    // Row 4: Pastels (cool to warm)
                    IM_COL32(153, 170, 230, 255), IM_COL32(128, 191, 230, 255),
                    IM_COL32(128, 217, 217, 255), IM_COL32(102, 217, 191, 255),
                    IM_COL32(153, 230, 153, 255), IM_COL32(255, 230, 102, 255)
                };
                
                ImDrawList* menuDrawList = ImGui::GetWindowDrawList();
                float colorSize = 22.0f;
                float spacing = 3.0f;
                
                for (int row = 0; row < 4; row++) {
                    for (int col = 0; col < 6; col++) {
                        int colorIdx = row * 6 + col;
                        
                        if (col > 0) ImGui::SameLine();
                        
                        char btnId[16];
                        snprintf(btnId, sizeof(btnId), "##col%d", colorIdx);
                        
                        ImVec2 btnPos = ImGui::GetCursorScreenPos();
                        
                        // Draw color swatch button
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(colorPalette[colorIdx]));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(
                            IM_COL32(
                                std::min(255, (int)((colorPalette[colorIdx] & 0xFF) * 1.2f)),
                                std::min(255, (int)(((colorPalette[colorIdx] >> 8) & 0xFF) * 1.2f)),
                                std::min(255, (int)(((colorPalette[colorIdx] >> 16) & 0xFF) * 1.2f)),
                                255
                            )
                        ));
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(colorPalette[colorIdx]));
                        
                        if (ImGui::Button(btnId, ImVec2(colorSize, colorSize))) {
                            tracks_[i].colorIndex = colorIdx;
                            markDirty();
                            ImGui::CloseCurrentPopup();
                        }
                        
                        ImGui::PopStyleColor(3);
                        
                        // Draw selection indicator if this is current color
                        if (tracks_[i].colorIndex == colorIdx) {
                            menuDrawList->AddRect(
                                btnPos, 
                                ImVec2(btnPos.x + colorSize, btnPos.y + colorSize),
                                IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f
                            );
                        }
                    }
                }
                
                ImGui::EndMenu();
            }
            
            ImGui::Separator();
            
            // Delete option
            if (ImGui::MenuItem("Delete Track", nullptr, false, tracks_.size() > 1)) {
                tracks_.erase(tracks_.begin() + i);
                if (selectedTrackIndex_ >= tracks_.size()) {
                    selectedTrackIndex_ = tracks_.size() - 1;
                }
                markDirty();
                ImGui::EndPopup();
                ImGui::PopID();
                ImGui::NextColumn();
                continue;
            }
            ImGui::EndPopup();
        }
        
        // Drop target for waveforms and instruments
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WAVEFORM")) {
                if (payload->DataSize == sizeof(Waveform)) {
                    Waveform droppedWave = *(Waveform*)payload->Data;
                    tracks_[i].oscillators.push_back(Oscillator(droppedWave, 1.0f, 0.5f));
                    tracks_[i].synth->setOscillators(tracks_[i].oscillators);
                    tracks_[i].waveformSet = true;
                    const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
                    tracks_[i].instrumentName = waveNames[static_cast<int>(droppedWave)];
                    selectedTrackIndex_ = i;
                    markDirty();
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INSTRUMENT")) {
                if (payload->DataSize == sizeof(size_t)) {
                    size_t presetIndex = *(size_t*)payload->Data;
                    if (presetIndex < instrumentPresets_.size()) {
                        const auto& preset = instrumentPresets_[presetIndex];
                        // Remove sampler if present
                        tracks_[i].hasSampler = false;
                        tracks_[i].samplerSamplePath.clear();
                        tracks_[i].samplerWaveform.clear();
                        tracks_[i].sampler.reset();
                        tracks_[i].oscillators = preset.oscillators;
                        tracks_[i].synth->setOscillators(tracks_[i].oscillators);
                        tracks_[i].synth->setEnvelope(preset.envelope);  // Apply envelope
                        tracks_[i].waveformSet = true;
                        tracks_[i].instrumentName = preset.name;
                        selectedTrackIndex_ = i;
                        markDirty();
                    }
                }
            }
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("USER_PRESET")) {
                if (payload->DataSize == sizeof(size_t)) {
                    size_t presetIndex = *(size_t*)payload->Data;
                    if (presetIndex < userPresets_.size()) {
                        const auto& preset = userPresets_[presetIndex];
                        // Remove sampler if present
                        tracks_[i].hasSampler = false;
                        tracks_[i].samplerSamplePath.clear();
                        tracks_[i].samplerWaveform.clear();
                        tracks_[i].sampler.reset();
                        tracks_[i].oscillators = preset.oscillators;
                        tracks_[i].synth->setOscillators(tracks_[i].oscillators);
                        tracks_[i].synth->setEnvelope(preset.envelope);  // Apply envelope
                        tracks_[i].waveformSet = true;
                        tracks_[i].instrumentName = preset.name;
                        selectedTrackIndex_ = i;
                        markDirty();
                    }
                }
            }
            // Accept effect preset drops on track headers
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("EFFECT_PRESET")) {
                if (payload->DataSize == sizeof(int) * 2) {
                    int* data = (int*)payload->Data;
                    int effectType = data[0];
                    int presetIdx = data[1];
                    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                    std::shared_ptr<Effect> newEffect;
                    
                    switch (effectType) {
                        case 0: {
                            auto reverb = std::make_shared<Reverb>(sampleRate);
                            reverb->loadPreset(static_cast<Reverb::Preset>(presetIdx));
                            newEffect = reverb;
                            break;
                        }
                        case 1: {
                            auto chorus = std::make_shared<Chorus>(sampleRate);
                            chorus->loadPreset(static_cast<Chorus::Preset>(presetIdx));
                            newEffect = chorus;
                            break;
                        }
                        case 2: {
                            auto distortion = std::make_shared<Distortion>(sampleRate);
                            distortion->loadPreset(static_cast<Distortion::Preset>(presetIdx));
                            newEffect = distortion;
                            break;
                        }
                        case 3: {
                            auto eq8 = std::make_shared<EQ8>(sampleRate);
                            eq8->loadPreset(static_cast<EQ8::Preset>(presetIdx));
                            newEffect = eq8;
                            break;
                        }
                        case 4: {
                            auto pump = std::make_shared<SidechainPump>(sampleRate);
                            newEffect = pump;
                            break;
                        }
                        case 5: {
                            auto wow = std::make_shared<WowFlutter>(sampleRate);
                            newEffect = wow;
                            break;
                        }
                        case 6: {
                            auto br = std::make_shared<BeatRepeat>(sampleRate);
                            newEffect = br;
                            break;
                        }
                        case 7: {
                            auto bit = std::make_shared<BitNoiseTexture>(sampleRate);
                            newEffect = bit;
                            break;
                        }
                        case 8: {
                            auto res = std::make_shared<ResonatorBank>(sampleRate);
                            newEffect = res;
                            break;
                        }
                    }
                    
                    if (newEffect) {
                        tracks_[i].effects.push_back(newEffect);
                        selectedTrackIndex_ = i;
                        g_switchToEffectsTab = true;
                        markDirty();
                    }
                }
            }
            // Accept Sampler drops on track headers
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIMPLER")) {
                tracks_[i].hasSampler = true;
                tracks_[i].samplerSamplePath = "";
                tracks_[i].samplerWaveform.clear();
                tracks_[i].oscillators.clear();  // Clear oscillators when loading sampler
                tracks_[i].instrumentName = "Sampler";
                // Create sampler instance
                double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                tracks_[i].sampler = std::make_shared<Sampler>(sampleRate);
                selectedTrackIndex_ = i;
                markDirty();
            }
            // Accept Sample drops on track headers (load sample into Sampler)
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SAMPLE")) {
                if (payload->DataSize == sizeof(size_t)) {
                    size_t sampleIdx = *(size_t*)payload->Data;
                    if (sampleIdx < userSamples_.size()) {
                        tracks_[i].hasSampler = true;
                        tracks_[i].samplerSamplePath = userSamples_[sampleIdx].path;
                        tracks_[i].samplerWaveform = userSamples_[sampleIdx].waveformDisplay;
                        tracks_[i].oscillators.clear();  // Clear oscillators when loading sample
                        tracks_[i].instrumentName = "Sampler: " + userSamples_[sampleIdx].name;
                        // Create sampler and load sample
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        tracks_[i].sampler = std::make_shared<Sampler>(sampleRate);
                        tracks_[i].sampler->loadSample(userSamples_[sampleIdx].path);
                        selectedTrackIndex_ = i;
                        markDirty();
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        
        // Timeline column
        ImGui::NextColumn();
        renderTrackTimeline(i);
        ImGui::NextColumn();
        
        ImGui::PopID();
    }
    
    // Fill remaining vertical space with grid continuation (timeline continues below tracks)
    ImVec2 remainingPos = ImGui::GetCursorScreenPos();
    float remainingHeight = ImGui::GetContentRegionAvail().y - 40.0f;  // Leave room for Add Track button
    
    if (remainingHeight > 0) {
        // Draw dark grid background for remaining space in both columns
        float leftColumnWidth = ImGui::GetColumnWidth(0);
        
        // Left column empty continuation
        drawList->AddRectFilled(remainingPos, 
                               ImVec2(remainingPos.x + leftColumnWidth, remainingPos.y + remainingHeight),
                               IM_COL32(28, 28, 28, 255));
        
        // Right column timeline grid continuation
        ImGui::NextColumn();
        ImVec2 timelinePos = ImGui::GetCursorScreenPos();
        float timelineWidth = ImGui::GetContentRegionAvail().x;
    
        // Dark background
        drawList->AddRectFilled(timelinePos,
                               ImVec2(timelinePos.x + timelineWidth, timelinePos.y + remainingHeight),
                               IM_COL32(22, 22, 22, 255));
        
        // Draw vertical grid lines to continue the timeline
        const float pixelsPerBeat = 50.0f;
        float visibleBeats = timelineWidth / pixelsPerBeat;
        float startBeat = std::floor(timelineScrollX_ / pixelsPerBeat);
        float endBeat = startBeat + visibleBeats + 1.0f;
        
        for (float beat = startBeat; beat <= endBeat; beat += 1.0f) {
            float x = timelinePos.x + (beat * pixelsPerBeat) - timelineScrollX_;
            if (x >= timelinePos.x && x <= timelinePos.x + timelineWidth) {
                bool isBar = (static_cast<int>(beat) % 4 == 0);
                ImU32 lineColor = isBar ? IM_COL32(50, 50, 50, 255) : IM_COL32(35, 35, 35, 255);
                drawList->AddLine(ImVec2(x, timelinePos.y),
                                 ImVec2(x, timelinePos.y + remainingHeight), lineColor);
            }
        }
        
        ImGui::NextColumn();
    }
    
    // Add Track button - CENTERED in column
    float addColumnWidth = ImGui::GetColumnWidth(0);
    ImVec2 addBtnSize(addColumnWidth - 32.0f, 28.0f);  // Slightly narrower for better centering
    ImVec2 addBtnPos = ImGui::GetCursorScreenPos();
    addBtnPos.x += (addColumnWidth - addBtnSize.x) / 2.0f;  // Center horizontally
    
    ImVec2 addMousePos = ImGui::GetMousePos();
    bool addHovered = (addMousePos.x >= addBtnPos.x && addMousePos.x <= addBtnPos.x + addBtnSize.x &&
                      addMousePos.y >= addBtnPos.y && addMousePos.y <= addBtnPos.y + addBtnSize.y);
    
    ImU32 addBtnColor = addHovered ? IM_COL32(55, 55, 55, 255) : IM_COL32(40, 40, 40, 255);
    drawList->AddRectFilled(addBtnPos, ImVec2(addBtnPos.x + addBtnSize.x, addBtnPos.y + addBtnSize.y),
                           addBtnColor, 2.0f);
    drawList->AddRect(addBtnPos, ImVec2(addBtnPos.x + addBtnSize.x, addBtnPos.y + addBtnSize.y),
                     IM_COL32(70, 70, 70, 255), 2.0f);
    
    const char* addText = "+ Add Track";
    ImVec2 addTextSize = ImGui::CalcTextSize(addText);
    drawList->AddText(ImVec2(addBtnPos.x + (addBtnSize.x - addTextSize.x) / 2.0f,
                             addBtnPos.y + (addBtnSize.y - addTextSize.y) / 2.0f),
                     IM_COL32(150, 150, 150, 255), addText);
    
    if (addHovered && ImGui::IsMouseClicked(0)) {
        Track newTrack;
        newTrack.colorIndex = static_cast<int>(tracks_.size() % 24);
        newTrack.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
        newTrack.synth->setVolume(0.5f);
        newTrack.synth->setOscillators(newTrack.oscillators);
        tracks_.push_back(std::move(newTrack));
        selectedTrackIndex_ = tracks_.size() - 1;
        markDirty();
    }
    
    // Drop target for + Add Track button (accept instruments, sampler, samples)
    ImGui::SetCursorScreenPos(addBtnPos);
    ImGui::InvisibleButton("##addTrackDrop", addBtnSize);
    if (ImGui::BeginDragDropTarget()) {
        // Accept Sampler
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIMPLER")) {
            Track newTrack;
            newTrack.colorIndex = static_cast<int>(tracks_.size() % 24);
            newTrack.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
            newTrack.synth->setVolume(0.5f);
            newTrack.hasSampler = true;
            newTrack.oscillators.clear();
            newTrack.instrumentName = "Sampler";
            // Create sampler instance
            newTrack.sampler = std::make_shared<Sampler>(engine_->getSampleRate());
            tracks_.push_back(std::move(newTrack));
            selectedTrackIndex_ = tracks_.size() - 1;
            markDirty();
        }
        // Accept Sample - create sampler with sample loaded
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SAMPLE")) {
            if (payload->DataSize == sizeof(size_t)) {
                size_t sampleIdx = *(size_t*)payload->Data;
                if (sampleIdx < userSamples_.size()) {
                    Track newTrack;
                    newTrack.colorIndex = static_cast<int>(tracks_.size() % 24);
                    newTrack.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
                    newTrack.synth->setVolume(0.5f);
                    newTrack.hasSampler = true;
                    newTrack.samplerSamplePath = userSamples_[sampleIdx].path;
                    newTrack.samplerWaveform = userSamples_[sampleIdx].waveformDisplay;
                    newTrack.oscillators.clear();
                    newTrack.instrumentName = "Sampler: " + userSamples_[sampleIdx].name;
                    // Create sampler instance and load sample
                    newTrack.sampler = std::make_shared<Sampler>(engine_->getSampleRate());
                    newTrack.sampler->loadSample(userSamples_[sampleIdx].path);
                    tracks_.push_back(std::move(newTrack));
                    selectedTrackIndex_ = tracks_.size() - 1;
                    markDirty();
                }
            }
        }
        // Accept Waveform instruments
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WAVEFORM")) {
            if (payload->DataSize == sizeof(Waveform)) {
                Waveform wave = *(Waveform*)payload->Data;
                Track newTrack;
                newTrack.colorIndex = static_cast<int>(tracks_.size() % 24);
                newTrack.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
                newTrack.synth->setVolume(0.5f);
                newTrack.oscillators.push_back(Oscillator(wave, 1.0f, 0.5f));
                newTrack.synth->setOscillators(newTrack.oscillators);
                const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
                newTrack.instrumentName = waveNames[static_cast<int>(wave)];
                tracks_.push_back(std::move(newTrack));
                selectedTrackIndex_ = tracks_.size() - 1;
                markDirty();
            }
        }
        // Accept Instrument presets
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("INSTRUMENT_PRESET")) {
            if (payload->DataSize == sizeof(size_t)) {
                size_t presetIdx = *(size_t*)payload->Data;
                if (presetIdx < instrumentPresets_.size()) {
                    Track newTrack;
                    newTrack.colorIndex = static_cast<int>(tracks_.size() % 24);
                    newTrack.synth = std::make_shared<Synthesizer>(engine_->getSampleRate());
                    newTrack.synth->setVolume(0.5f);
                    newTrack.oscillators = instrumentPresets_[presetIdx].oscillators;
                    newTrack.synth->setOscillators(newTrack.oscillators);
                    newTrack.synth->setEnvelope(instrumentPresets_[presetIdx].envelope);
                    newTrack.instrumentName = instrumentPresets_[presetIdx].name;
                    tracks_.push_back(std::move(newTrack));
                    selectedTrackIndex_ = tracks_.size() - 1;
                    markDirty();
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    ImGui::Dummy(ImVec2(addColumnWidth, addBtnSize.y + 8.0f));
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
        
        // Add home directory (using text labels - no emoji as ImGui font doesn't support them)
        commonDirectories_.push_back({"Home", homePath.string()});
        
        // Add common subdirectories if they exist
        std::vector<std::pair<std::string, std::string>> commonDirs = {
            {"Documents", (homePath / "Documents").string()},
            {"Downloads", (homePath / "Downloads").string()},
            {"Music", (homePath / "Music").string()},
            {"Pictures", (homePath / "Pictures").string()},
            {"Videos", (homePath / "Videos").string()},
            {"Desktop", (homePath / "Desktop").string()}
        };
        
        for (const auto& [name, path] : commonDirs) {
            if (fs::exists(path) && fs::is_directory(path)) {
                commonDirectories_.push_back({name, path});
            }
        }
    }
    
    // Add root directory
    commonDirectories_.push_back({"Root /", "/"});
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
    
    // Position display to the left of BPM
    float posWidth = 100.0f;  // Position display width
    float bpmWidth = 150.0f;  // Total width for "BPM:" + input
    float posStartX = centerX - buttonAreaWidth / 2.0f - bpmWidth - posWidth - 40.0f;
    
    ImGui::SetCursorPosX(posStartX);
    
    // Vertically center content
    float textHeight = ImGui::GetTextLineHeight();
    float buttonHeight = 30.0f;
    float verticalOffset = (buttonHeight - textHeight) / 2.0f;
    
    // Timeline position display (editable/draggable)
    static bool posEditing = false;
    static char posInput[64] = "";
    static bool posDragging = false;
    static float posDragStartX = 0.0f;
    static float posDragStartValue = 0.0f;
    
    float currentPos;
    {
        std::lock_guard<std::mutex> lock(timelineMutex_);
        currentPos = timelinePosition_;
    }
    
    // Convert beats to bar.beat.subdivision format (4 beats per bar, 4 subdivisions per beat)
    int bars = static_cast<int>(currentPos / 4.0f) + 1;
    float beatsInBar = std::fmod(currentPos, 4.0f);
    int beats = static_cast<int>(beatsInBar) + 1;
    float subBeatFraction = std::fmod(beatsInBar, 1.0f);
    int subdivision = static_cast<int>(subBeatFraction * 4.0f) + 1;  // 1-4 subdivisions per beat
    
    ImGui::PushID("position");
    ImGui::SetNextItemWidth(90.0f);  // Slightly wider for new format
    
    if (posEditing) {
        // Text input mode
        if (ImGui::InputText("##pos_input", posInput, sizeof(posInput),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            // Parse bar.beat.subdivision or bar.beat or just beats
            int newBar = 1, newBeat = 1, newSub = 1;
            if (sscanf(posInput, "%d.%d.%d", &newBar, &newBeat, &newSub) == 3) {
                float newPos = (newBar - 1) * 4.0f + (newBeat - 1) + (newSub - 1) * 0.25f;
                if (newPos >= 0.0f) {
                    std::lock_guard<std::mutex> lock(timelineMutex_);
                    timelinePosition_ = newPos;
                    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                    float beatsPerSecond = bpm_ / 60.0f;
                    playbackSamplePosition_ = static_cast<int64_t>(newPos / beatsPerSecond * sampleRate);
                }
            } else if (sscanf(posInput, "%d.%d", &newBar, &newBeat) == 2) {
                float newPos = (newBar - 1) * 4.0f + (newBeat - 1);
                if (newPos >= 0.0f) {
                    std::lock_guard<std::mutex> lock(timelineMutex_);
                    timelinePosition_ = newPos;
                    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                    float beatsPerSecond = bpm_ / 60.0f;
                    playbackSamplePosition_ = static_cast<int64_t>(newPos / beatsPerSecond * sampleRate);
                }
            } else {
                float newPos = std::atof(posInput);
                if (newPos >= 0.0f) {
                    std::lock_guard<std::mutex> lock(timelineMutex_);
                    timelinePosition_ = newPos;
                    double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                    float beatsPerSecond = bpm_ / 60.0f;
                    playbackSamplePosition_ = static_cast<int64_t>(newPos / beatsPerSecond * sampleRate);
                }
            }
            posEditing = false;
            posInput[0] = '\0';
        }
        if (!ImGui::IsItemActive() && ImGui::IsMouseClicked(0)) {
            posEditing = false;
            posInput[0] = '\0';
        }
    } else {
        // Display mode - bar.beat.subdivision format
        char posDisplay[64];
        snprintf(posDisplay, sizeof(posDisplay), "%d.%d.%d", bars, beats, subdivision);
        
        ImGui::Button(posDisplay, ImVec2(80.0f, buttonHeight));
        bool wasHovered = ImGui::IsItemHovered();
        
        // Handle dragging
        if (posDragging) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            
            float dragDelta = ImGui::GetMousePos().x - posDragStartX;
            float beatsDelta = dragDelta * 0.05f;  // ~20 pixels = 1 beat
            float newPos = std::max(0.0f, posDragStartValue + beatsDelta);
            
            {
                std::lock_guard<std::mutex> lock(timelineMutex_);
                timelinePosition_ = newPos;
            }
            
            // Update playback position if not playing
            if (!isPlaying_) {
                double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                float beatsPerSecond = bpm_ / 60.0f;
                playbackSamplePosition_ = static_cast<int64_t>(newPos / beatsPerSecond * sampleRate);
            }
            
            if (!ImGui::IsMouseDown(0)) {
                posDragging = false;
            }
        } else {
            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f)) {
                posDragging = true;
                posDragStartX = ImGui::GetMousePos().x;
                std::lock_guard<std::mutex> lock(timelineMutex_);
                posDragStartValue = timelinePosition_;
            }
        }
        
        if (!posDragging && wasHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            
            if (ImGui::IsMouseDoubleClicked(0)) {
                snprintf(posInput, sizeof(posInput), "%d.%d.%d", bars, beats, subdivision);
                posEditing = true;
            } else {
                // Scroll to change position
                float scroll = ImGui::GetIO().MouseWheel;
                if (scroll != 0.0f) {
                    float newPos = std::max(0.0f, currentPos + scroll * 0.5f);
                    {
                        std::lock_guard<std::mutex> lock(timelineMutex_);
                        timelinePosition_ = newPos;
                    }
                    if (!isPlaying_) {
                        double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
                        float beatsPerSecond = bpm_ / 60.0f;
                        playbackSamplePosition_ = static_cast<int64_t>(newPos / beatsPerSecond * sampleRate);
                    }
                }
            }
        }
    }
    ImGui::PopID();
    
    ImGui::SameLine();
    
    // Position BPM controls to the left of center
    float bpmStartX = centerX - buttonAreaWidth / 2.0f - bpmWidth - 20.0f;
    
    ImGui::SetCursorPosX(bpmStartX);
    
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
    
    // Master output meter (right side of transport - flush with edge)
    float masterMeterWidth = 60.0f;
    float masterMeterHeight = 14.0f;
    float meterStartX = windowWidth - masterMeterWidth - 8.0f;  // 8px from right edge
    
    ImGui::SameLine();
    ImGui::SetCursorPosX(meterStartX);
    
    // Update peak hold
    auto now = std::chrono::steady_clock::now();
    double currentTime = std::chrono::duration<double>(now.time_since_epoch()).count();
    
    if (currentTime - masterPeakHoldTime_ > 1.5) {
        masterPeakHoldL_ = std::max(0.0f, masterPeakHoldL_ - 0.01f);
        masterPeakHoldR_ = std::max(0.0f, masterPeakHoldR_ - 0.01f);
    }
    
    if (masterPeakL_ > masterPeakHoldL_) {
        masterPeakHoldL_ = masterPeakL_;
        masterPeakHoldTime_ = currentTime;
    }
    if (masterPeakR_ > masterPeakHoldR_) {
        masterPeakHoldR_ = masterPeakR_;
        masterPeakHoldTime_ = currentTime;
    }
    
    // Draw stereo meter
    ImVec2 meterPos = ImGui::GetCursorScreenPos();
    float channelHeight = 6.0f;
    float channelGap = 2.0f;
    
    // Background - subtle border
    drawList->AddRectFilled(meterPos, ImVec2(meterPos.x + masterMeterWidth, meterPos.y + masterMeterHeight),
                           IM_COL32(12, 12, 12, 255), 2.0f);
    drawList->AddRect(meterPos, ImVec2(meterPos.x + masterMeterWidth, meterPos.y + masterMeterHeight),
                     IM_COL32(40, 40, 40, 255), 2.0f);
    
    // Left channel
    float levelL = std::min(1.0f, masterPeakL_);
    float levelR = std::min(1.0f, masterPeakR_);
    
    // Gradient color based on level
    auto getMeterColor = [](float level) -> ImU32 {
        if (level > 0.9f) return IM_COL32(255, 50, 50, 255);    // Red (clipping)
        if (level > 0.7f) return IM_COL32(255, 200, 0, 255);    // Yellow
        return IM_COL32(132, 214, 79, 255);                      // Green
    };
    
    // Left channel fill
    if (levelL > 0.001f) {
        drawList->AddRectFilled(
            ImVec2(meterPos.x + 1, meterPos.y + 1),
            ImVec2(meterPos.x + 1 + (masterMeterWidth - 2) * levelL, meterPos.y + 1 + channelHeight),
            getMeterColor(levelL), 1.0f);
    }
    
    // Right channel fill
    if (levelR > 0.001f) {
        drawList->AddRectFilled(
            ImVec2(meterPos.x + 1, meterPos.y + 1 + channelHeight + channelGap),
            ImVec2(meterPos.x + 1 + (masterMeterWidth - 2) * levelR, meterPos.y + 1 + channelHeight + channelGap + channelHeight),
            getMeterColor(levelR), 1.0f);
    }
    
    // Peak hold indicators
    if (masterPeakHoldL_ > 0.01f) {
        float peakX = meterPos.x + 1 + (masterMeterWidth - 2) * masterPeakHoldL_;
        drawList->AddLine(ImVec2(peakX, meterPos.y + 1),
                         ImVec2(peakX, meterPos.y + 1 + channelHeight),
                         IM_COL32(255, 255, 255, 200), 1.0f);
    }
    if (masterPeakHoldR_ > 0.01f) {
        float peakX = meterPos.x + 1 + (masterMeterWidth - 2) * masterPeakHoldR_;
        drawList->AddLine(ImVec2(peakX, meterPos.y + 1 + channelHeight + channelGap),
                         ImVec2(peakX, meterPos.y + 1 + channelHeight + channelGap + channelHeight),
                         IM_COL32(255, 255, 255, 200), 1.0f);
    }
    
    ImGui::Dummy(ImVec2(masterMeterWidth, masterMeterHeight));
    
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
    const float timelineHeight = 60.0f;  // Match track header height
    const float beatMarkerHeight = 16.0f;
    
    // Track color palette (24 colors, same as renderTracks)
    static const ImU32 trackColors[24] = {
        IM_COL32(255, 149, 0, 255), IM_COL32(255, 94, 94, 255),
        IM_COL32(255, 94, 153, 255), IM_COL32(219, 103, 186, 255),
        IM_COL32(179, 102, 219, 255), IM_COL32(138, 103, 219, 255),
        IM_COL32(102, 128, 219, 255), IM_COL32(51, 153, 219, 255),
        IM_COL32(51, 179, 204, 255), IM_COL32(0, 199, 140, 255),
        IM_COL32(79, 199, 102, 255), IM_COL32(132, 214, 79, 255),
        IM_COL32(255, 179, 102, 255), IM_COL32(255, 128, 128, 255),
        IM_COL32(255, 153, 187, 255), IM_COL32(230, 153, 204, 255),
        IM_COL32(204, 153, 230, 255), IM_COL32(179, 153, 230, 255),
        IM_COL32(153, 170, 230, 255), IM_COL32(128, 191, 230, 255),
        IM_COL32(128, 217, 217, 255), IM_COL32(102, 217, 191, 255),
        IM_COL32(153, 230, 153, 255), IM_COL32(255, 230, 102, 255)
    };
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 600.0f);
    canvasSize.y = timelineHeight;
    
    int trackColorIdx = tracks_[trackIndex].colorIndex % 24;
    
    // Dark timeline background
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                           IM_COL32(22, 22, 22, 255));
    
    // Beat marker area (top)
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + beatMarkerHeight),
                           IM_COL32(30, 30, 30, 255));
    
    // Draw beat markers
    float visibleBeats = canvasSize.x / pixelsPerBeat;
    float startBeat = std::floor(timelineScrollX_ / pixelsPerBeat);
    float endBeat = startBeat + visibleBeats + 1.0f;
    
    for (float beat = startBeat; beat <= endBeat; beat += 1.0f) {
        float x = canvasPos.x + (beat * pixelsPerBeat) - timelineScrollX_;
        if (x >= canvasPos.x && x <= canvasPos.x + canvasSize.x) {
            bool isBar = (static_cast<int>(beat) % 4 == 0);
            
            // Vertical line
            ImU32 lineColor = isBar ? IM_COL32(70, 70, 70, 255) : IM_COL32(45, 45, 45, 255);
            drawList->AddLine(ImVec2(x, canvasPos.y + beatMarkerHeight),
                             ImVec2(x, canvasPos.y + canvasSize.y), lineColor);
            
            // Beat/bar number
            if (isBar) {
                char beatLabel[16];
                snprintf(beatLabel, sizeof(beatLabel), "%d", static_cast<int>(beat / 4.0f) + 1);
                drawList->AddText(ImVec2(x + 3, canvasPos.y + 2), IM_COL32(100, 100, 100, 255), beatLabel);
            }
        }
    }
    
    // Bottom border of beat marker area
    drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + beatMarkerHeight),
                     ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + beatMarkerHeight),
                     IM_COL32(55, 55, 55, 255));
    
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
    
    // Draw notes as rounded rectangles with track color
    float laneHeight = canvasSize.y - beatMarkerHeight - 8.0f;
    float pixelsPerNote = laneHeight / visibleNoteRange;
    float noteDrawHeight = std::max(8.0f, pixelsPerNote * 0.85f);
    
    // Get track color for notes
    ImU32 baseColor = trackColors[trackColorIdx];
    int baseR = (baseColor >> 0) & 0xFF;
    int baseG = (baseColor >> 8) & 0xFF;
    int baseB = (baseColor >> 16) & 0xFF;
    
    for (const auto& note : notesToDraw) {
        float startX = canvasPos.x + (note.startBeat * pixelsPerBeat) - timelineScrollX_;
        float endX = canvasPos.x + (note.endBeat * pixelsPerBeat) - timelineScrollX_;
        
        if (endX >= canvasPos.x && startX <= canvasPos.x + canvasSize.x) {
            float noteOffsetFromCenter = centerNote - note.noteNum;
            float noteY = canvasPos.y + beatMarkerHeight + 4.0f + (laneHeight / 2.0f) + (noteOffsetFromCenter * pixelsPerNote);
            
            // Velocity affects brightness
            float velocityFactor = 0.6f + (note.velocity / 127.0f) * 0.4f;
            int r = static_cast<int>(baseR * velocityFactor);
            int g = static_cast<int>(baseG * velocityFactor);
            int b = static_cast<int>(baseB * velocityFactor);
            
            ImU32 noteColor = IM_COL32(r, g, b, 255);
            
            float drawEndX = std::max(endX, startX + 3.0f);
            
            // Rounded note rectangle
            drawList->AddRectFilled(
                ImVec2(startX, noteY),
                ImVec2(drawEndX, noteY + noteDrawHeight),
                noteColor, 2.0f
            );
            
            // Subtle border
            drawList->AddRect(
                ImVec2(startX, noteY),
                ImVec2(drawEndX, noteY + noteDrawHeight),
                IM_COL32(r/2, g/2, b/2, 200), 2.0f, 0, 1.0f
            );
        }
    }
    
    // Draw playhead (vertical line with triangle head) - Ableton style
    float currentTimelinePos;
    {
        std::lock_guard<std::mutex> lock(timelineMutex_);
        currentTimelinePos = timelinePosition_;
    }
    float playheadX = canvasPos.x + (currentTimelinePos * pixelsPerBeat) - timelineScrollX_;
    if (playheadX >= canvasPos.x && playheadX <= canvasPos.x + canvasSize.x) {
        // Playhead line (orange like Ableton)
        ImU32 playheadColor = IM_COL32(255, 149, 0, 255);
        drawList->AddLine(
            ImVec2(playheadX, canvasPos.y),
            ImVec2(playheadX, canvasPos.y + canvasSize.y),
            playheadColor, 2.0f
        );
        
        // Triangle head at top
        drawList->AddTriangleFilled(
            ImVec2(playheadX - 5, canvasPos.y),
            ImVec2(playheadX + 5, canvasPos.y),
            ImVec2(playheadX, canvasPos.y + 8),
            playheadColor
        );
    }
    
    // Thin 1px separator line at bottom of timeline (visual divider between tracks)
    drawList->AddLine(ImVec2(canvasPos.x, canvasPos.y + canvasSize.y - 1),
                     ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y - 1),
                     IM_COL32(15, 15, 15, 255));
    
    // Set cursor at TOP of canvas for interaction, then InvisibleButton advances cursor correctly
    ImGui::SetCursorScreenPos(canvasPos);
    ImGui::InvisibleButton("timeline_canvas", canvasSize);
    
    // Handle playhead dragging on timeline (click or drag to set position)
    static bool timelineDragging = false;
    if (ImGui::IsItemHovered() || timelineDragging) {
        ImVec2 mousePos = ImGui::GetMousePos();
        
        // Check if near the playhead or on the ruler area (top 20 pixels)
        bool nearPlayhead = std::abs(mousePos.x - playheadX) < 10.0f;
        bool onRuler = (mousePos.y - canvasPos.y) < 20.0f;
        
        if (nearPlayhead || onRuler) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        
        // Start dragging on click in ruler area or on playhead
        if (ImGui::IsMouseClicked(0) && (nearPlayhead || onRuler)) {
            timelineDragging = true;
        }
        
        // Update position while dragging
        if (timelineDragging) {
            float newBeat = (mousePos.x - canvasPos.x + timelineScrollX_) / pixelsPerBeat;
            newBeat = std::max(0.0f, newBeat);
            
            {
                std::lock_guard<std::mutex> lock(timelineMutex_);
                timelinePosition_ = newBeat;
            }
            
            // Update playback sample position
            double sampleRate = engine_ ? engine_->getSampleRate() : 44100.0;
            float beatsPerSecond = bpm_ / 60.0f;
            playbackSamplePosition_ = static_cast<int64_t>(newBeat / beatsPerSecond * sampleRate);
            
            if (!ImGui::IsMouseDown(0)) {
                timelineDragging = false;
            }
        }
    }
    
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
    
    // Reserve bottom 1/4 for velocity editor
    ImVec2 fullCanvasPos = ImGui::GetCursorScreenPos();
    ImVec2 fullCanvasSize = ImGui::GetContentRegionAvail();
    const float velocityEditorHeight = std::max(80.0f, fullCanvasSize.y * 0.25f);
    
    ImVec2 canvasPos = fullCanvasPos;
    ImVec2 canvasSize = fullCanvasSize;
    canvasSize.y = std::max(fullCanvasSize.y - velocityEditorHeight - 4.0f, totalNotes * noteHeight);
    
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    // Background - darker for Ableton style
    drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), 
                           IM_COL32(18, 18, 18, 255));
    
    // Draw piano keys on the left with Ableton-style coloring
    for (int i = 0; i < totalNotes; ++i) {
        int noteNum = lowestNote + (totalNotes - 1 - i);
        int noteInOctave = noteNum % 12;
        float y = canvasPos.y + i * noteHeight;
        
        // Black keys are darker
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || 
                          noteInOctave == 8 || noteInOctave == 10);
        bool isC = (noteInOctave == 0);
        
        // Ableton-style: white keys are light gray, black keys are darker
        ImU32 keyColor = isBlackKey ? IM_COL32(35, 35, 35, 255) : IM_COL32(60, 60, 60, 255);
        ImU32 textColor = isBlackKey ? IM_COL32(100, 100, 100, 255) : IM_COL32(140, 140, 140, 255);
        
        // C notes get a slightly different background
        if (isC) {
            keyColor = IM_COL32(70, 70, 70, 255);
        }
        
        drawList->AddRectFilled(ImVec2(canvasPos.x, y), 
                               ImVec2(canvasPos.x + pianoKeyWidth, y + noteHeight),
                               keyColor);
        
        // Subtle separator between keys
        drawList->AddLine(ImVec2(canvasPos.x, y + noteHeight - 1),
                         ImVec2(canvasPos.x + pianoKeyWidth, y + noteHeight - 1),
                         IM_COL32(25, 25, 25, 255));
        
        // Draw note name on C notes (larger, clearer)
        if (isC) {
            char noteName[16];
            snprintf(noteName, sizeof(noteName), "C%d", noteNum / 12 - 1);
            drawList->AddText(ImVec2(canvasPos.x + 4, y + 1), 
                             IM_COL32(180, 180, 180, 255), noteName);
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
    
    // Draw horizontal lane backgrounds (alternating for octaves, Ableton-style)
    for (int i = 0; i < totalNotes; ++i) {
        int noteNum = lowestNote + (totalNotes - 1 - i);
        int noteInOctave = noteNum % 12;
        float y = canvasPos.y + i * noteHeight;
        
        // White keys get slightly lighter lanes
        bool isBlackKey = (noteInOctave == 1 || noteInOctave == 3 || noteInOctave == 6 || 
                          noteInOctave == 8 || noteInOctave == 10);
        bool isC = (noteInOctave == 0);
        
        ImU32 laneColor;
        if (isC) {
            laneColor = IM_COL32(30, 30, 30, 255);  // C notes - slightly brighter
        } else if (isBlackKey) {
            laneColor = IM_COL32(22, 22, 22, 255);  // Black key lanes - darker
        } else {
            laneColor = IM_COL32(26, 26, 26, 255);  // White key lanes
        }
        
        drawList->AddRectFilled(ImVec2(gridStart, y),
                               ImVec2(gridStart + gridWidth, y + noteHeight),
                               laneColor);
        
        // Subtle lane separator
        drawList->AddLine(ImVec2(gridStart, y + noteHeight - 1),
                         ImVec2(gridStart + gridWidth, y + noteHeight - 1),
                         IM_COL32(35, 35, 35, 255));
    }
    
    // Draw subdivision lines (vertical)
    for (int i = 0; i < totalSubdivisions; ++i) {
        float x = gridStart + i * subdivisionPixels;
        float beatPos = i * subdivisionBeats;
        
        // Determine line strength based on position
        bool isBar = (std::abs(std::fmod(beatPos, 4.0f)) < 0.001f);
        bool isBeat = (std::abs(std::fmod(beatPos, 1.0f)) < 0.001f);
        
        ImU32 lineColor;
        float lineThickness;
        
        if (isBar) {
            // Bar line (every 4 beats) - brighter
            lineColor = IM_COL32(80, 80, 80, 255);
            lineThickness = 1.0f;
        } else if (isBeat) {
            // Beat line
            lineColor = IM_COL32(50, 50, 50, 255);
            lineThickness = 1.0f;
        } else {
            // Subdivision line - very subtle
            lineColor = IM_COL32(38, 38, 38, 255);
            lineThickness = 1.0f;
        }
        
        drawList->AddLine(ImVec2(x, canvasPos.y), 
                         ImVec2(x, canvasPos.y + canvasSize.y),
                         lineColor, lineThickness);
        
        // Draw bar number on bar lines
        if (isBar) {
            char beatLabel[16];
            snprintf(beatLabel, sizeof(beatLabel), "%d", static_cast<int>(beatPos / 4.0f) + 1);
            drawList->AddText(ImVec2(x + 4, canvasPos.y + 2), IM_COL32(100, 100, 100, 255), beatLabel);
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
    
    // Get track color for notes (Ableton-style: notes inherit track color - 24 colors)
    static const ImU32 trackColors[24] = {
        IM_COL32(255, 149, 0, 255),   // 0: Orange (playhead color)
        IM_COL32(255, 94, 94, 255),   // 1: Red
        IM_COL32(255, 94, 153, 255),  // 2: Rose
        IM_COL32(219, 103, 186, 255), // 3: Pink
        IM_COL32(179, 102, 219, 255), // 4: Magenta
        IM_COL32(138, 103, 219, 255), // 5: Purple
        IM_COL32(102, 128, 219, 255), // 6: Indigo
        IM_COL32(51, 153, 219, 255),  // 7: Blue
        IM_COL32(51, 179, 204, 255),  // 8: Cyan
        IM_COL32(0, 199, 140, 255),   // 9: Teal
        IM_COL32(79, 199, 102, 255),  // 10: Green
        IM_COL32(132, 214, 79, 255),  // 11: Lime
        IM_COL32(255, 179, 102, 255), // 12: Peach
        IM_COL32(255, 128, 128, 255), // 13: Light red
        IM_COL32(255, 153, 187, 255), // 14: Light rose
        IM_COL32(230, 153, 204, 255), // 15: Light pink
        IM_COL32(204, 153, 230, 255), // 16: Light magenta
        IM_COL32(179, 153, 230, 255), // 17: Light purple
        IM_COL32(153, 170, 230, 255), // 18: Light indigo
        IM_COL32(128, 191, 230, 255), // 19: Light blue
        IM_COL32(128, 217, 217, 255), // 20: Light cyan
        IM_COL32(102, 217, 191, 255), // 21: Light teal
        IM_COL32(153, 230, 153, 255), // 22: Light green
        IM_COL32(255, 230, 102, 255)  // 23: Light yellow
    };
    
    int trackColorIdx = (selectedTrackIndex_ < tracks_.size()) ? tracks_[selectedTrackIndex_].colorIndex : 0;
    ImU32 baseNoteColor = trackColors[trackColorIdx % 24];
    
    // Draw all notes with rounded corners
    for (const auto& note : noteRects) {
        if (note.noteNum >= lowestNote && note.noteNum < lowestNote + totalNotes) {
            bool isSelected = selectedNotes_.find({note.clipIndex, note.eventIndex}) != selectedNotes_.end();
            bool isBeingDragged = isSelected && draggedNote_.isDragging;
            
            float displayStartBeat = note.startBeat;
            uint8_t displayNoteNum = note.noteNum;
            
            if (isBeingDragged) {
                displayStartBeat += draggedNote_.currentBeatDelta;
                displayNoteNum = std::clamp(static_cast<int>(note.noteNum) + draggedNote_.currentNoteDelta, 0, 127);
            }
            
            if (displayNoteNum >= lowestNote && displayNoteNum < lowestNote + totalNotes) {
                int noteIndex = totalNotes - 1 - (displayNoteNum - lowestNote);
                float noteY = canvasPos.y + noteIndex * noteHeight;
                float noteX = gridStart + displayStartBeat * pixelsPerBeat;
                float noteW = (note.endBeat - note.startBeat) * pixelsPerBeat;
                
                // Velocity affects brightness (Ableton-style)
                float velocityFactor = 0.6f + (note.velocity / 127.0f) * 0.4f;
                
                // Extract RGB from base color and apply velocity
                int r = ((baseNoteColor >> 0) & 0xFF);
                int g = ((baseNoteColor >> 8) & 0xFF);
                int b = ((baseNoteColor >> 16) & 0xFF);
                
                r = static_cast<int>(r * velocityFactor);
                g = static_cast<int>(g * velocityFactor);
                b = static_cast<int>(b * velocityFactor);
                
                ImU32 noteColor = IM_COL32(r, g, b, 255);
                
                // Selected notes get white highlight
                if (isSelected) {
                    noteColor = IM_COL32(255, 255, 255, 255);
                }
                
                // Dragged notes are semi-transparent
                if (isBeingDragged) {
                    noteColor = (noteColor & 0x00FFFFFF) | 0xB0000000;
                }
                
                float cornerRadius = 2.0f;
                float noteTop = noteY + 1.0f;
                float noteBottom = noteY + noteHeight - 1.0f;
                
                // Note fill with rounded corners
                drawList->AddRectFilled(ImVec2(noteX, noteTop), 
                                       ImVec2(noteX + noteW, noteBottom),
                                       noteColor, cornerRadius);
                
                // Subtle darker border for depth
                ImU32 borderColor = isSelected ? 
                    IM_COL32(200, 200, 200, 255) : 
                    IM_COL32(r/2, g/2, b/2, 200);
                drawList->AddRect(ImVec2(noteX, noteTop), 
                                 ImVec2(noteX + noteW, noteBottom),
                                 borderColor, cornerRadius, 0, 1.0f);
                
                // Top highlight for 3D effect (only on non-selected)
                if (!isSelected && noteW > 4.0f) {
                    ImU32 highlightColor = IM_COL32(
                        std::min(255, r + 40),
                        std::min(255, g + 40),
                        std::min(255, b + 40), 150);
                    drawList->AddLine(ImVec2(noteX + 1, noteTop + 1),
                                     ImVec2(noteX + noteW - 1, noteTop + 1),
                                     highlightColor);
                }
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
    
    // Set draw tool cursor when active - ONLY within piano roll window AND canvas
    // Must check both IsWindowHovered (mouse over Piano Roll window) AND mouseOverCanvas (over grid area)
    bool pianoRollWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (pencilToolActive_ && pianoRollWindowHovered && mouseOverCanvas && !drawingNote_.isDrawing) {
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
    // Note: Arrow cursor is set at frame start when pencilToolActive_ is true
    
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
    
    // === VELOCITY EDITOR ===
    // Draw velocity editor below the piano roll
    float velocityY = canvasPos.y + canvasSize.y + 4.0f;
    float velocityX = canvasPos.x + pianoKeyWidth;
    float velocityWidth = canvasSize.x - pianoKeyWidth;
    
    renderVelocityEditor(velocityX, velocityY, velocityWidth, velocityEditorHeight);
    
    ImGui::End();
#endif
}

void MainWindow::renderVelocityEditor(float canvasX, float canvasY, float canvasWidth, float canvasHeight) {
#ifdef PAN_USE_GUI
    if (tracks_.empty() || selectedTrackIndex_ >= tracks_.size()) return;
    
    auto& track = tracks_[selectedTrackIndex_];
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    
    const float pixelsPerBeat = 50.0f;
    const float pianoKeyWidth = 60.0f;
    const float scrollX = 0.0f;  // Piano roll starts at beat 0
    
    // Background
    drawList->AddRectFilled(
        ImVec2(canvasX - pianoKeyWidth, canvasY),
        ImVec2(canvasX + canvasWidth, canvasY + canvasHeight),
        IM_COL32(25, 25, 28, 255)
    );
    
    // Label area
    drawList->AddRectFilled(
        ImVec2(canvasX - pianoKeyWidth, canvasY),
        ImVec2(canvasX, canvasY + canvasHeight),
        IM_COL32(40, 40, 45, 255)
    );
    drawList->AddText(ImVec2(canvasX - pianoKeyWidth + 5, canvasY + 5), IM_COL32(150, 150, 150, 255), "VEL");
    
    // Draw velocity scale lines
    for (int i = 0; i <= 4; ++i) {
        float y = canvasY + canvasHeight - (i / 4.0f) * canvasHeight;
        ImU32 lineColor = (i == 4) ? IM_COL32(100, 100, 100, 100) : IM_COL32(50, 50, 55, 100);
        drawList->AddLine(ImVec2(canvasX, y), ImVec2(canvasX + canvasWidth, y), lineColor);
    }
    
    // Draw beat grid lines
    float gridEndBeat = scrollX + canvasWidth / pixelsPerBeat;
    
    for (float beat = 0.0f; beat <= gridEndBeat; beat += 1.0f) {
        float x = canvasX + (beat - scrollX) * pixelsPerBeat;
        if (x >= canvasX && x <= canvasX + canvasWidth) {
            bool isMeasure = (static_cast<int>(beat) % 4 == 0);
            ImU32 lineColor = isMeasure ? IM_COL32(70, 70, 75, 150) : IM_COL32(40, 40, 45, 100);
            drawList->AddLine(ImVec2(x, canvasY), ImVec2(x, canvasY + canvasHeight), lineColor);
        }
    }
    
    // Get track color
    static const ImU32 trackColors[24] = {
        IM_COL32(255, 149, 0, 255), IM_COL32(255, 94, 94, 255),
        IM_COL32(255, 94, 153, 255), IM_COL32(219, 103, 186, 255),
        IM_COL32(179, 102, 219, 255), IM_COL32(138, 103, 219, 255),
        IM_COL32(102, 128, 219, 255), IM_COL32(51, 153, 219, 255),
        IM_COL32(51, 179, 204, 255), IM_COL32(0, 199, 140, 255),
        IM_COL32(79, 199, 102, 255), IM_COL32(132, 214, 79, 255),
        IM_COL32(255, 179, 102, 255), IM_COL32(255, 128, 128, 255),
        IM_COL32(255, 153, 187, 255), IM_COL32(230, 153, 204, 255),
        IM_COL32(204, 153, 230, 255), IM_COL32(179, 153, 230, 255),
        IM_COL32(153, 170, 230, 255), IM_COL32(128, 191, 230, 255),
        IM_COL32(128, 217, 217, 255), IM_COL32(102, 217, 191, 255),
        IM_COL32(153, 230, 153, 255), IM_COL32(255, 230, 102, 255)
    };
    int colorIdx = track.colorIndex % 24;
    ImU32 trackColor = trackColors[colorIdx];
    
    // Draw velocity bars for each note in all clips
    for (size_t clipIdx = 0; clipIdx < track.clips.size(); ++clipIdx) {
        auto& clip = track.clips[clipIdx];
        if (!clip) continue;
        
        auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
        float clipStartBeat = static_cast<float>(clip->getStartTime()) / (44100.0f * 60.0f / bpm_);
        
        for (size_t i = 0; i < events.size(); ++i) {
            auto& event = events[i];
            if (!event.message.isNoteOn()) continue;
            
            float noteStartBeat = clipStartBeat + event.timestamp;
            float x = canvasX + (noteStartBeat - scrollX) * pixelsPerBeat;
            
            // Skip notes outside visible area
            if (x < canvasX - 20 || x > canvasX + canvasWidth + 20) continue;
            
            // Calculate bar height based on velocity
            float velocity = event.message.getVelocity() / 127.0f;
            float barHeight = velocity * (canvasHeight - 10.0f);
            float barY = canvasY + canvasHeight - barHeight - 5.0f;
            
            // Bar width (thin vertical bar)
            float barWidth = 6.0f;
            
            // Check if this note is selected
            bool isSelected = selectedNotes_.count({selectedTrackIndex_, clipIdx}) > 0 ||
                             selectedNotes_.count({selectedTrackIndex_, i}) > 0;
            
            // Draw velocity bar
            ImU32 barColor = isSelected ? IM_COL32(255, 255, 255, 255) : trackColor;
            drawList->AddRectFilled(
                ImVec2(x - barWidth/2, barY),
                ImVec2(x + barWidth/2, canvasY + canvasHeight - 5.0f),
                barColor
            );
            
            // Draw velocity handle at top
            drawList->AddCircleFilled(ImVec2(x, barY), 4.0f, barColor);
            
            // Check for mouse interaction
            ImVec2 mousePos = ImGui::GetMousePos();
            bool hoveringBar = mousePos.x >= x - barWidth/2 - 4 && mousePos.x <= x + barWidth/2 + 4 &&
                              mousePos.y >= barY - 4 && mousePos.y <= canvasY + canvasHeight;
            
            if (hoveringBar) {
                // Show tooltip
                ImGui::BeginTooltip();
                ImGui::Text("Velocity: %d", event.message.getVelocity());
                ImGui::EndTooltip();
                
                // Drag to change velocity
                if (ImGui::IsMouseDown(0)) {
                    float newVelocity = 1.0f - (mousePos.y - canvasY - 5.0f) / (canvasHeight - 10.0f);
                    newVelocity = std::clamp(newVelocity, 0.0f, 1.0f);
                    uint8_t newVel = static_cast<uint8_t>(newVelocity * 127);
                    // Reconstruct the MIDI message with new velocity
                    event.message = MidiMessage(MidiMessageType::NoteOn,
                                               event.message.getChannel(),
                                               event.message.getNoteNumber(), 
                                               newVel);
                    markDirty();
                }
            }
        }
    }
    
    // Also draw velocity for recording clip if present
    if (track.recordingClip) {
        auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(track.recordingClip->getEvents());
        float clipStartBeat = static_cast<float>(track.recordingClip->getStartTime()) / (44100.0f * 60.0f / bpm_);
        
        for (size_t i = 0; i < events.size(); ++i) {
            auto& event = events[i];
            if (!event.message.isNoteOn()) continue;
            
            float noteStartBeat = clipStartBeat + event.timestamp;
            float x = canvasX + (noteStartBeat - scrollX) * pixelsPerBeat;
            
            if (x < canvasX - 20 || x > canvasX + canvasWidth + 20) continue;
            
            float velocity = event.message.getVelocity() / 127.0f;
            float barHeight = velocity * (canvasHeight - 10.0f);
            float barY = canvasY + canvasHeight - barHeight - 5.0f;
            float barWidth = 6.0f;
            
            // Recording clip notes shown in a different color
            drawList->AddRectFilled(
                ImVec2(x - barWidth/2, barY),
                ImVec2(x + barWidth/2, canvasY + canvasHeight - 5.0f),
                IM_COL32(255, 100, 100, 200)
            );
            drawList->AddCircleFilled(ImVec2(x, barY), 4.0f, IM_COL32(255, 100, 100, 200));
        }
    }
    
    // Draw mode: click and drag to set velocities
    if (pencilToolActive_) {
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= canvasX && mousePos.x <= canvasX + canvasWidth &&
            mousePos.y >= canvasY && mousePos.y <= canvasY + canvasHeight) {
            
            if (ImGui::IsMouseDown(0)) {
                // Calculate velocity from mouse Y position
                float targetVelocity = 1.0f - (mousePos.y - canvasY - 5.0f) / (canvasHeight - 10.0f);
                targetVelocity = std::clamp(targetVelocity, 0.0f, 1.0f);
                uint8_t targetVel = static_cast<uint8_t>(targetVelocity * 127);
                
                // Find notes near this beat position and set their velocity
                for (auto& clip : track.clips) {
                    if (!clip) continue;
                    auto& events = const_cast<std::vector<MidiClip::MidiEvent>&>(clip->getEvents());
                    float clipStartBeat = static_cast<float>(clip->getStartTime()) / (44100.0f * 60.0f / bpm_);
                    
                    for (auto& event : events) {
                        if (!event.message.isNoteOn()) continue;
                        
                        // Check if note is within a small threshold of click position
                        float noteBeat = clipStartBeat + event.timestamp;
                        float noteX = canvasX + (noteBeat - scrollX) * pixelsPerBeat;
                        
                        if (std::abs(mousePos.x - noteX) < 10.0f) {
                            event.message = MidiMessage(MidiMessageType::NoteOn,
                                                       event.message.getChannel(),
                                                       event.message.getNoteNumber(),
                                                       targetVel);
                            markDirty();
                        }
                    }
                }
            }
        }
    }
    
    // Make the velocity area interactive
    ImGui::SetCursorScreenPos(ImVec2(canvasX - pianoKeyWidth, canvasY));
    ImGui::InvisibleButton("velocity_canvas", ImVec2(canvasWidth + pianoKeyWidth, canvasHeight));
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

void MainWindow::initializeInstrumentPresets() {
    // Clear any existing presets
    instrumentPresets_.clear();
    
    // === SYNTH CATEGORY ===
    
    // Supersaw - Multiple detuned sawtooth oscillators (subtractive)
    instrumentPresets_.push_back(InstrumentPreset(
        "Supersaw",
        "Synth",
        {
            Oscillator(Waveform::Sawtooth, 1.0f, 0.4f),
            Oscillator(Waveform::Sawtooth, 0.995f, 0.3f),
            Oscillator(Waveform::Sawtooth, 1.005f, 0.3f),
            Oscillator(Waveform::Sawtooth, 0.99f, 0.2f),
            Oscillator(Waveform::Sawtooth, 1.01f, 0.2f)
        }
    ));
    
    // Hollow Pad - Odd harmonics only create "hollow" nasal character
    {
        InstrumentEnvelope envHollowPad;
        envHollowPad.ampEnvelope = ADSREnvelope(0.5f, 0.3f, 0.75f, 1.8f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Hollow Pad",
            "Pad",
            {
                Oscillator(Waveform::Square, 1.0f, 0.5f),
                Oscillator(Waveform::Sine, 3.0f, 0.25f),
                Oscillator(Waveform::Sine, 5.0f, 0.15f),
                Oscillator(Waveform::Sine, 7.0f, 0.1f)
            },
            envHollowPad
        ));
    }
    
    // Ring Mod Lead - Inharmonic bell-like (multiplicative/ring modulation)
    instrumentPresets_.push_back(InstrumentPreset(
        "Bell Lead",
        "Synth",
        {
            Oscillator(Waveform::Sine, 1.0f, 0.5f),          // Fundamental
            Oscillator(Waveform::Sine, 2.76f, 0.3f),         // Inharmonic ratio
            Oscillator(Waveform::Sine, 5.4f, 0.2f),          // Metallic overtone
            Oscillator(Waveform::Triangle, 8.93f, 0.12f)     // Bell-like shimmer
        }
    ));
    
    // Deep Bass - Floor-shaking sub-bass (-1 octave from default)
    // Plays an octave lower than MIDI note for maximum sub impact
    instrumentPresets_.push_back(InstrumentPreset(
        "Deep Bass",
        "Bass",
        {
            // Sub-bass core: -2 octaves (0.25x)
            Oscillator(Waveform::Sine, 0.25f, 1.0f),
            // Deep sub: -3 octaves for extreme rumble
            Oscillator(Waveform::Sine, 0.125f, 0.7f),
            // Low bass body: -1 octave
            Oscillator(Waveform::Sine, 0.5f, 0.3f),
            // Harmonic clarity: fundamental
            Oscillator(Waveform::Sine, 1.0f, 0.15f)
        }
    ));
    
    // Harsh Lead - Complex ratios (multiplicative/FM-style)
    instrumentPresets_.push_back(InstrumentPreset(
        "Harsh Lead",
        "Synth",
        {
            Oscillator(Waveform::Sawtooth, 1.0f, 0.55f),     // Carrier
            Oscillator(Waveform::Square, 1.618f, 0.4f),      // Golden ratio (FM)
            Oscillator(Waveform::Square, 2.333f, 0.3f),      // 7/3 ratio
            Oscillator(Waveform::Sawtooth, 4.0f, 0.15f)      // Brightness
        }
    ));
    
    // === PIANO CATEGORY ===
    
    // Bright Piano - Harmonic series with percussive envelope
    {
        InstrumentEnvelope envBrightPiano;
        envBrightPiano.ampEnvelope = ADSREnvelope(0.005f, 1.5f, 0.3f, 0.8f);  // Fast attack, long decay
        instrumentPresets_.push_back(InstrumentPreset(
            "Bright Piano",
            "Piano",
            {
                Oscillator(Waveform::Triangle, 1.0f, 0.5f),
                Oscillator(Waveform::Sine, 2.0f, 0.3f),
                Oscillator(Waveform::Sine, 3.0f, 0.15f),
                Oscillator(Waveform::Sine, 4.0f, 0.1f),
                Oscillator(Waveform::Sine, 5.0f, 0.05f)
            },
            envBrightPiano
        ));
    }
    
    // Electric Piano - Rhodes-style with bell-like envelope
    {
        InstrumentEnvelope envEPiano;
        envEPiano.ampEnvelope = ADSREnvelope(0.001f, 2.0f, 0.2f, 1.0f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Electric Piano",
            "Piano",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.6f),
                Oscillator(Waveform::Sine, 1.414f, 0.3f),
                Oscillator(Waveform::Triangle, 2.0f, 0.2f),
                Oscillator(Waveform::Sine, 3.732f, 0.15f)
            },
            envEPiano
        ));
    }
    
    // Dark Piano - Lower harmonics with warm envelope
    {
        InstrumentEnvelope envDarkPiano;
        envDarkPiano.ampEnvelope = ADSREnvelope(0.01f, 2.5f, 0.25f, 1.2f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Dark Piano",
            "Piano",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.8f),
                Oscillator(Waveform::Triangle, 1.0f, 0.3f),
                Oscillator(Waveform::Sine, 2.0f, 0.2f),
                Oscillator(Waveform::Sine, 0.5f, 0.15f)
            },
            envDarkPiano
        ));
    }
    
    // === BASS CATEGORY ===
    
    // 808 Sub Bass - The classic TR-808 style with pitch drop
    // IMPORTANT: Play this at low notes (C1-C2, MIDI 24-36) for proper sub bass
    // The 808 is characterized by: Pure sine, pitch drop on attack, long decay
    {
        InstrumentEnvelope env808;
        env808.ampEnvelope = ADSREnvelope(0.001f, 1.5f, 0.0f, 0.8f);  // Long decay for that sustained boom
        env808.pitchEnvelope = PitchEnvelope(2.0f, 0.08f);  // Start 1 octave up, drop over 80ms
        // Add a gentle low-pass to smooth it out
        env808.filter.enabled = true;
        env808.filter.cutoff = 0.3f;  // ~200Hz cutoff
        env808.filter.resonance = 0.1f;
        instrumentPresets_.push_back(InstrumentPreset(
            "808 Sub",
            "Bass",
            {
                // Main 808 tone - at fundamental frequency
                // When you play C2 (65Hz), this gives you 65Hz - classic 808 territory
                Oscillator(Waveform::Sine, 1.0f, 1.0f),
                // Sub layer one octave down for weight
                Oscillator(Waveform::Sine, 0.5f, 0.7f),
            },
            env808
        ));
    }
    
    // === FLYING LOTUS INSPIRED SET ===
    {
        // Cosmic Pad (lush detuned triangle/saw stack with slow attack)
        InstrumentEnvelope envPad;
        envPad.ampEnvelope = ADSREnvelope(1.5f, 2.5f, 0.8f, 2.5f);
        envPad.filter.enabled = true;
        envPad.filter.cutoff = 0.4f;
        envPad.filter.resonance = 0.15f;
        envPad.unison.enabled = true;
        envPad.unison.voices = 3;
        envPad.unison.detune = 18.0f;
        envPad.unison.spread = 0.7f;
        instrumentPresets_.push_back(InstrumentPreset(
            "Cosmic Pad",
            "Pad",
            {
                Oscillator(Waveform::Triangle, 1.0f, 0.5f),
                Oscillator(Waveform::Sawtooth, 0.995f, 0.25f),
                Oscillator(Waveform::Sawtooth, 1.006f, 0.25f),
                Oscillator(Waveform::Sine, 2.0f, 0.1f)
            },
            envPad
        ));
    }
    {
        // Sub Wobble Bass (mono, glide, LFO-filter friendly)
        InstrumentEnvelope envWobble;
        envWobble.ampEnvelope = ADSREnvelope(0.01f, 0.25f, 0.7f, 0.4f);
        envWobble.filter.enabled = true;
        envWobble.filter.cutoff = 0.25f;
        envWobble.filter.resonance = 0.25f;
        envWobble.portamento.enabled = true;
        envWobble.portamento.time = 0.12f;
        envWobble.portamento.legato = true;
        envWobble.saturation.enabled = true;
        envWobble.saturation.drive = 3.0f;
        envWobble.saturation.mix = 0.45f;
        instrumentPresets_.push_back(InstrumentPreset(
            "Sub Wobble",
            "Bass",
            {
                Oscillator(Waveform::Sine, 0.5f, 0.9f),
                Oscillator(Waveform::Square, 1.0f, 0.2f),
                Oscillator(Waveform::Triangle, 2.0f, 0.15f)
            },
            envWobble
        ));
    }
    {
        // Dusty Keys (lofi keys with slight detune)
        InstrumentEnvelope envKeys;
        envKeys.ampEnvelope = ADSREnvelope(0.01f, 0.6f, 0.5f, 0.8f);
        envKeys.filter.enabled = true;
        envKeys.filter.cutoff = 0.55f;
        envKeys.filter.resonance = 0.1f;
        instrumentPresets_.push_back(InstrumentPreset(
            "Dusty Keys",
            "Keys",
            {
                Oscillator(Waveform::Triangle, 1.0f, 0.6f),
                Oscillator(Waveform::Sine, 2.0f, 0.25f),
                Oscillator(Waveform::Square, 0.995f, 0.2f),
                Oscillator(Waveform::Square, 1.006f, 0.2f)
            },
            envKeys
        ));
    }
    
    // 808 Boom - More aggressive attack, shorter decay
    {
        InstrumentEnvelope env808Boom;
        env808Boom.ampEnvelope = ADSREnvelope(0.0005f, 0.6f, 0.0f, 0.3f);
        env808Boom.pitchEnvelope = PitchEnvelope(3.0f, 0.05f);  // Higher pitch start, faster drop
        instrumentPresets_.push_back(InstrumentPreset(
            "808 Boom",
            "Bass",
            {
                Oscillator(Waveform::Sine, 1.0f, 1.0f),
                Oscillator(Waveform::Sine, 0.5f, 0.5f),
            },
            env808Boom
        ));
    }
    
    // 808 Long - Extended sustain for melodic bass lines
    {
        InstrumentEnvelope env808Long;
        env808Long.ampEnvelope = ADSREnvelope(0.001f, 0.5f, 0.7f, 1.5f);  // Has sustain for holding notes
        env808Long.pitchEnvelope = PitchEnvelope(1.5f, 0.1f);  // Subtle pitch drop
        env808Long.filter.enabled = true;
        env808Long.filter.cutoff = 0.4f;
        instrumentPresets_.push_back(InstrumentPreset(
            "808 Long",
            "Bass",
            {
                Oscillator(Waveform::Sine, 1.0f, 1.0f),
                Oscillator(Waveform::Sine, 0.5f, 0.6f),
                Oscillator(Waveform::Sine, 2.0f, 0.1f),  // Subtle octave up for presence
            },
            env808Long
        ));
    }
    
    // 808 Distorted - With harmonics from triangle wave
    {
        InstrumentEnvelope env808Dist;
        env808Dist.ampEnvelope = ADSREnvelope(0.0005f, 0.8f, 0.0f, 0.4f);
        env808Dist.pitchEnvelope = PitchEnvelope(2.5f, 0.06f);
        instrumentPresets_.push_back(InstrumentPreset(
            "808 Dirty",
            "Bass",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.8f),
                Oscillator(Waveform::Triangle, 1.0f, 0.3f),  // Adds odd harmonics
                Oscillator(Waveform::Sine, 0.5f, 0.5f),
            },
            env808Dist
        ));
    }
    
    // Reese Bass - Detuned unison with proper envelope
    {
        InstrumentEnvelope envReese;
        envReese.ampEnvelope = ADSREnvelope(0.05f, 0.2f, 0.8f, 0.4f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Reese Bass",
            "Bass",
            {
                Oscillator(Waveform::Sawtooth, 0.5f, 0.4f, 0.0f),
                Oscillator(Waveform::Sawtooth, 0.5f, 0.4f, -15.0f),   // Detuned -15 cents
                Oscillator(Waveform::Sawtooth, 0.5f, 0.4f, +15.0f),   // Detuned +15 cents
                Oscillator(Waveform::Sine, 0.25f, 0.4f)               // Sub layer
            },
            envReese
        ));
    }
    
    // FM Bass - Classic DX7 style with snappy envelope
    {
        InstrumentEnvelope envFM;
        envFM.ampEnvelope = ADSREnvelope(0.001f, 0.15f, 0.5f, 0.2f);
        instrumentPresets_.push_back(InstrumentPreset(
            "FM Bass",
            "Bass",
            {
                Oscillator(Waveform::Sine, 0.5f, 0.6f),
                Oscillator(Waveform::Sine, 1.0f, 0.35f),
                Oscillator(Waveform::Sine, 1.75f, 0.2f),
                Oscillator(Waveform::Sine, 0.25f, 0.25f)
            },
            envFM
        ));
    }
    
    // Acid Bass - TB-303 style with envelope
    {
        InstrumentEnvelope envAcid;
        envAcid.ampEnvelope = ADSREnvelope(0.001f, 0.3f, 0.4f, 0.15f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Acid Bass",
            "Bass",
            {
                Oscillator(Waveform::Square, 0.5f, 0.5f),
                Oscillator(Waveform::Sawtooth, 0.5f, 0.3f),
                Oscillator(Waveform::Square, 0.25f, 0.2f)
            },
            envAcid
        ));
    }
    
    // === PAD CATEGORY ===
    
    // Warm Pad - Soft even harmonics with slow envelope
    {
        InstrumentEnvelope envWarmPad;
        envWarmPad.ampEnvelope = ADSREnvelope(0.8f, 0.3f, 0.7f, 1.5f);  // Slow attack/release
        instrumentPresets_.push_back(InstrumentPreset(
            "Warm Pad",
            "Pad",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.6f),
                Oscillator(Waveform::Triangle, 1.0f, 0.3f),
                Oscillator(Waveform::Sine, 2.0f, 0.2f),
                Oscillator(Waveform::Sine, 0.5f, 0.15f)
            },
            envWarmPad
        ));
    }
    
    // String Pad - Rich harmonics with slow envelope
    {
        InstrumentEnvelope envStringPad;
        envStringPad.ampEnvelope = ADSREnvelope(1.0f, 0.5f, 0.8f, 2.0f);
        instrumentPresets_.push_back(InstrumentPreset(
            "String Pad",
            "Pad",
            {
                Oscillator(Waveform::Sawtooth, 1.0f, 0.3f, -5.0f),
                Oscillator(Waveform::Sawtooth, 1.0f, 0.3f, +5.0f),
                Oscillator(Waveform::Sawtooth, 1.0f, 0.3f),
                Oscillator(Waveform::Sine, 2.0f, 0.1f)
            },
            envStringPad
        ));
    }
    
    // Glass Pad - Ethereal with shimmer
    {
        InstrumentEnvelope envGlassPad;
        envGlassPad.ampEnvelope = ADSREnvelope(0.6f, 0.4f, 0.75f, 2.5f);
        envGlassPad.lfo1 = LFO(0.2f, 0.1f, LFO::Target::Pitch);  // Gentle shimmer
        instrumentPresets_.push_back(InstrumentPreset(
            "Glass Pad",
            "Pad",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.5f),
                Oscillator(Waveform::Sine, 3.0f, 0.25f),
                Oscillator(Waveform::Sine, 5.0f, 0.15f),
                Oscillator(Waveform::Triangle, 7.0f, 0.08f)
            },
            envGlassPad
        ));
    }
    
    // === LEAD CATEGORY ===
    
    // Square Lead - Classic 8-bit with quick attack
    {
        InstrumentEnvelope envSquareLead;
        envSquareLead.ampEnvelope = ADSREnvelope(0.02f, 0.1f, 0.8f, 0.2f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Square Lead",
            "Lead",
            {
                Oscillator(Waveform::Square, 1.0f, 0.6f),
                Oscillator(Waveform::Square, 2.0f, 0.2f)
            },
            envSquareLead
        ));
    }
    
    // Sync Lead - Bright and cutting
    {
        InstrumentEnvelope envSyncLead;
        envSyncLead.ampEnvelope = ADSREnvelope(0.01f, 0.15f, 0.7f, 0.15f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Sync Lead",
            "Lead",
            {
                Oscillator(Waveform::Sawtooth, 1.0f, 0.5f),
                Oscillator(Waveform::Sawtooth, 2.0f, 0.4f),
                Oscillator(Waveform::Sawtooth, 3.0f, 0.2f)
            },
            envSyncLead
        ));
    }
    
    // PWM Lead - Pulse width mod simulation with slow attack
    {
        InstrumentEnvelope envPWMLead;
        envPWMLead.ampEnvelope = ADSREnvelope(0.1f, 0.2f, 0.8f, 0.3f);
        envPWMLead.lfo1 = LFO(0.5f, 0.05f, LFO::Target::Pitch);  // Subtle vibrato
        instrumentPresets_.push_back(InstrumentPreset(
            "PWM Lead",
            "Lead",
            {
                Oscillator(Waveform::Square, 1.0f, 0.4f, -10.0f),
                Oscillator(Waveform::Square, 1.0f, 0.3f, +10.0f),
                Oscillator(Waveform::Square, 1.0f, 0.3f),
                Oscillator(Waveform::Sine, 2.0f, 0.1f)
            },
            envPWMLead
        ));
    }
    
    // === KEYS CATEGORY ===
    
    // Organ - Drawbar style (instant on/off like real organs)
    {
        InstrumentEnvelope envOrgan;
        envOrgan.ampEnvelope = ADSREnvelope(0.005f, 0.01f, 1.0f, 0.1f);  // Very fast, full sustain
        instrumentPresets_.push_back(InstrumentPreset(
            "Organ",
            "Keys",
            {
                Oscillator(Waveform::Sine, 0.5f, 0.3f),
                Oscillator(Waveform::Sine, 1.0f, 0.5f),
                Oscillator(Waveform::Sine, 2.0f, 0.4f),
                Oscillator(Waveform::Sine, 3.0f, 0.25f),
                Oscillator(Waveform::Sine, 4.0f, 0.15f)
            },
            envOrgan
        ));
    }
    
    // Clavinet - Funky keys with snappy envelope
    {
        InstrumentEnvelope envClav;
        envClav.ampEnvelope = ADSREnvelope(0.001f, 0.5f, 0.4f, 0.2f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Clavinet",
            "Keys",
            {
                Oscillator(Waveform::Square, 1.0f, 0.5f),
                Oscillator(Waveform::Sawtooth, 1.0f, 0.3f),
                Oscillator(Waveform::Square, 2.0f, 0.2f),
                Oscillator(Waveform::Sawtooth, 4.0f, 0.1f)
            },
            envClav
        ));
    }
    
    // Harpsichord - Bright pluck with fast decay
    {
        InstrumentEnvelope envHarpsi;
        envHarpsi.ampEnvelope = ADSREnvelope(0.001f, 0.8f, 0.0f, 0.3f);  // No sustain
        instrumentPresets_.push_back(InstrumentPreset(
            "Harpsichord",
            "Keys",
            {
                Oscillator(Waveform::Sawtooth, 1.0f, 0.4f),
                Oscillator(Waveform::Sawtooth, 2.0f, 0.35f),
                Oscillator(Waveform::Triangle, 3.0f, 0.2f),
                Oscillator(Waveform::Sawtooth, 4.0f, 0.15f)
            },
            envHarpsi
        ));
    }
    
    // === PLUCK CATEGORY ===
    
    // Pluck - Karplus-Strong style with fast decay
    {
        InstrumentEnvelope envPluck;
        envPluck.ampEnvelope = ADSREnvelope(0.001f, 0.6f, 0.0f, 0.3f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Pluck",
            "Pluck",
            {
                Oscillator(Waveform::Triangle, 1.0f, 0.6f),
                Oscillator(Waveform::Sine, 2.0f, 0.25f),
                Oscillator(Waveform::Sine, 3.0f, 0.15f)
            },
            envPluck
        ));
    }
    
    // Marimba - Bell-like decay
    {
        InstrumentEnvelope envMarimba;
        envMarimba.ampEnvelope = ADSREnvelope(0.001f, 1.2f, 0.0f, 0.5f);
        instrumentPresets_.push_back(InstrumentPreset(
            "Marimba",
            "Pluck",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.7f),
                Oscillator(Waveform::Sine, 4.0f, 0.35f),
                Oscillator(Waveform::Sine, 9.2f, 0.12f)
            },
            envMarimba
        ));
    }
    
    // Kalimba - Thumb piano
    {
        InstrumentEnvelope envKalimba;
        envKalimba.ampEnvelope = ADSREnvelope(0.001f, 0.4f, 0.0f, 0.3f);  // Plucky, no sustain
        instrumentPresets_.push_back(InstrumentPreset(
            "Kalimba",
            "Pluck",
            {
                Oscillator(Waveform::Sine, 1.0f, 0.6f),
                Oscillator(Waveform::Sine, 3.0f, 0.3f),
                Oscillator(Waveform::Sine, 5.0f, 0.15f),
                Oscillator(Waveform::Triangle, 7.0f, 0.08f)
            },
            envKalimba
        ));
    }
    
    // === ATMOSPHERE CATEGORY ===
    // Evolving pads inspired by Hyper Light Drifter's ambient soundtrack
    // These change and morph when holding keys
    
    // Drifter Pad - Slow evolving ethereal texture
    {
        InstrumentEnvelope envDrifter;
        envDrifter.ampEnvelope = ADSREnvelope(2.5f, 1.0f, 0.7f, 3.0f);  // Very slow attack/release
        envDrifter.lfo1 = LFO(0.1f, 0.3f, LFO::Target::Pitch);   // Slow pitch wobble
        envDrifter.lfo2 = LFO(0.07f, 0.4f, LFO::Target::Amplitude);  // Amplitude swell
        instrumentPresets_.push_back(InstrumentPreset(
            "Drifter",
            "Atmosphere",
            {
                // Detuned unison for width
                Oscillator(Waveform::Sine, 1.0f, 0.4f, -8.0f),
                Oscillator(Waveform::Sine, 1.0f, 0.4f, +8.0f),
                // Fifth harmony
                Oscillator(Waveform::Triangle, 1.5f, 0.25f, -5.0f),
                // Octave shimmer
                Oscillator(Waveform::Sine, 2.0f, 0.15f, +12.0f),
                // Sub foundation
                Oscillator(Waveform::Sine, 0.5f, 0.3f)
            },
            envDrifter
        ));
    }
    
    // Crystalline - Glassy, evolving harmonics
    {
        InstrumentEnvelope envCrystal;
        envCrystal.ampEnvelope = ADSREnvelope(1.5f, 0.5f, 0.8f, 4.0f);
        envCrystal.lfo1 = LFO(0.15f, 0.2f, LFO::Target::Pitch);
        envCrystal.lfo2 = LFO(0.23f, 0.3f, LFO::Target::Amplitude);
        instrumentPresets_.push_back(InstrumentPreset(
            "Crystalline",
            "Atmosphere",
            {
                Oscillator(Waveform::Triangle, 1.0f, 0.5f),
                Oscillator(Waveform::Sine, 2.0f, 0.3f, +7.0f),
                Oscillator(Waveform::Sine, 3.0f, 0.2f, -7.0f),
                Oscillator(Waveform::Sine, 4.0f, 0.15f, +14.0f),
                Oscillator(Waveform::Sine, 5.0f, 0.1f)
            },
            envCrystal
        ));
    }
    
    // Void - Dark, rumbling atmosphere
    {
        InstrumentEnvelope envVoid;
        envVoid.ampEnvelope = ADSREnvelope(3.0f, 2.0f, 0.6f, 5.0f);
        envVoid.lfo1 = LFO(0.05f, 0.15f, LFO::Target::Pitch);  // Very slow drift
        envVoid.lfo2 = LFO(0.12f, 0.5f, LFO::Target::Amplitude);  // Breathing
        instrumentPresets_.push_back(InstrumentPreset(
            "Void",
            "Atmosphere",
            {
                // Deep sub drones
                Oscillator(Waveform::Sine, 0.25f, 0.6f),
                Oscillator(Waveform::Sine, 0.5f, 0.5f, -10.0f),
                Oscillator(Waveform::Triangle, 0.5f, 0.3f, +10.0f),
                // Distant harmonic
                Oscillator(Waveform::Sine, 1.0f, 0.2f),
                // Dark overtone
                Oscillator(Waveform::Triangle, 1.5f, 0.1f)
            },
            envVoid
        ));
    }
    
    // Northern Lights - Shimmering, otherworldly
    {
        InstrumentEnvelope envAurora;
        envAurora.ampEnvelope = ADSREnvelope(2.0f, 1.5f, 0.75f, 4.0f);
        envAurora.lfo1 = LFO(0.08f, 0.25f, LFO::Target::Pitch);
        envAurora.lfo2 = LFO(0.19f, 0.35f, LFO::Target::Amplitude);
        instrumentPresets_.push_back(InstrumentPreset(
            "Aurora",
            "Atmosphere",
            {
                // Wide stereo foundation
                Oscillator(Waveform::Sine, 1.0f, 0.35f, -15.0f),
                Oscillator(Waveform::Sine, 1.0f, 0.35f, +15.0f),
                // Perfect fifth shimmer
                Oscillator(Waveform::Triangle, 1.498f, 0.25f),
                Oscillator(Waveform::Triangle, 1.502f, 0.25f),
                // High harmonic sparkle
                Oscillator(Waveform::Sine, 3.0f, 0.12f, +20.0f),
                Oscillator(Waveform::Sine, 4.0f, 0.08f, -20.0f)
            },
            envAurora
        ));
    }
    
    // Ancient Temple - Mysterious, reverberant
    {
        InstrumentEnvelope envTemple;
        envTemple.ampEnvelope = ADSREnvelope(1.8f, 1.0f, 0.7f, 6.0f);  // Very long release
        envTemple.lfo1 = LFO(0.03f, 0.1f, LFO::Target::Pitch);  // Barely perceptible drift
        envTemple.lfo2 = LFO(0.09f, 0.4f, LFO::Target::Amplitude);
        instrumentPresets_.push_back(InstrumentPreset(
            "Ancient Temple",
            "Atmosphere",
            {
                // Fundamental drone
                Oscillator(Waveform::Sine, 1.0f, 0.5f),
                // Perfect fifth (sacred interval)
                Oscillator(Waveform::Sine, 1.5f, 0.35f, -6.0f),
                Oscillator(Waveform::Sine, 1.5f, 0.35f, +6.0f),
                // Octave
                Oscillator(Waveform::Triangle, 2.0f, 0.2f),
                // Sub presence
                Oscillator(Waveform::Sine, 0.5f, 0.3f)
            },
            envTemple
        ));
    }
    
    // Digital Rain - Glitchy, evolving texture
    {
        InstrumentEnvelope envRain;
        envRain.ampEnvelope = ADSREnvelope(0.8f, 0.5f, 0.65f, 2.5f);
        envRain.lfo1 = LFO(0.33f, 0.15f, LFO::Target::Pitch);  // Faster modulation
        envRain.lfo2 = LFO(0.17f, 0.45f, LFO::Target::Amplitude);
        instrumentPresets_.push_back(InstrumentPreset(
            "Digital Rain",
            "Atmosphere",
            {
                Oscillator(Waveform::Square, 1.0f, 0.25f, -20.0f),
                Oscillator(Waveform::Square, 1.0f, 0.25f, +20.0f),
                Oscillator(Waveform::Sine, 2.0f, 0.3f),
                Oscillator(Waveform::Triangle, 3.0f, 0.15f, +10.0f),
                Oscillator(Waveform::Sine, 0.5f, 0.2f)
            },
            envRain
        ));
    }
    
    // Distant Memory - Nostalgic, warm evolving pad
    {
        InstrumentEnvelope envMemory;
        envMemory.ampEnvelope = ADSREnvelope(3.5f, 2.0f, 0.5f, 5.0f);
        envMemory.lfo1 = LFO(0.06f, 0.2f, LFO::Target::Pitch);
        envMemory.lfo2 = LFO(0.11f, 0.5f, LFO::Target::Amplitude);
        instrumentPresets_.push_back(InstrumentPreset(
            "Distant Memory",
            "Atmosphere",
            {
                // Warm detuned core
                Oscillator(Waveform::Triangle, 1.0f, 0.4f, -12.0f),
                Oscillator(Waveform::Triangle, 1.0f, 0.4f, +12.0f),
                // Gentle harmonics
                Oscillator(Waveform::Sine, 2.0f, 0.25f),
                Oscillator(Waveform::Sine, 3.0f, 0.1f),
                // Sub warmth
                Oscillator(Waveform::Sine, 0.5f, 0.35f)
            },
            envMemory
        ));
    }
    
    std::cout << "Initialized " << instrumentPresets_.size() << " instrument presets" << std::endl;
}

void MainWindow::saveUserPreset(const std::string& name, const std::vector<Oscillator>& oscillators) {
    // Add to user presets
    userPresets_.push_back(InstrumentPreset(name, "User", oscillators));
    
    // Save to file
    saveUserPresetsToFile();
    
    std::cout << "Saved user preset: " << name << std::endl;
}

void MainWindow::loadUserPresets() {
    userPresets_.clear();
    
    // Try to load from file
    std::ifstream file("user_presets.pan");
    if (!file.is_open()) {
        std::cout << "No user presets file found (this is normal on first run)" << std::endl;
        return;
    }
    
    try {
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty()) continue;
            
            // Format: name|numOscillators
            size_t pipePos = line.find('|');
            if (pipePos == std::string::npos) continue;
            
            std::string name = line.substr(0, pipePos);
            int numOscs = std::stoi(line.substr(pipePos + 1));
            
            std::vector<Oscillator> oscillators;
            for (int i = 0; i < numOscs; ++i) {
                if (!std::getline(file, line)) break;
                
                // Format: waveform,freqMult,amplitude
                std::istringstream oscStream(line);
                std::string waveStr, freqStr, ampStr;
                
                std::getline(oscStream, waveStr, ',');
                std::getline(oscStream, freqStr, ',');
                std::getline(oscStream, ampStr);
                
                Waveform wave = static_cast<Waveform>(std::stoi(waveStr));
                float freq = std::stof(freqStr);
                float amp = std::stof(ampStr);
                
                oscillators.push_back(Oscillator(wave, freq, amp));
            }
            
            userPresets_.push_back(InstrumentPreset(name, "User", oscillators));
        }
        
        std::cout << "Loaded " << userPresets_.size() << " user presets" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading user presets: " << e.what() << std::endl;
        userPresets_.clear();
    }
    
    file.close();
}

void MainWindow::saveUserPresetsToFile() {
    std::ofstream file("user_presets.pan");
    if (!file.is_open()) {
        std::cerr << "Failed to save user presets file" << std::endl;
        return;
    }
    
    for (const auto& preset : userPresets_) {
        // Write preset name and oscillator count
        file << preset.name << "|" << preset.oscillators.size() << "\n";
        
        // Write each oscillator
        for (const auto& osc : preset.oscillators) {
            file << static_cast<int>(osc.waveform) << ","
                 << osc.frequencyMultiplier << ","
                 << osc.amplitude << "\n";
        }
    }
    
    file.close();
    std::cout << "Saved " << userPresets_.size() << " user presets to file" << std::endl;
}

void MainWindow::loadSamplesFromDirectory() {
    userSamples_.clear();
    
    // Create samples directory if it doesn't exist
    std::filesystem::create_directories("samples");
    
    // Scan for WAV and MP3 files
    try {
        for (const auto& entry : std::filesystem::directory_iterator("samples")) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                
                if (ext == ".wav" || ext == ".mp3") {
                    SampleInfo info;
                    info.path = entry.path().string();
                    info.name = entry.path().stem().string();
                    
                    // Load waveform preview using a temporary Sampler
                    Sampler tempSampler(44100.0);
                    if (tempSampler.loadSample(info.path)) {
                        const Sample* sample = tempSampler.getSample();
                        if (sample) {
                            info.waveformDisplay = sample->waveformDisplay;
                        }
                    }
                    
                    userSamples_.push_back(info);
                }
            }
        }
        
        std::cout << "Loaded " << userSamples_.size() << " samples from samples/" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error scanning samples directory: " << e.what() << std::endl;
    }
}

void MainWindow::refreshSampleList() {
    loadSamplesFromDirectory();
}

bool MainWindow::importSample(const std::string& sourcePath) {
    // Create samples directory if it doesn't exist
    std::filesystem::create_directories("samples");
    
    // Get filename from source path
    std::filesystem::path srcPath(sourcePath);
    std::string filename = srcPath.filename().string();
    std::filesystem::path destPath = std::filesystem::path("samples") / filename;
    
    // Check if file already exists
    if (std::filesystem::exists(destPath)) {
        std::cout << "Sample already exists: " << filename << std::endl;
        return true;  // Already imported
    }
    
    // Copy the file
    try {
        std::filesystem::copy_file(srcPath, destPath);
        std::cout << "Imported sample: " << filename << std::endl;
        
        // Refresh the sample list
        refreshSampleList();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Failed to import sample: " << e.what() << std::endl;
        return false;
    }
}

} // namespace pan




