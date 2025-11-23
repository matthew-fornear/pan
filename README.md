# Pan - Digital Audio Workstation

Pan is a modern, open-source Digital Audio Workstation (DAW) designed for music production, audio editing, and sound design.

GitHub: https://github.com/matthew-fornear/pan

## Overview

Pan provides a comprehensive platform for creating, editing, and mixing audio projects with a full-featured graphical interface. Whether you're a professional producer or an aspiring musician, Pan offers the tools you need to bring your musical ideas to life.

## Features

### Core Functionality
- **Full GUI Interface**: Complete graphical user interface built with ImGui and GLFW
- **Multi-track Recording**: Record and edit multiple MIDI tracks simultaneously
- **MIDI Support**: Full MIDI sequencing and editing with real-time input
- **Piano Roll Editor**: Visual MIDI note editor with:
  - Grid snapping (1/4, 1/8, 1/16, triplets, etc.)
  - Draw tool for creating notes
  - Note dragging and resizing
  - Box selection
  - Quantization
- **Timeline View**: Visual timeline with beat markers and playhead
- **Instrument Library**: Built-in instrument presets organized by category:
  - **Synth**: Supersaw, Hollow Pad, Bell Lead, Deep Bass, Harsh Lead
  - **Piano**: Bright Piano, Electric Piano, Dark Piano
- **User Presets**: Save and load custom oscillator configurations
- **Audio Effects**: 
  - Reverb with adjustable parameters (Room Size, Damping, Wet/Dry, Width)
  - Reverb presets (Room, Hall, Plate, Chamber, Cathedral, Spring, Custom)
- **Multiple Oscillators**: Each track supports multiple oscillators with:
  - Waveform types: Sine, Square, Sawtooth, Triangle
  - Frequency multipliers
  - Amplitude control
- **Project Management**: Save and load projects in `.pan` format with file browser
- **Transport Controls**: Play, pause, stop, record with BPM control
- **Count-In**: 4-beat click before recording/playback
- **Drag & Drop**: Intuitive drag-and-drop for instruments, waveforms, and effects

### Planned Features
- Additional effects (Delay, Chorus, Distortion, etc.)
- Plugin support (VST, AU, LV2)
- Automation curves
- Audio track recording (currently MIDI only)
- Time-stretching and pitch-shifting
- Spectral editing
- Surround sound support

## Installation

