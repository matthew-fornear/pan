#!/bin/bash
# Run sine_wave_test with automatic JACK port connection

cd "$(dirname "$0")/build" || exit 1

echo "Starting sine_wave_test..."
./sine_wave_test >/dev/null 2>&1 &
APP_PID=$!

# Wait for PortAudio ports to appear
echo "Waiting for PortAudio ports..."
for i in {1..20}; do
    if jack_lsp 2>/dev/null | grep -q "PortAudio:out_0"; then
        echo "Found PortAudio ports! Connecting..."
        jack_connect "PortAudio:out_0" "system:playback_1" 2>/dev/null
        jack_connect "PortAudio:out_1" "system:playback_2" 2>/dev/null
        if jack_lsp -c 2>/dev/null | grep -q "PortAudio:out_0.*system:playback_1"; then
            echo "✓ Connected! You should hear a 440 Hz tone."
        else
            echo "Connection attempted. Check manually with: jack_lsp -c"
        fi
        break
    fi
    sleep 0.1
done

# Wait for the app to finish
wait $APP_PID 2>/dev/null

