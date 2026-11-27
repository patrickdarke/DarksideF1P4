#!/usr/bin/env bash
# Build (and optionally flash) Darkside F1 P4 for the Elecrow CrowPanel
# Advanced 10.1" ESP32-P4. Console/flash is the onboard CH340 USB-UART
# bridge, so Serial rides UART0 (CDCOnBoot=default).
#
#   ./build.sh            compile
#   ./build.sh flash      compile + flash the attached panel (USB)
#   ./build.sh monitor    serial log at 115200
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

FQBN="esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=default,CDCOnBoot=default"

[ -f "$HERE/secrets.h" ] || { echo "✗ copy secrets.h.example to secrets.h and fill it in"; exit 1; }

find_port() {
  # Explicit preference order: the CH340 bridge first, so another board on
  # the bench with a native-USB port is never flashed by mistake.
  for p in /dev/cu.wchusbserial* /dev/ttyUSB* /dev/ttyACM* /dev/cu.usbserial* /dev/cu.usbmodem*; do
    [ -e "$p" ] && { echo "$p"; return; }
  done
  echo "✗ no panel port found" >&2; exit 1
}

if [ "${1:-}" = "monitor" ]; then
  arduino-cli monitor -p "$(find_port)" -c baudrate=115200
  exit 0
fi

# Optional one-off overrides without touching secrets.h, e.g.
#   DEFINES="-DREPLAY_SESSION_KEY=11420" ./build.sh flash
arduino-cli compile --fqbn "$FQBN" --libraries "$HERE/lib" --output-dir "$HERE/build" \
  --build-property "compiler.cpp.extra_flags=${DEFINES:-}" "$HERE"

if [ "${1:-}" = "flash" ]; then
  PORT="$(find_port)"
  arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$HERE/build" "$HERE"
  echo "==> flashed $PORT"
fi
