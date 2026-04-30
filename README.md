# owl_esp32 — `snapshot` branch

This branch is a **standalone WiFi photo + audio device**, not the multi-session recorder on `main`. The firmware:

- Captures a single SVGA JPEG when you **short-press BOOT**.
- Toggles a 16 kHz mono WAV recording when you **long-press BOOT** (≥ 2.5 s).
- Hosts its own WiFi access point named **`owl`** (password **`owlowlowl`**).
- Serves a thumbnail gallery + inline audio player at **`http://owl.local/`** so any phone or laptop joined to the AP can browse, download, or delete files.

There's no session-folder layout, no Python face/transcription pipeline, and no USB Mass Storage mode on this branch — those live on `main`.

## What gets stored on the SD

```text
/photos/
  img_000001.jpg
  img_000002.jpg
  ...
/audio/
  rec_000001.wav
  rec_000002.wav
  ...
```

Counters are persistent: at boot the firmware scans each directory and resumes from `max(existing) + 1`. Capped at `999999` per kind.

## Hardware required

- Seeed XIAO ESP32-S3 Sense with camera, microphone, and microSD expansion board.
- microSD card, **≤ 32 GB, FAT32**.
- USB-C cable that supports data.
- macOS or Linux dev machine with `arduino-cli` and `esptool` for flashing.

## Firmware setup

```bash
brew install arduino-cli
pip3 install esptool

arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
```

Flash:

```bash
./flash.sh
```

Required board target (`flash.sh` sets this automatically):

```text
esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default
```

`PSRAM=opi` is required for camera framebuffers + WiFi co-existence.

## Connect & download

1. Insert a FAT32 microSD card and power the board over USB.
2. Wait for the LED to start a slow heartbeat (~1 s on, 2 s off) — that's the AP being up.
3. On your Mac/phone, join the WiFi network **`owl`** with password **`owlowlowl`**.
   - Your device will say "Connected, no internet" — correct, the AP has no upstream.
4. Open **`http://owl.local/`** in a browser. (Or `http://192.168.4.1/` if mDNS isn't working.)
5. Press BOOT on the device — within ~5 s the new photo appears in the gallery. Hover any card to reveal **↓** (download) and **✕** (delete) buttons.

For raw curl access:

```bash
curl http://owl.local/list                                # JSON of all files
curl -o test.jpg http://owl.local/photo/img_000001.jpg    # download a photo
curl -o test.wav http://owl.local/audio/rec_000001.wav    # download a recording
curl -X DELETE http://owl.local/photo/img_000001.jpg      # delete one
```

## Buttons

| Gesture | Action |
| --- | --- |
| Short-press BOOT (< 2.5 s) | Capture one JPEG to `/photos/`. |
| Long-press BOOT (≥ 2.5 s) | Toggle audio recording to `/audio/`. First long-press starts; next stops. |

You can short-press to capture a photo while audio recording is in progress — both subsystems stay independent.

## LED indicators

| Pattern | Meaning |
| --- | --- |
| Off | Booting before AP comes up. |
| Slow heartbeat (100 ms on, 1900 ms off) | Idle — AP is up, ready for input. |
| Single 100 ms blink | Photo captured successfully (overlays heartbeat or recording). |
| Solid on | Audio recording in progress. |
| 5× 50 ms strobe | Capture or write error (camera, SD, or WAV). HTTP server stays up. |

**Permanent fault — LED blinks forever and the firmware never serves**

| Blink rate | What failed |
| --- | --- |
| ~4 Hz (toggles every 120 ms) | SD mount or `/photos`/`/audio` mkdir failed. Card present and FAT32? |
| ~1.2 Hz (toggles every 400 ms) | Camera init failed. Re-seat the camera ribbon cable. |
| ~0.7 Hz (toggles every 700 ms) | PDM mic init failed. |

## Serial monitor

The firmware logs at **115200 baud** over USB CDC.

```bash
ls /dev/cu.usbmodem*                                          # macOS
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200
```

(Replace the port with whatever `ls` printed. Exit with Ctrl-C.)

Useful boot-time markers:

```
[sd] mounted, ... MB total
[cam] ready
[mic] ready
[photos] resuming at img_000NNN.jpg
[audio]  resuming at rec_000NNN.wav
[wifi] AP up: SSID=owl IP=192.168.4.1
[mdns] http://owl.local/
[http] listening on :80
```

## Configuration knobs

In `owl_esp32.ino`:

| Setting | Default | Notes |
| --- | --- | --- |
| `AP_SSID` | `"owl"` | WiFi network name. |
| `AP_PASSWORD` | `"owlowlowl"` | Must be ≥ 8 chars for WPA2. **Hardcoded — visible to anyone reading this repo.** |
| `AUDIO_SAMPLE_RATE` | `16000` | Hz. 16-bit mono WAV. |
| `LONG_PRESS_MS` | `2500` | Hold time to trigger audio toggle. |
| `cfg.frame_size` | `FRAMESIZE_SVGA` | 800×600 JPEG. UXGA (1600×1200) is also supported but slower over WiFi. |
| `cfg.jpeg_quality` | `12` | 0–63, lower = better quality + larger file. |

## Important notes

- **AP password is in source.** Anyone who reads this repo knows it. Acceptable for a hobby device on a private 1-client AP; not acceptable for anything sensitive.
- **No internet uplink.** When you're joined to `owl` your device has no internet access until you switch back to your usual WiFi.
- **No real timestamps.** The board has no RTC and doesn't fetch NTP; files are sequentially numbered.
- **Power loss during audio.** WAV header is finalized on stop — a yank mid-recording leaves a malformed header. The PCM data is still on disk; can usually be recovered with `ffmpeg` or `sox`.
- **For face detection / transcription**, switch to the `main` branch — its session-folder layout matches the `software/` Python pipeline. The flat `/photos/` + `/audio/` layout on this branch isn't fed into that pipeline.
- `TODO.md` documents the step-by-step build of this branch from `main` for anyone wanting to understand or replay the design choices.
- `software/PROJECT.md` and `PROJECT.md` document the firmware + pipeline behavior on `main`.
