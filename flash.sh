#!/usr/bin/env bash
# Build and flash owl_esp32 to a XIAO ESP32-S3 Sense.
#
# Why this script exists instead of a plain `arduino-cli upload`:
#   * arduino-cli uses esptool with `--after hard-reset`, which on this board
#     lands the chip in ROM download mode (boot:0x0) instead of running the
#     new firmware. We use `--after watchdog-reset` instead (internal reset,
#     doesn't touch the GPIO0 strap).
#   * When TinyUSB CDC is already running on the chip, the arduino-cli reset
#     pulse briefly re-enumerates the USB device, the port name changes, and
#     esptool errors out mid-flash. We let that first pass fail on purpose
#     (it still drops the chip into ROM USB-JTAG), then flash cleanly in a
#     second pass with `--before no-reset` against the new ROM port.

set -e

SKETCH_DIR="$(cd "$(dirname "$0")" && pwd)"
FQBN="esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default"

echo "==> compile"
arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"

# Arduino caches builds under ~/Library/Caches/arduino/sketches/<hash>/
BUILD=$(ls -dt "$HOME/Library/Caches/arduino/sketches"/*/ | while read d; do
  [ -f "$d/owl_esp32.ino.bin" ] && { echo "${d%/}"; break; }
done)
[ -n "$BUILD" ] || { echo "build dir not found"; exit 1; }

pick_port() { ls /dev/cu.usbmodem* 2>/dev/null | head -1; }

echo "==> kick chip into ROM download mode"
# arduino-cli upload intentionally fails here on the port rename, but its
# DTR/RTS pulse parks the chip in ROM USB-JTAG, which is what we want.
arduino-cli upload --fqbn "$FQBN" --port "$(pick_port)" "$SKETCH_DIR" 2>/dev/null || true
sleep 2

PORT=$(pick_port)
echo "==> flashing via esptool on $PORT"
# Use the standalone esptool.py on $PATH instead of `python3 -m esptool` so
# this works regardless of which virtualenv (if any) is active.
command -v esptool.py >/dev/null || { echo "esptool.py not on PATH; install with 'pip install esptool'"; exit 1; }
esptool.py --chip esp32s3 --port "$PORT" --baud 460800 \
  --before no-reset --after watchdog-reset \
  write-flash -z \
  0x0     "$BUILD/owl_esp32.ino.bootloader.bin" \
  0x8000  "$BUILD/owl_esp32.ino.partitions.bin" \
  0x10000 "$BUILD/owl_esp32.ino.bin"

echo "==> done. Firmware should be running."
