cmake_minimum_required(VERSION 3.15)

# Add test executable
add_executable(pan_tests
    test_audio_buffer.cpp
)

# Link against the pan library
target_link_libraries(pan_tests PRIVATE pan_lib)

# Include directories for tests
target_include_directories(pan_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Add tests
add_test(NAME AudioBufferTests COMMAND pan_tests)

