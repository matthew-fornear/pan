cmake_minimum_required(VERSION 3.14)
project(pan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build options
option(BUILD_TESTS "Build test suite" ON)
option(BUILD_EXAMPLES "Build example projects" OFF)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Library source files (excluding main.cpp)
set(LIB_SOURCES
    src/audio/audio_engine.cpp
    src/audio/audio_buffer.cpp
    src/audio/audio_device.cpp
    src/audio/reverb.cpp
    src/project/project_manager.cpp
    src/track/track.cpp
    src/track/track_manager.cpp
    src/track/audio_clip.cpp
    src/midi/midi_message.cpp
    src/midi/midi_clip.cpp
    src/midi/synthesizer.cpp
    src/midi/midi_input.cpp
    src/gui/main_window.cpp
)

# Header files
set(HEADERS
    include/pan/audio/audio_engine.h
    include/pan/audio/audio_buffer.h
    include/pan/audio/audio_device.h
    include/pan/project/project_manager.h
    include/pan/track/track.h
    include/pan/track/track_manager.h
    include/pan/track/audio_clip.h
    include/pan/midi/midi_message.h
    include/pan/midi/midi_clip.h
    include/pan/midi/synthesizer.h
    include/pan/midi/midi_input.h
    include/pan/gui/main_window.h
)

# Create library target
add_library(pan_lib STATIC ${LIB_SOURCES} ${HEADERS})

# GUI requires GLFW and OpenGL
find_package(glfw3 QUIET)
find_package(OpenGL QUIET)

if(glfw3_FOUND AND OpenGL_FOUND)
    # Download and configure ImGui
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG docking
    )
    FetchContent_MakeAvailable(imgui)
    
    # Link ImGui sources to library
    target_sources(pan_lib PRIVATE
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(pan_lib PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    
    target_link_libraries(pan_lib PRIVATE glfw OpenGL::GL)
    target_compile_definitions(pan_lib PRIVATE PAN_USE_GUI=1)
else()
    message(STATUS "GLFW3 or OpenGL not found - GUI will be disabled")
    message(STATUS "Install: sudo apt-get install libglfw3-dev libgl1-mesa-dev")
endif()

# Find PortAudio - prefer /usr/local (where we just installed it)
find_library(PORTAUDIO_LIBRARY
    NAMES portaudio
    PATHS
        /usr/local/lib
        /usr/lib
        /opt/local/lib
        ${CMAKE_SOURCE_DIR}/third_party/portaudio/lib
)

find_path(PORTAUDIO_INCLUDE_DIR
    NAMES portaudio.h
    PATHS
        /usr/local/include
        /usr/include
        /opt/local/include
        ${CMAKE_SOURCE_DIR}/third_party/portaudio/include
)

if(PORTAUDIO_LIBRARY AND PORTAUDIO_INCLUDE_DIR)
    message(STATUS "Found PortAudio: ${PORTAUDIO_LIBRARY}")
    target_include_directories(pan_lib PRIVATE ${PORTAUDIO_INCLUDE_DIR})
    target_link_libraries(pan_lib PRIVATE ${PORTAUDIO_LIBRARY})
    target_compile_definitions(pan_lib PRIVATE PAN_USE_PORTAUDIO=1)
else()
    message(WARNING "PortAudio not found. Audio I/O will be disabled.")
    message(STATUS "Install PortAudio to enable audio functionality:")
    message(STATUS "  Ubuntu/Debian: sudo apt-get install libportaudio2 portaudio19-dev")
    message(STATUS "  macOS: brew install portaudio")
    message(STATUS "  Windows: Download from http://www.portaudio.com/")
endif()

# Find ALSA for MIDI support
find_library(ASOUND_LIBRARY
    NAMES asound
    PATHS
        /usr/lib
        /usr/local/lib
)

if(ASOUND_LIBRARY)
    message(STATUS "Found ALSA: ${ASOUND_LIBRARY}")
    target_link_libraries(pan_lib PRIVATE ${ASOUND_LIBRARY})
    target_compile_definitions(pan_lib PRIVATE PAN_USE_ALSA_MIDI=1)
else()
    message(WARNING "ALSA not found. MIDI input will be disabled.")
    message(STATUS "Install ALSA development libraries:")
    message(STATUS "  Ubuntu/Debian: sudo apt-get install libasound2-dev")
endif()

# Create executable
add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE pan_lib)

# Copy icon file to build directory if it exists (so executable can find it)
if(EXISTS ${CMAKE_SOURCE_DIR}/betaicon.png)
    file(COPY ${CMAKE_SOURCE_DIR}/betaicon.png DESTINATION ${CMAKE_BINARY_DIR})
    message(STATUS "Copying betaicon.png to build directory")
else()
    message(WARNING "betaicon.png not found in source directory")
endif()

# Copy SVG icons directory to build directory
if(EXISTS ${CMAKE_SOURCE_DIR}/svg)
    file(COPY ${CMAKE_SOURCE_DIR}/svg DESTINATION ${CMAKE_BINARY_DIR})
    message(STATUS "Copying svg directory to build directory")
else()
    message(WARNING "svg directory not found in source directory")
endif()

# Install rules
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# Testing
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(BUILD_EXAMPLES)
    add_executable(sine_wave_test examples/sine_wave_test.cpp)
    target_link_libraries(sine_wave_test PRIVATE pan_lib)
    target_include_directories(sine_wave_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(PORTAUDIO_LIBRARY)
        target_link_libraries(sine_wave_test PRIVATE ${PORTAUDIO_LIBRARY})
        target_compile_definitions(sine_wave_test PRIVATE PAN_USE_PORTAUDIO=1)
    endif()
    
    add_executable(midi_test examples/midi_test.cpp)
    target_link_libraries(midi_test PRIVATE pan_lib)
    target_include_directories(midi_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(PORTAUDIO_LIBRARY)
        target_link_libraries(midi_test PRIVATE ${PORTAUDIO_LIBRARY})
        target_compile_definitions(midi_test PRIVATE PAN_USE_PORTAUDIO=1)
    endif()
    
    add_executable(midi_keyboard_test examples/midi_keyboard_test.cpp)
    target_link_libraries(midi_keyboard_test PRIVATE pan_lib)
    target_include_directories(midi_keyboard_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(PORTAUDIO_LIBRARY)
        target_link_libraries(midi_keyboard_test PRIVATE ${PORTAUDIO_LIBRARY})
        target_compile_definitions(midi_keyboard_test PRIVATE PAN_USE_PORTAUDIO=1)
    endif()
    if(ASOUND_LIBRARY)
        target_link_libraries(midi_keyboard_test PRIVATE ${ASOUND_LIBRARY})
        target_compile_definitions(midi_keyboard_test PRIVATE PAN_USE_ALSA_MIDI=1)
    endif()
    
    # Waveform test (terminal-based)
    add_executable(waveform_test examples/waveform_test.cpp)
    target_link_libraries(waveform_test PRIVATE pan_lib)
    target_include_directories(waveform_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
    if(PORTAUDIO_LIBRARY)
        target_link_libraries(waveform_test PRIVATE ${PORTAUDIO_LIBRARY})
        target_compile_definitions(waveform_test PRIVATE PAN_USE_PORTAUDIO=1)
    endif()
    if(ASOUND_LIBRARY)
        target_link_libraries(waveform_test PRIVATE ${ASOUND_LIBRARY})
        target_compile_definitions(waveform_test PRIVATE PAN_USE_ALSA_MIDI=1)
    endif()
    
    # Waveform GUI test (optional, requires GLFW and OpenGL)
    find_package(glfw3 QUIET)
    if(NOT glfw3_FOUND)
        find_library(GLFW_LIB NAMES glfw glfw3 PATHS /usr/lib /usr/local/lib)
        find_path(GLFW_INC NAMES GLFW/glfw3.h PATHS /usr/include /usr/local/include)
        if(GLFW_LIB AND GLFW_INC)
            set(glfw3_FOUND TRUE)
            set(glfw3_LIBRARIES ${GLFW_LIB})
            set(glfw3_INCLUDE_DIRS ${GLFW_INC})
        endif()
    endif()
    
    find_package(OpenGL QUIET)
    
    if(glfw3_FOUND AND OpenGL_FOUND)
        message(STATUS "GLFW3 and OpenGL found - building waveform_gui_test")
        # Download and configure ImGui only if dependencies are available
        include(FetchContent)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG docking  # Use docking branch (more stable than version tags)
        )
        FetchContent_MakeAvailable(imgui)
        
        add_executable(waveform_gui_test examples/waveform_gui_test.cpp)
        # Use the imported target from find_package(glfw3) - it handles all dependencies
        target_link_libraries(waveform_gui_test PRIVATE pan_lib glfw OpenGL::GL)
        target_include_directories(waveform_gui_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
        
        # Link ImGui sources directly
        target_sources(waveform_gui_test PRIVATE
            ${imgui_SOURCE_DIR}/imgui.cpp
            ${imgui_SOURCE_DIR}/imgui_demo.cpp
            ${imgui_SOURCE_DIR}/imgui_draw.cpp
            ${imgui_SOURCE_DIR}/imgui_tables.cpp
            ${imgui_SOURCE_DIR}/imgui_widgets.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        )
        target_include_directories(waveform_gui_test PRIVATE
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )
        
        if(PORTAUDIO_LIBRARY)
            target_link_libraries(waveform_gui_test PRIVATE ${PORTAUDIO_LIBRARY})
            target_compile_definitions(waveform_gui_test PRIVATE PAN_USE_PORTAUDIO=1)
        endif()
        if(ASOUND_LIBRARY)
            target_link_libraries(waveform_gui_test PRIVATE ${ASOUND_LIBRARY})
            target_compile_definitions(waveform_gui_test PRIVATE PAN_USE_ALSA_MIDI=1)
        endif()
        
        set_target_properties(waveform_gui_test PROPERTIES CXX_STANDARD 17)
    else()
        message(STATUS "GLFW3 or OpenGL not found - waveform_gui_test will not be built")
        message(STATUS "Install: sudo apt-get install libglfw3-dev libgl1-mesa-dev")
    endif()
endif()

