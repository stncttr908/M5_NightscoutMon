#!/usr/bin/env bash
set -e
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SIM_DIR="$ROOT_DIR/tools/native_sim"

echo "⚡ Compiling native C++ simulator with PlatformIO..."
/Users/david/.platformio/penv/bin/pio run -d "$SIM_DIR" -s

echo "🚀 Launching M5Stack Core2 Native Simulator..."
"$SIM_DIR/.pio/build/native_core2/program"
