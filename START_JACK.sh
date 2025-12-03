#!/bin/bash
# Script to start JACK for audio testing

echo "Starting JACK audio server..."
echo "This will allow PortAudio to use JACK instead of ALSA"

# Try to start JACK with PipeWire backend
if command -v pw-jack &> /dev/null; then
    echo "Using PipeWire JACK bridge..."
    pw-jack &
    sleep 2
elif command -v jackd &> /dev/null; then
    echo "Starting JACK daemon..."
    jackd -R -d alsa -d hw:0 &
    sleep 2
else
    echo "JACK not found. Install with: sudo apt-get install jackd2"
    exit 1
fi

echo "JACK should now be running. Try running ./sine_wave_test again"