### Prerequisites
- CMake 3.14 or higher
- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- **PortAudio** (for audio I/O):
  - Ubuntu/Debian: `sudo apt-get install libportaudio2 portaudio19-dev`
  - macOS: `brew install portaudio`
  - Windows: Download from [portaudio.com](http://www.portaudio.com/)
- **ALSA** (for MIDI input on Linux):
  - Ubuntu/Debian: `sudo apt-get install libasound2-dev`
- **GLFW3** (for GUI windowing):
  - Ubuntu/Debian: `sudo apt-get install libglfw3-dev libgl1-mesa-dev`
  - macOS: `brew install glfw`
  - Fedora/RHEL: `sudo dnf install glfw-devel mesa-libGL-devel`
- **ImGui** (included as submodule or bundled)
- **nanosvg** (for SVG icon rendering, included)

### Building from Source

```bash
git clone https://github.com/matthew-fornear/pan.git
cd pan
mkdir build && cd build
cmake ..
make
# Or on Windows: cmake --build . --config Release
```

The executable will be built as `build/pan`.

## Usage

### Running the Application

```bash
cd build
./pan
```

### Basic Workflow

1. **Create a Track**: Click "+ Add Track" to create a new track
2. **Load an Instrument**: 
   - Drag an instrument from the Library (Synth or Piano categories)
   - Or drag a basic waveform (Sine, Square, Sawtooth, Triangle)
   - Drop onto a track header or the Components tab
3. **Record MIDI**: 
   - Arm a track (click the red/grey circle)
   - Enable master record (red circle in transport)
   - Click play to start recording
   - Play notes on your MIDI keyboard
4. **Edit Notes**: 
   - Use the Piano Roll editor at the bottom
   - Draw notes with the Draw tool (Ctrl+D or D key)
   - Drag notes to move them
   - Resize notes by dragging their edges
   - Select multiple notes with box selection
5. **Add Effects**: 
   - Go to the Effects tab
   - Click "+ Add Effect" → select Reverb
   - Adjust parameters in the effect box
6. **Save Your Project**: 
   - File → Save (Ctrl+S) or Save As...
   - Projects are saved as `.pan` files

### Keyboard Shortcuts

- **Ctrl+D** or **D**: Toggle draw tool in piano roll
- **Ctrl+A**: Select all notes in piano roll
- **Delete**: Delete selected notes
- **Ctrl+S**: Save project
- **Ctrl+O**: Open project
- **Ctrl+N**: New project
- **Q**: Quantize selected notes (when piano roll is active)

### Piano Roll Features

- **Grid Snapping**: Right-click to change grid division (1/4, 1/8, 1/16, triplets, etc.)
- **Draw Tool**: Click and drag to create notes of variable length
- **Note Editing**: 
  - Click and drag notes to move them
  - Drag left/right edges to resize notes
  - Box select multiple notes
  - Drag multiple selected notes together
- **Note Overlap**: Dragging a note over another automatically cuts the underlying note
- **Quantization**: Right-click → "Quantize Notes" to snap notes to grid

### Saving Presets

1. Configure oscillators in the Components tab
2. Click "Save Preset" next to "Track X Components:"
3. Enter a name for your preset
4. Your preset will appear in the "Presets" section of the Library
5. Right-click a saved preset to delete it

## Architecture

Pan is built with a modular C++ architecture:

### Core Components

- **Audio Engine** (`src/audio/`): Real-time audio processing engine
  - `AudioEngine`: Main audio processing coordinator
  - `AudioBuffer`: Multi-channel audio buffer management
  - `Effect`: Base class for audio effects
  - `Reverb`: Reverb effect implementation

- **MIDI System** (`src/midi/`): MIDI input and synthesis
  - `MidiInput`: MIDI device input handling
  - `MidiMessage`: MIDI message representation
  - `Synthesizer`: Multi-voice synthesizer with multiple oscillators
  - `MidiClip`: MIDI clip storage and playback

- **GUI System** (`src/gui/`): Complete graphical interface
  - `MainWindow`: Main application window and UI management
  - Piano roll editor
  - Timeline view
  - Track management
  - File browser
  - Component and effect editors

### Project Structure

```
pan/
├── include/pan/          # Public headers
│   ├── audio/           # Audio engine headers
│   ├── midi/            # MIDI system headers
│   └── gui/             # GUI headers
├── src/                  # Implementation files
│   ├── audio/           # Audio engine implementation
│   ├── midi/            # MIDI implementation
│   ├── gui/             # GUI implementation
│   └── main.cpp         # Application entry point
├── svg/                  # SVG icons (draw, folder)
├── CMakeLists.txt       # Build configuration
└── README.md            # This file
```

### Technology Stack

- **Language**: C++17
- **Build System**: CMake
- **Audio Backend**: PortAudio
- **MIDI Backend**: ALSA (Linux)
- **GUI**: ImGui + GLFW (required)
- **SVG Rendering**: nanosvg

## Troubleshooting

### Audio Issues on Ubuntu/PipeWire

If audio isn't working on Ubuntu with PipeWire, ensure PortAudio is built with PulseAudio support:

```bash
# Install build dependencies
sudo apt-get install build-essential libasound2-dev libpulse-dev

# Download and build PortAudio with PulseAudio support
cd /tmp
wget http://files.portaudio.com/archives/pa_stable_v190700_20210406.tgz
tar xzf pa_stable_v190700_20210406.tgz
cd portaudio
./configure --with-pulse
make
sudo make install
sudo ldconfig

# Rebuild your project
cd ~/projects/pan/build
cmake ..
make
```

### ALSA/PipeWire Configuration

If you're having audio issues, try creating `~/.asoundrc`:

```bash
pcm.!default {
    type pulse
}
ctl.!default {
    type pulse
}
```

Then restart your audio session or reboot.

### MIDI Device Not Found

- Ensure your MIDI device is connected
- Check ALSA MIDI device list: `aconnect -l`
- The application will continue without MIDI if no device is found

### CMake Issues

- **CMake not found**: Install CMake 3.14 or higher
- **Compiler errors**: Ensure you have a C++17 compatible compiler
- **Link errors**: Make sure all required libraries are installed (PortAudio, GLFW, OpenGL)

## Development Roadmap

- [x] Core audio engine structure
- [x] Audio buffer management
- [x] MIDI input support
- [x] Synthesizer with multiple waveforms and oscillators
- [x] Full GUI interface with ImGui
- [x] Piano roll editor with drawing and editing
- [x] Timeline view with MIDI clip playback
- [x] Track system with drag-and-drop
- [x] Instrument library with presets
- [x] User preset saving and loading
- [x] Effects system (Reverb implemented)
- [x] Project file format (save/load)
- [x] File browser for project management
- [ ] Additional effects (Delay, Chorus, Distortion)
- [ ] Audio track recording
- [ ] Automation curves
- [ ] Plugin support (VST, AU, LV2)
- [ ] Audio export functionality

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## License

[Specify your license here]

## Acknowledgments

- PortAudio for cross-platform audio I/O
- ALSA for MIDI support on Linux
- ImGui for the GUI framework
- GLFW for window management
- nanosvg for SVG rendering

## Contact

GitHub: https://github.com/matthew-fornear/pan
