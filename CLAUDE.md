# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository overview

Two loosely related parts:

- **Firmware** (repo root) — Arduino sketch `owl_esp32.ino` for a Seeed XIAO ESP32-S3 Sense. Records audio (PDM mic, 16 kHz mono WAV) plus 1 JPEG every 2 s (SVGA) to a microSD, and can reboot into a USB Mass Storage mode that exposes the SD card to the host.
- **Software** (`software/`) — Python pipeline (`python -m owl process`) that walks copied SD-card session folders, picks one best face crop per session, transcribes audio, and upserts encounters into SQLite.

`PROJECT.md` (root) and `software/PROJECT.md` are the authoritative narrative docs — read them before changing behavior in either half.

## Common commands

### Firmware

```bash
./flash.sh                    # compile + flash to a connected XIAO
arduino-cli compile --fqbn "$FQBN" .   # compile only (FQBN below)
```

`FQBN` must be exactly:
```
esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default
```
`PSRAM=opi` is required for camera framebuffers; `USBMode=default` (TinyUSB) is required for MSC.

Toolchain prerequisites: `arduino-cli`, `pip install esptool`, and `arduino-cli core install esp32:esp32@3.3.8` (with the espressif `package_esp32_index.json` board-manager URL added).

### Software pipeline

```bash
cd software
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
.venv/bin/python extract_faces.py --input-dir ./data
.venv/bin/python -m owl process --input-dir ./data
```

First run downloads the `buffalo_l` InsightFace model (~280 MB) to `~/.insightface/models/`.

## Architecture and gotchas

### `flash.sh` does a deliberate two-pass flash

`arduino-cli upload` is run once and **expected to fail** — its DTR/RTS pulse parks the chip in ROM USB-JTAG and the USB port renames mid-flash. The script then re-picks the new `/dev/cu.usbmodem*` and runs `esptool` directly with `--before no-reset --after watchdog-reset`. Do not "fix" the failing first pass; it is the reset mechanism.

Why `--after watchdog-reset` instead of the default `hard-reset`: on this board's native USB-Serial/JTAG, `hard-reset` toggles GPIO0 in a way that re-enters ROM download mode (`boot:0x0`) instead of running the new app. Watchdog reset bypasses the GPIO0 strap.

### Mode flag must live in NVS, not RTC RAM

The recorder ↔ MSC switch reboots via `esp_restart()`. arduino-esp32 3.3.8's startup zeroes `RTC_DATA_ATTR` and (per testing) doesn't reliably preserve `RTC_NOINIT_ATTR` either. Mode is stored in `Preferences` (NVS namespace `owl`, key `nextmode`) and **consumed on boot** so the next reboot defaults back to recorder mode.

### Mode trigger is "long-press while running", never "hold at power-on"

ESP32-S3 ROM samples GPIO0 at power-on; holding BOOT during USB plug-in lands the chip in flash-download mode before firmware runs. The selector is a 2.5 s long-press in recorder mode (see `LONG_PRESS_MS` and `rebootIntoMsc()` in `owl_esp32.ino`).

### MSC mode bypasses the Arduino SD wrapper

In flash-drive mode, `owl_esp32.ino` talks to the card via raw `sdmmc_host_*` / `sdmmc_read_sectors` / `sdmmc_write_sectors` and does **not** mount FAT on the ESP32 side. Mounting FAT on both ends would double-cache writes and corrupt the filesystem. Recorder mode uses `SD_MMC` (FAT via VFS) and is mutually exclusive with MSC.

### Camera config: `CAMERA_GRAB_WHEN_EMPTY` + `fb_count = 1`

We snap a frame every 2 s. The default `CAMERA_GRAB_LATEST` + `fb_count = 2` keeps DMA filling buffers and floods the serial log with `cam_hal: FB-OVF` warnings. Don't change these without re-checking the log.

### SD mount retry ladder

Cold boots usually mount first try; soft resets need the SD slot voltage to settle. `setup()` retries up to 8× — first two attempts at `SDMMC_FREQ_PROBING` (400 kHz), then `SDMMC_FREQ_DEFAULT` (20 MHz). Permanent fail = LED double-flashes forever.

### Session-folder naming uses compile date, not wall-clock

`compileDateIso()` parses `__DATE__`. Folders are `/<YYYY-MM-DD>_<NNN>` where `NNN` is `max(existing) + 1` for that date. There is no RTC/NTP — adding wall-clock dates means wiring up Wi-Fi + NTP.

### Card constraints

microSD must be **≤ 32 GB and FAT32**. The ESP32-S3 SDMMC controller does not reliably handle larger cards or exFAT.

## Layout pointers

- `owl_esp32.ino` — single-file sketch with both modes (recorder + MSC).
- `camera_pins.h` — OV2640 pin map for the Sense expansion board.
- `_test_blink/`, `_test_msc/` — minimal reference sketches; not part of the build.
- `software/owl/` — full face + audio + SQLite pipeline; tuning knobs documented in `software/PROJECT.md`.
- `software/extract_faces.py` — legacy face-only entrypoint.
