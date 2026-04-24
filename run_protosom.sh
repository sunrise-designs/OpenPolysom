#!/bin/bash

# Configuration
SESSION_NAME="recording_session"
PROJECT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
VENV_PATH="$PROJECT_DIR/.venv"
BUILD_DIR="$PROJECT_DIR/build"

# Check that your microphone is at hw:2,0. You can check this with `arecord -l` and adjust if necessary.
HW_ID="hw:2,0"
# Generates: output_20231027_143005.flac
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
FILENAME="output_${TIMESTAMP}.flac"

# 1. Check that the project has been built
if [ ! -f "$BUILD_DIR/main" ]; then
    echo "ERROR: $BUILD_DIR/main not found. Build the project first:"
    echo "  cmake -B build && cmake --build build"
    exit 1
fi

# 2. Check if the audio device exists
# We check /proc/asound/cards or use arecord to see if the device is listed
if ! arecord -l | grep -q "card ${HW_ID:3:1}"; then
    echo "ERROR: Audio device $HW_ID not found!"
    echo "Check your connections with 'arecord -l' and try again."
    exit 1
fi

# Function to kill the tmux session on exit
cleanup() {
    echo "Shutting down..."
    tmux kill-session -t "$SESSION_NAME"
    exit
}

# Trap SIGINT (Ctrl+C) and SIGTERM
trap cleanup SIGINT SIGTERM

# Start a new detached tmux session
tmux new-session -d -s "$SESSION_NAME"

# --- Task 1: Audio Recording ---
tmux send-keys -t "$SESSION_NAME" "/usr/bin/nice -n 19 ffmpeg -f alsa -sample_rate 96000 -i $HW_ID -af \"aresample=resampler=soxr\" -ar 22050 -ac 1 -acodec flac $FILENAME" Enter

# --- Task 2: ble.py (in Virtual Env) ---
tmux split-window -h -t "$SESSION_NAME"
tmux send-keys -t "$SESSION_NAME" "source $VENV_PATH/bin/activate && cd $PROJECT_DIR/python && python3 ble.py" Enter

# --- Task 3: main (High Priority) ---
tmux split-window -v -t "$SESSION_NAME"
# Using sudo for -n -20 as negative niceness usually requires root/sudo
tmux send-keys -t "$SESSION_NAME" "cd $BUILD_DIR && /usr/bin/nice -n -20 ./main edf" Enter

echo "Session '$SESSION_NAME' started."
echo "Press [Ctrl+C] in this terminal to stop all tasks and close tmux."

# Keep the script alive so the trap stays active
while true; do sleep 1; done
