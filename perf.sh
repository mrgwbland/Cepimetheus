#!/bin/bash

# Automate perf report

ENGINE_BIN="./release/Cepimetheus"

# 1. Check if engine exists
if [ ! -f "$ENGINE_BIN" ]; then
    echo "Error: $ENGINE_BIN not found!"
    exit 1
fi

echo "Starting perf recording..."

# 2. Use a subshell to feed commands with delays
# This ensures 'quit' is only sent AFTER the search is done.
perf record -e branch-misses -g -- "$ENGINE_BIN" <<< bench

echo "----------------------------------------------------"
echo "Recording complete! If you see 'Captured' samples (above 100), it worked."
echo "Writing full report to report.txt..."
perf report --stdio > report.txt
echo "Top 20 Branch Misses:"
perf report --stdio -n --no-children | grep -E '^[ ]+[0-9]+\.[0-9]+%' | head -n 20