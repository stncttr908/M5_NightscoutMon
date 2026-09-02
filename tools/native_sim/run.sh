#!/usr/bin/env bash
set -e
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "⚡ Compiling native C++ simulator..."
/Users/david/.platformio/penv/bin/pio run -d "$DIR" -s

echo "🚀 Launching M5Stack Core2 Native Simulator..."
"$DIR/.pio/build/native_core2/program"
