#!/bin/bash

# --- Cepimetheus Slow-Feed Profiler ---

ENGINE_BIN="./release/Cepimetheus"

# 1. Check if engine exists
if [ ! -f "$ENGINE_BIN" ]; then
    echo "Error: $ENGINE_BIN not found!"
    exit 1
fi

echo "Starting perf recording (Search will run for 10 seconds)..."

# 2. Use a subshell to feed commands with delays
# This ensures 'quit' is only sent AFTER the search is done.
(
  echo "isready"
  sleep 0.5
  echo "position startpos"
  sleep 0.5
  echo "go movetime 10000"
  sleep 10.5               # Wait slightly longer than the search time
  echo "quit"
) | perf record -g -- "$ENGINE_BIN"

echo "----------------------------------------------------"
echo "Recording complete! If you see 'Captured' samples (above 100), it worked."
echo "Run 'perf report --stdio > report.txt' to save analysis."