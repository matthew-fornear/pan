#include <iostream>
#include "pan/audio/audio_engine.h"
#include "pan/midi/midi_input.h"
#include "pan/midi/synthesizer.h"
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <csignal>

#ifdef PAN_USE_PORTAUDIO
#include <portaudio.h>
#endif

// ImGui includes
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << std::endl;
}

// Track structure
struct Track {
    pan::Waveform waveform;
    bool isRecording;
    std::shared_ptr<pan::Synthesizer> synth;
    std::string name;
    
    Track() : waveform(pan::Waveform::Sine), isRecording(false), name("") {}
};

std::atomic<bool> shouldQuit{false};
pan::AudioEngine* g_engine = nullptr;
pan::MidiInput* g_midiInput = nullptr;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    shouldQuit = true;
    if (g_midiInput) g_midiInput->stop();
    if (g_engine) {
        g_engine->stop();
        g_engine->shutdown();
    }
    std::exit(0);
}

int main() {
    // Set up signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGHUP, signalHandler);
    std::signal(SIGTERM, signalHandler);
    
    std::cout << "Pan Synthesizer GUI" << std::endl;
    std::cout << "Connect your MIDI keyboard and play notes!" << std::endl;
    
    // Setup GLFW
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return 1;
    }
    
    // GL 3.3 + GLSL 330
    const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Get primary monitor for fullscreen
    GLFWmonitor* primaryMonitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(primaryMonitor);
    
    // Create window - fullscreen or use monitor resolution
    GLFWwindow* window = glfwCreateWindow(mode->width, mode->height, "Pan Synthesizer", nullptr, nullptr);
    if (!window) {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return 1;
    }
    
    // Center window (or make fullscreen)
    glfwSetWindowPos(window, 0, 0);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync
    
    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;  // Enable docking for better layout
    
    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    
    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    
    // Initialize audio engine
    pan::AudioEngine engine;
    g_engine = &engine;
    
    if (!engine.initialize()) {
        std::cerr << "Failed to initialize audio engine" << std::endl;
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    
    // Create 5 tracks
    std::vector<Track> tracks(5);
    for (auto& track : tracks) {
        track.synth = std::make_shared<pan::Synthesizer>(engine.getSampleRate());
        track.synth->setVolume(0.5f);
    }
    
    // Current waveform selection for dragging
    std::atomic<pan::Waveform> draggedWaveform{pan::Waveform::Sine};
    bool isDragging = false;
    
    // Set up audio processing - mix all recording tracks
    engine.setProcessCallback([&](pan::AudioBuffer& input, pan::AudioBuffer& output, size_t numFrames) {
        output.clear();
        
        // Mix all recording tracks
        for (auto& track : tracks) {
            if (track.isRecording && track.synth) {
                pan::AudioBuffer trackBuffer(output.getNumChannels(), numFrames);
                track.synth->generateAudio(trackBuffer, numFrames);
                
                // Mix into output
                for (size_t ch = 0; ch < output.getNumChannels(); ++ch) {
                    const float* trackSamples = trackBuffer.getReadPointer(ch);
                    float* outputSamples = output.getWritePointer(ch);
                    for (size_t i = 0; i < numFrames; ++i) {
                        outputSamples[i] += trackSamples[i];
                    }
                }
            }
        }
    });
    
    // Start audio engine
    if (!engine.start()) {
        std::cerr << "Failed to start audio engine" << std::endl;
        engine.shutdown();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return 1;
    }
    
    // Setup MIDI input
    pan::MidiInput midiInput;
    g_midiInput = &midiInput;
    auto midiDevices = pan::MidiInput::enumerateDevices();
    
    if (!midiDevices.empty()) {
        std::cout << "Opening MIDI device: " << midiDevices[0] << std::endl;
        if (midiInput.openDevice(midiDevices[0])) {
            midiInput.setCallback([&tracks](const pan::MidiMessage& msg) {
                // Route MIDI to all recording tracks
                for (auto& track : tracks) {
                    if (track.isRecording && track.synth) {
                        track.synth->processMidiMessage(msg);
                    }
                }
            });
            midiInput.start();
            std::cout << "MIDI keyboard ready!" << std::endl;
        }
    } else {
        std::cout << "No MIDI devices found" << std::endl;
    }
    
    // Main loop
    while (!glfwWindowShouldClose(window) && !shouldQuit) {
        glfwPollEvents();
        
        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        
        // Full window dock space
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos);
        ImGui::SetNextWindowSize(viewport->Size);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        
        ImGui::Begin("DockSpace", nullptr, 
            ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
        
        ImGui::PopStyleVar(3);
        
        ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
        
        // Sample Library (Left 1/3)
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
        ImGui::Begin("Sample Library", nullptr, ImGuiWindowFlags_None);
        
        if (ImGui::CollapsingHeader("Basic Waves", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* waveNames[] = { "Sine", "Square", "Sawtooth", "Triangle" };
            pan::Waveform waveforms[] = { 
                pan::Waveform::Sine, 
                pan::Waveform::Square, 
                pan::Waveform::Sawtooth, 
                pan::Waveform::Triangle 
            };
            
            for (int i = 0; i < 4; ++i) {
                ImGui::PushID(i);
                
                // Make waveform draggable
                if (ImGui::Button(waveNames[i], ImVec2(-1, 0))) {
                    // Click to select
                }
                
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                    ImGui::SetDragDropPayload("WAVEFORM", &waveforms[i], sizeof(pan::Waveform));
                    ImGui::Text("Dragging %s", waveNames[i]);
                    ImGui::EndDragDropSource();
                }
                
                ImGui::PopID();
            }
        }
        
        ImGui::End();
        
        // Tracks (Middle 2/3)
        ImGui::SetNextWindowDockID(dockspace_id, ImGuiCond_FirstUseEver);
        ImGui::Begin("Tracks", nullptr, ImGuiWindowFlags_None);
        
        for (size_t i = 0; i < tracks.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            
            ImGui::BeginGroup();
            
            // Track header
            ImGui::Text("Track %zu", i + 1);
            ImGui::SameLine();
            
            // Record button (red when recording)
            if (tracks[i].isRecording) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
            }
            if (ImGui::Button("●", ImVec2(30, 30))) {
                tracks[i].isRecording = !tracks[i].isRecording;
            }
            if (tracks[i].isRecording) {
                ImGui::PopStyleColor(2);
            }
            ImGui::SameLine();
            
            // Waveform display/drop target
            ImGui::BeginChild(ImGui::GetID("track_content"), ImVec2(-1, 60), true);
            
            if (tracks[i].synth) {
                const char* currentWave = "";
                switch (tracks[i].waveform) {
                    case pan::Waveform::Sine: currentWave = "Sine"; break;
                    case pan::Waveform::Square: currentWave = "Square"; break;
                    case pan::Waveform::Sawtooth: currentWave = "Sawtooth"; break;
                    case pan::Waveform::Triangle: currentWave = "Triangle"; break;
                }
                
                if (tracks[i].waveform == pan::Waveform::Sine && tracks[i].name.empty()) {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Drop waveform here");
                } else {
                    ImGui::Text("Waveform: %s", currentWave);
                }
                
                // Volume control for this track
                float volume = tracks[i].synth->getVolume();
                if (ImGui::SliderFloat("Volume", &volume, 0.0f, 1.0f)) {
                    tracks[i].synth->setVolume(volume);
                }
            }
            
            // Drop target
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("WAVEFORM")) {
                    pan::Waveform droppedWave = *(pan::Waveform*)payload->Data;
                    tracks[i].waveform = droppedWave;
                    if (tracks[i].synth) {
                        tracks[i].synth->setWaveform(droppedWave);
                    }
                }
                ImGui::EndDragDropTarget();
            }
            
            ImGui::EndChild();
            ImGui::EndGroup();
            
            if (i < tracks.size() - 1) {
                ImGui::Separator();
            }
            
            ImGui::PopID();
        }
        
        ImGui::End();
        
        ImGui::End(); // DockSpace
        
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
    
    // Cleanup
    g_midiInput = nullptr;
    g_engine = nullptr;
    
    midiInput.stop();
    engine.stop();
    engine.shutdown();
    
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    
    glfwDestroyWindow(window);
    glfwTerminate();
    
    std::cout << "\nDone!" << std::endl;
    return 0;
}
