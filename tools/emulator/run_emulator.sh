#!/usr/bin/env bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$DIR"

MODE="${1:-qemu}"

# Use provided token if not already in environment
export WOKWI_CLI_TOKEN="${WOKWI_CLI_TOKEN:-wok_LhMDIDHAYTvNf7aKXjxzkta802vi8Za6f7073eec}"

case "$MODE" in
  wokwi)
    if [ -z "$WOKWI_CLI_TOKEN" ]; then
      echo "---------------------------------------------------------------"
      echo "[WOKWI] To run Wokwi CLI, get a free token at:"
      echo "        https://wokwi.com/dashboard/ci"
      echo "        Then run: export WOKWI_CLI_TOKEN=\"<your_token>\""
      echo "---------------------------------------------------------------"
    fi
    echo "[EMU] Launching Wokwi M5Stack Core2 Emulator..."
    wokwi-cli .
    ;;
  qemu)
    echo "[EMU] Merging and padding 16MB flash image for QEMU..."
    mkdir -p /tmp/qemu_esp32
    esptool --chip esp32 merge-bin -o /tmp/qemu_esp32/flash_image.bin \
      --flash-mode dio --flash-size 16MB \
      0x1000 Binaries/ESP32_16MB/M5_NightscoutMon.ino.bootloader.bin \
      0x8000 Binaries/ESP32_16MB/M5_NightscoutMon.ino.partitions.bin \
      0x10000 Binaries/ESP32_16MB/M5_NightscoutMon.ino.bin
    truncate -s 16M /tmp/qemu_esp32/flash_image.bin
    echo "[EMU] Starting Espressif QEMU ESP32 machine (Press Ctrl+A then X to exit)..."
    qemu-system-xtensa -nographic -machine esp32 \
      -drive file=/tmp/qemu_esp32/flash_image.bin,if=mtd,format=raw
    ;;
  browser|gui|*)
    echo "[EMU] Opening Virtual M5Core2 Emulator in default browser..."
    open "$DIR/tools/emulator/emulator.html"
    ;;
esac
