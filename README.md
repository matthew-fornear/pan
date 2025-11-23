# Pan - Digital Audio Workstation

Pan is a modern, open-source Digital Audio Workstation (DAW) designed for music production, audio editing, and sound design.

GitHub: https://github.com/matthew-fornear/pan

## Overview

Pan provides a comprehensive platform for creating, editing, and mixing audio projects. Whether you're a professional producer or an aspiring musician, Pan offers the tools you need to bring your musical ideas to life.

## Features

### Core Functionality
- **Multi-track Recording**: Record and edit multiple audio tracks simultaneously
- **MIDI Support**: Full MIDI sequencing and editing capabilities with synthesizer
- **Waveform Types**: Sine, Square, Sawtooth, and Triangle waveforms
- **Sustain Pedal**: Full sustain pedal support (CC 64)
- **Audio Effects**: Built-in effects processing (reverb, delay, EQ, compression, etc.)
- **Virtual Instruments**: Synthesizers with multiple waveforms
- **Mixing Console**: Professional mixing interface with faders, panning, and sends
- **Timeline Editing**: Precise audio and MIDI editing with waveform visualization
- **Project Management**: Save, load, and export projects in various formats

### Planned Features
- Plugin support (VST, AU, LV2)
- Automation curves
- Time-stretching and pitch-shifting
- Spectral editing
- Surround sound support
- Collaboration features

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

### Building from Source

```bash
git clone https://github.com/matthew-fornear/pan.git
cd pan
mkdir build && cd build
cmake ..
make
# Or on Windows: cmake --build . --config Release
```

### Build Options

- `BUILD_TESTS=ON/OFF`: Build the test suite (default: ON)
- `BUILD_EXAMPLES=ON/OFF`: Build example projects (default: OFF)

Example:
```bash
cmake -DBUILD_EXAMPLES=ON -DBUILD_TESTS=OFF ..
make
```

### Running Tests

After building:
```bash
cd build
ctest
# Or run directly:
./pan_tests
```

## Usage

### Basic Usage

Run the main application:
```bash
cd build
./pan
```

### Testing Audio Output

Build and run the sine wave test example:
```bash
cd build
cmake -DBUILD_EXAMPLES=ON ..
make sine_wave_test
./sine_wave_test
```

This will play a 440 Hz sine wave for 3 seconds to verify audio output is working.

### MIDI Keyboard Test

Test with a MIDI keyboard:
```bash
cd build
cmake -DBUILD_EXAMPLES=ON ..
make midi_keyboard_test
./midi_keyboard_test
```

Connect your MIDI keyboard and play notes!

### Waveform Testing

Test different waveforms (terminal-based, no GUI needed):
```bash
cd build
cmake -DBUILD_EXAMPLES=ON ..
make waveform_test
./waveform_test
```

Then use the menu:
- Press `1` for Sine wave
- Press `2` for Square wave
- Press `3` for Sawtooth wave
- Press `4` for Triangle wave
- Press `q` to quit

The waveform changes in real-time while you play your MIDI keyboard.

### GUI Waveform Test (Optional)

If you want a graphical interface, install GUI dependencies first:

**Ubuntu/Debian:**
```bash
sudo apt-get install libglfw3-dev libgl1-mesa-dev
```

**macOS (with Homebrew):**
```bash
brew install glfw
```

**Fedora/RHEL:**
```bash
sudo dnf install glfw-devel mesa-libGL-devel
```

Then build and run:
```bash
cd build
cmake .. -DBUILD_EXAMPLES=ON
make waveform_gui_test
./waveform_gui_test
```

The GUI provides radio buttons to select waveforms and a volume slider.

## Architecture

Pan is built with a modular C++ architecture:

### Core Components

- **Audio Engine** (`src/audio/`): Real-time audio processing engine
  - `AudioEngine`: Main audio processing coordinator
  - `AudioBuffer`: Multi-channel audio buffer management
  - `AudioDevice`: Audio I/O device abstraction

- **MIDI System** (`src/midi/`): MIDI input and synthesis
  - `MidiInput`: MIDI device input handling
  - `MidiMessage`: MIDI message representation
  - `Synthesizer`: Multi-voice synthesizer with multiple waveforms

- **Track System** (`src/track/`): Track and clip management
  - `Track`: Individual audio/MIDI track with effects and mixing
  - `TrackManager`: Manages all tracks in a project

- **Project Management** (`src/project/`): Project file operations
  - `ProjectManager`: Handles project save/load operations

### Project Structure

```
pan/
├── include/pan/          # Public headers
│   ├── audio/           # Audio engine headers
│   ├── midi/            # MIDI system headers
│   ├── track/            # Track system headers
│   └── project/         # Project management headers
├── src/                  # Implementation files
│   ├── audio/           # Audio engine implementation
│   ├── midi/            # MIDI implementation
│   ├── track/            # Track system implementation
│   ├── project/         # Project management implementation
│   └── main.cpp         # Application entry point
├── examples/             # Example programs
├── tests/                # Unit tests
├── CMakeLists.txt       # Build configuration
└── README.md            # This file
```

### Technology Stack

- **Language**: C++17
- **Build System**: CMake
- **Audio Backend**: PortAudio
- **MIDI Backend**: ALSA (Linux)
- **GUI**: ImGui + GLFW (optional)

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

### CMake Issues

- **CMake not found**: Install CMake 3.14 or higher
- **Compiler errors**: Ensure you have a C++17 compatible compiler
- **Link errors**: Make sure all required audio libraries are installed

## Development Roadmap

- [x] Core audio engine structure
- [x] Audio buffer management
- [x] Track system architecture
- [x] Project management framework
- [x] Audio backend integration (PortAudio)
- [x] Real-time audio I/O and routing
- [x] MIDI input support
- [x] Synthesizer with multiple waveforms
- [x] Sustain pedal support
- [ ] User interface (GUI framework)
- [ ] Effects processing system
- [ ] Virtual instruments
- [ ] Project file format (save/load)
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
- ImGui for the GUI framework (optional)

## Contact

GitHub: https://github.com/matthew-fornear/pan
