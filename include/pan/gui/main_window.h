#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <set>
#include <utility>
#include "pan/audio/audio_engine.h"
#include "pan/audio/effect.h"
#include "pan/audio/sampler.h"
#include "pan/midi/midi_input.h"
#include "pan/midi/synthesizer.h"
#include "pan/midi/midi_clip.h"

// Forward declaration for ImGui types
typedef unsigned int ImGuiID;

namespace pan {

struct Track {
    std::vector<Oscillator> oscillators;  // Multiple oscillators per track
    bool isRecording;
    bool isSolo;      // Solo button state
    bool isMuted;     // Mute button state
    std::shared_ptr<Synthesizer> synth;
    std::string name;
    bool waveformSet;  // Track if waveform has been explicitly set via drag-and-drop (deprecated, kept for compatibility)
    std::string instrumentName;  // Name of loaded instrument or wave (e.g., "Supersaw", "Sine")
    int colorIndex;   // Index into track color palette (0-15)
    float peakLevel;  // Current peak level for metering (0.0 - 1.0)
    float peakHold;   // Peak hold value for meter
    double peakHoldTime;  // Time when peak was set
    
    // Effects chain
    std::vector<std::shared_ptr<Effect>> effects;  // Audio effects applied to this track
    
    // Sampler
    bool hasSampler = false;  // Track has a sampler instrument
    std::string samplerSamplePath;  // Path to loaded sample (empty = waiting for sample)
    std::vector<float> samplerWaveform;  // Waveform display data for sampler
    std::shared_ptr<Sampler> sampler;  // Actual sampler instance for audio playback
    
    // MIDI recording
    std::shared_ptr<MidiClip> recordingClip;  // Current recording clip (null when not recording)
    std::vector<std::shared_ptr<MidiClip>> clips;  // All recorded clips for this track
    
    // Waveform visualization buffer (circular buffer)
    static constexpr size_t WAVEFORM_BUFFER_SIZE = 512;  // Number of samples to visualize
    std::vector<float> waveformBuffer;  // Circular buffer for waveform display
    size_t waveformBufferWritePos;  // Current write position in circular buffer
    std::unique_ptr<std::mutex> waveformBufferMutex;  // Thread safety for audio thread updates (pointer to allow copying)
    
    Track();
    void addWaveformSample(float sample);  // Add sample to circular buffer (called from audio thread)
    std::vector<float> getWaveformSamples() const;  // Get current waveform for display (called from GUI thread)
};

// Instrument preset definition
struct InstrumentPreset {
    std::string name;
    std::string category;  // e.g., "Synth", "Piano", "Bass", "Pad", "Atmosphere"
    std::vector<Oscillator> oscillators;  // Oscillator configuration
    InstrumentEnvelope envelope;  // ADSR, pitch envelope, LFO settings
    
    InstrumentPreset(const std::string& n, const std::string& cat, const std::vector<Oscillator>& oscs)
        : name(n), category(cat), oscillators(oscs) {}
    
    InstrumentPreset(const std::string& n, const std::string& cat, const std::vector<Oscillator>& oscs, 
                     const InstrumentEnvelope& env)
        : name(n), category(cat), oscillators(oscs), envelope(env) {}
};

// Sample info for browser display
struct SampleInfo {
    std::string name;
    std::string path;
    std::vector<float> waveformDisplay;  // For visualization
};

class MainWindow {
public:
    MainWindow();
    ~MainWindow();
    
    bool initialize();
    void shutdown();
    void run();
    void requestQuit();  // Request shutdown (called by signal handlers)
    
    // OS file drop support (public for GLFW callback access)
    std::vector<std::string> droppedFiles_;
    std::mutex droppedFilesMutex_;
    
private:
    bool initializeAudio();
    bool initializeMIDI();
    void renderUI();
    void renderSampleLibrary(ImGuiID target_dock_id);
    void renderComponents(ImGuiID target_dock_id);
    void renderTracks(ImGuiID target_dock_id);
    
