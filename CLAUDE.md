# CLAUDE.md

Guidance for Claude Code working in this repository. This is the **`snapshot`** branch — a single-file Arduino sketch that turns a Seeed XIAO ESP32-S3 Sense into a WiFi photo + audio device.

The wider `main` branch (multi-session recorder + Python face/audio/SQLite pipeline) is a different scope and not present here. Don't try to use docs or commands that reference `software/` or `MSC` mode — they don't apply.

## What this branch does

- Short-press BOOT → capture one SVGA JPEG to `/photos/img_NNNNNN.jpg` on the SD.
- Long-press BOOT (≥ 2.5 s) → toggle a 16 kHz mono WAV recording to `/audio/rec_NNNNNN.wav`.
- Hosts a WPA2 AP **`owl`** (password `owlowlowl`) and serves a thumbnail gallery + inline audio player + per-file delete/download at **`http://owl.local/`** (or `http://192.168.4.1/`).

User docs live in `README.md`; design narrative in `TODO.md`.

## Common commands

```bash
./flash.sh                              # compile + flash to a connected XIAO
arduino-cli compile --fqbn "$FQBN" .    # compile only
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=115200   # serial logs
```

`FQBN` must be exactly:

```
esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default
```

`PSRAM=opi` is required for camera framebuffers + WiFi co-existence.

Toolchain: `arduino-cli`, `pip install esptool`, and `arduino-cli core install esp32:esp32@3.3.8` with the espressif `package_esp32_index.json` board-manager URL added.

## Architecture and gotchas

### `flash.sh` does a deliberate two-pass flash

`arduino-cli upload` is run once and **expected to fail** — its DTR/RTS pulse parks the chip in ROM USB-JTAG and the USB port renames mid-flash. The script then re-picks the new `/dev/cu.usbmodem*` and runs `esptool.py` directly with `--before no-reset --after watchdog-reset`. Don't "fix" the failing first pass; it is the reset mechanism.

Why `--after watchdog-reset` instead of the default `hard-reset`: on this board's native USB-Serial/JTAG, `hard-reset` toggles GPIO0 in a way that re-enters ROM download mode (`boot:0x0`) instead of running the new app. Watchdog reset bypasses the GPIO0 strap.

`flash.sh` calls `esptool.py` (the standalone binary on `$PATH`) rather than `python3 -m esptool` so it works regardless of which Python venv is active.

### Camera config: `CAMERA_GRAB_WHEN_EMPTY` + `fb_count = 1`

This branch captures a single JPEG per BOOT short-press, so the default `CAMERA_GRAB_LATEST` + `fb_count = 2` (which keeps DMA filling buffers continuously) just generates `cam_hal: FB-OVF` warnings. We use `CAMERA_GRAB_WHEN_EMPTY` + `fb_count = 1`, then drop two stale frames at the start of `captureOnePhoto()` so the saved frame reflects the current scene rather than whatever was buffered at the previous press.

### SD mount retry ladder

Cold boots usually mount first try; soft resets need the SD slot voltage to settle. `setup()` retries up to 8× — first two attempts at `SDMMC_FREQ_PROBING` (400 kHz), then `SDMMC_FREQ_DEFAULT` (20 MHz). Permanent fail = LED ~4 Hz blink forever.

### LED has overlapping states managed in loop()

`ledHeartbeatTick()` runs every loop iteration. While `g_recording == true` it forces the LED solid on (suppresses the heartbeat). `ledFlash(100)` during a photo capture is allowed to interrupt either pattern; the next tick (within ~5–10 ms) restores the correct state.

### WiFi AP password is hardcoded in source

`AP_PASSWORD` is in `owl_esp32.ino` and visible to anyone reading this repo. Acceptable for a hobby device on a private 1-client AP; not acceptable for anything sensitive.

### No real-time clock

The board has no RTC and doesn't reach NTP (the AP has no upstream). Files are sequentially numbered (`img_NNNNNN.jpg`, `rec_NNNNNN.wav`); counters are seeded at boot by scanning `/photos` and `/audio` for the highest existing number. Capped at 999999 — beyond that we log and refuse to write rather than wrap silently.

### Card constraints

microSD must be **≤ 32 GB and FAT32**. The ESP32-S3 SDMMC controller does not reliably handle larger cards or exFAT.

### HTTP path-traversal guard

`/photo/<name>` and `/audio/<name>` reject any name containing `/` or `..` before resolving against `PHOTOS_DIR` / `AUDIO_DIR`, so a crafted URL can't read or delete files outside those dirs.

### The actively-recording WAV is hidden from /list

If `g_recording` is true, the in-progress `rec_NNNNNN.wav` is excluded from `/list` and `GET /audio/<that_name>` returns 503. The file's RIFF/data sizes aren't patched until `recordingStop()` runs, so serving it would expose a malformed header.

## Layout pointers

- `owl_esp32.ino` — single-file sketch: photos, audio, WiFi AP, HTTP server, gallery HTML.
- `camera_pins.h` — OV2640 pin map for the Sense expansion board.
- `flash.sh` — build + flash wrapper that works around the reset quirk.
- `README.md` — user-facing docs (connect & download, button gestures, LED indicators, HTTP API).
- `TODO.md` — design narrative for the build-out (Steps 1–10, all complete).
