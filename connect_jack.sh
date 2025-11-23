#!/bin/bash
# Script to connect PortAudio JACK ports to system output
# Run this while sine_wave_test is running

echo "Connecting PortAudio to system output..."

# Wait for PortAudio ports to appear
for i in {1..20}; do
    if jack_lsp 2>/dev/null | grep -q "PortAudio:out"; then
        echo "Found PortAudio ports!"
        jack_connect "PortAudio:out_0" "system:playback_1" 2>/dev/null
        jack_connect "PortAudio:out_1" "system:playback_2" 2>/dev/null
        if jack_lsp -c 2>/dev/null | grep -q "PortAudio:out_0.*system:playback_1"; then
            echo "✓ Connected! You should hear audio now."
        else
            echo "Connection attempted. Check with: jack_lsp -c"
        fi
        exit 0
    fi
    sleep 0.2
done

echo "PortAudio ports not found. Make sure sine_wave_test is running and JACK is started."