    std::shared_ptr<AudioEngine> engine_;
    std::shared_ptr<MidiInput> midiInput_;
    std::vector<Track> tracks_;
    size_t selectedTrackIndex_;  // Currently selected track for components view
    
    // Project management
    std::string currentProjectPath_;  // Path to current project file
    bool hasUnsavedChanges_;  // Track if project has unsaved changes
    bool triggerSaveAsDialog_;  // Flag to trigger Save As dialog from keyboard shortcut
    
    // File browser state
    std::string fileBrowserPath_;  // Current directory in file browser
    std::vector<std::string> fileBrowserDirs_;  // Directories in current path
    std::vector<std::string> fileBrowserFiles_;  // Files in current path
    std::vector<std::pair<std::string, std::string>> commonDirectories_;  // Common dirs (name, path)
    void updateFileBrowser(const std::string& path);  // Update file browser contents
    void initializeCommonDirectories();  // Setup common directory shortcuts
    
    // Instrument library
    std::vector<InstrumentPreset> instrumentPresets_;  // Preset instruments
    std::vector<InstrumentPreset> userPresets_;  // User-saved presets
    void initializeInstrumentPresets();  // Setup instrument presets
    void saveUserPreset(const std::string& name, const std::vector<Oscillator>& oscillators);
    void loadUserPresets();  // Load user presets from file
    void saveUserPresetsToFile();  // Save user presets to file
    
    // Sample library
    std::vector<SampleInfo> userSamples_;  // User-loaded samples
    void loadSamplesFromDirectory();  // Load samples from samples/ directory
    void refreshSampleList();  // Refresh the samples list
    bool importSample(const std::string& sourcePath);  // Import sample to samples/
    float effectsScrollY_;  // Scroll position for effects panel
    
    // Master output metering
    float masterPeakL_;      // Left channel peak level
    float masterPeakR_;      // Right channel peak level
    float masterPeakHoldL_;  // Left channel peak hold
    float masterPeakHoldR_;  // Right channel peak hold
    double masterPeakHoldTime_;  // Time of last peak hold
    
    // SVG icon textures
    void* folderIconTexture_;  // OpenGL texture for folder icon
    void* drawIconTexture_;    // OpenGL texture for draw/pencil icon
    int folderIconWidth_, folderIconHeight_;
    int drawIconWidth_, drawIconHeight_;
    int drawIconTipOffsetX_, drawIconTipOffsetY_;  // Offset of bottom-leftmost pixel from top-left corner
    void loadSVGIcons();       // Load and rasterize SVG icons
    void* loadSVGToTexture(const char* filepath, int& width, int& height, int targetSize, int& tipOffsetX, int& tipOffsetY);  // Helper to load SVG
    
    void* window_;  // GLFWwindow* (opaque pointer to avoid including GLFW in header)
    bool shouldQuit_;
    
    // Timeline and transport controls
    float bpm_;  // Beats per minute
    bool masterRecord_;  // Master record button state (primes recording, doesn't start playback)
    float timelinePosition_;  // Current playhead position in beats
    float timelineScrollX_;  // Horizontal scroll offset for timeline
    bool isPlaying_;  // Transport play state
    double lastTime_;  // Last update time for timeline
    std::mutex timelineMutex_;  // Thread safety for timeline position (accessed from MIDI callback)
    bool isDraggingPlayhead_;  // Whether user is dragging the playhead
    float dragStartBeat_;  // Beat position when drag started
    int64_t playbackSamplePosition_;  // Current playback position in samples (for MIDI clip playback)
    
    void processDroppedFiles();  // Process files dropped from OS
    
    // Count-in feature
    bool countInEnabled_;  // Whether count-in is enabled
    bool isCountingIn_;  // Currently in count-in phase
    int countInBeatsRemaining_;  // Number of beats left in count-in
    double countInLastBeatTime_;  // Time of last count-in beat
    void generateClickSound(AudioBuffer& buffer, size_t numFrames, bool isAccent);
    
    // Piano roll editor
    enum class GridDivision {
        Whole = 1,
        Half = 2,
        Quarter = 4,
        Eighth = 8,
        Sixteenth = 16,
        ThirtySecond = 32,
        QuarterTriplet = 3,     // 3 notes per beat
        EighthTriplet = 6,      // 6 notes per beat
        SixteenthTriplet = 12   // 12 notes per beat
    };
    
    bool pianoRollActive_;  // Whether piano roll is focused
    bool pencilToolActive_;  // P key toggles pencil tool
    bool gridSnapEnabled_;  // Grid snapping enabled
    GridDivision currentGridDivision_;
    float pianoRollScrollY_;  // Vertical scroll (note range)
    int pianoRollHoverNote_;  // Currently hovered MIDI note
    bool showContextMenu_;  // Show right-click context menu
    int pianoRollCenterNote_;  // Center note for piano roll view (C4 = 60)
    bool pianoRollAutoPositioned_;  // Whether piano roll has been auto-positioned
    
    // Track which notes are currently being played (for visualization)
    std::array<bool, 128> notesPlaying_;
    std::mutex notesMutex_;
    
    struct DraggedNote {
        bool isDragging;
        size_t clipIndex;
        size_t eventIndex;
        float startBeat;
        uint8_t startNote;
        float noteDuration;
        float currentBeatDelta;  // How much the note has moved in beats
        int currentNoteDelta;     // How much the note has moved in semitones
        float clickOffsetBeat;    // Where within the note the user clicked (in beats from note start)
        int clickOffsetNote;      // Vertical offset from note center
    };
    DraggedNote draggedNote_;
    
    // Note resizing
    struct ResizingNote {
        bool isResizing;
        bool isLeftEdge;  // true for left edge, false for right edge
        size_t clipIndex;
        size_t eventIndex;
        float originalStartBeat;
        float originalEndBeat;
        uint8_t noteNum;
    };
    ResizingNote resizingNote_;
    
    // Drawing new note
    struct DrawingNote {
        bool isDrawing;
        float startBeat;
        uint8_t noteNum;
        size_t clipIndex;
    };
    DrawingNote drawingNote_;
    
    // Box selection
    struct BoxSelection {
        bool isSelecting;
        float startX, startY;
        float currentX, currentY;
    };
    BoxSelection boxSelection_;
    
    // Selected notes (clipIndex, eventIndex pairs)
    std::set<std::pair<size_t, size_t>> selectedNotes_;
    
    // Track renaming
    int renamingTrackIndex_ = -1;  // -1 means not renaming
    char trackRenameBuffer_[64] = "";
    
    void renderPianoRoll();
    void handlePianoRollInput();
    float snapToGrid(float beat) const;
    const char* getGridDivisionName(GridDivision division) const;
    void quantizeSelectedTrack();
    
    void renderMenuBar();
    void renderTransportControls();
    void renderComponentBox(size_t trackIndex, size_t oscIndex, Oscillator& osc);
    void renderInstrumentPanel(size_t trackIndex);
    void renderSamplerPanel(size_t trackIndex);
    void renderVelocityEditor(float canvasX, float canvasY, float canvasWidth, float canvasHeight);
    void renderEffectBox(size_t trackIndex, size_t effectIndex, std::shared_ptr<Effect> effect);
    void renderTrackTimeline(size_t trackIndex);
    void updateTimeline();
    
    // Project management
    void newProject();
    bool saveProject();
    bool saveProjectAs();
    bool openProject();
    bool checkUnsavedChanges();  // Returns true if user wants to continue
    void markDirty();  // Mark project as having unsaved changes
    std::string serializeProject() const;
    bool deserializeProject(const std::string& data);
};

} // namespace pan

