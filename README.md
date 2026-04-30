# owl_esp32 — `snapshot` branch

A Seeed XIAO ESP32-S3 Sense turned into a self-contained WiFi photo + audio device. No router, no cloud, no app — the board hosts its own WiFi network and serves a web gallery directly off its microSD card.

- **Short-press BOOT** → snap one JPEG.
- **Long-press BOOT** (≥ 2.5 s) → start a WAV recording. Long-press again to stop.
- **Connect to the `owl` WiFi** → browse, download, and delete files at `http://owl.local/`.

---

## Table of contents

1. [What this does](#what-this-does)
2. [Hardware you need](#hardware-you-need)
3. [End-to-end workflow](#end-to-end-workflow)
   - [First-time setup](#1-first-time-setup-once-per-machine--board)
   - [Capturing](#2-capturing-photos-and-audio)
   - [Retrieving from your laptop or phone](#3-retrieving-files-from-your-laptop-or-phone)
   - [Cleaning up](#4-cleaning-up)
4. [Buttons & LED reference](#buttons--led-reference)
5. [Web interface](#web-interface)
6. [HTTP API](#http-api-curl)
7. [Serial monitor (debugging)](#serial-monitor-debugging)
8. [Troubleshooting](#troubleshooting)
9. [Configuration knobs](#configuration-knobs)
10. [Limitations](#limitations)

---

## What this does

```
         ┌─────────────────────────┐
         │ XIAO ESP32-S3 Sense     │
         │  · OV2640 camera        │             ╔═════════╗
         │  · PDM mic              │  WiFi AP    ║         ║
   BOOT  │  · microSD              ├─────────────╢ Mac /   ║
   ────► │  · WiFi AP "owl"        │             ║ phone   ║
         │  · HTTP server :80      │             ║         ║
         └────────────┬────────────┘             ╚═════════╝
                      │
                  microSD card
              /photos/img_NNNNNN.jpg
              /audio/rec_NNNNNN.wav
```

- A short-press on BOOT captures one SVGA (800×600) JPEG.
- A long-press toggles a 16 kHz mono WAV recording.
- The board hosts a WPA2 WiFi access point named **`owl`** (password `owlowlowl`).
- Anything in `/photos/` and `/audio/` is browsable at **`http://owl.local/`** — thumbnails for photos, an inline audio player for recordings, with hover-to-reveal download (↓) and delete (✕) buttons.
- Photos auto-increment (`img_000001.jpg`, `img_000002.jpg`, ...). Audio recordings auto-increment too. Counters survive reboots.

---

## Hardware you need

| Item | Notes |
| --- | --- |
| Seeed XIAO ESP32-S3 Sense | Must be the **Sense** variant (camera + mic + microSD on the expansion board). |
| microSD card | **≤ 32 GB, FAT32**. Larger or exFAT cards do not reliably mount on this board. |
| USB-C cable | Must support data, not charging only. |
| Mac or Linux dev machine | For flashing once and serial debugging. After that, any device with WiFi + browser works (laptop, phone, tablet). |

---

## End-to-end workflow

### 1. First-time setup (once per machine + board)

**a. Install the toolchain on your Mac:**

```bash
brew install arduino-cli
pip3 install esptool

arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
```

**b. Format the microSD as FAT32** (use Disk Utility on macOS — pick MS-DOS (FAT)) and insert it into the Sense expansion board.

**c. Plug the XIAO into your Mac via USB-C.**

**d. Clone this branch and flash:**

```bash
git clone -b snapshot https://github.com/tzuiyang/owl_esp32.git
cd owl_esp32
./flash.sh
```

The flash script intentionally fails on its first pass (kicks the chip into ROM mode), then succeeds on the second pass. Look for `==> done. Firmware should be running.` at the end.

**e. Verify it booted.** Open a serial monitor in another terminal:

```bash
arduino-cli monitor -p /dev/cu.usbmodem* -c baudrate=115200
```

Press the **RESET** button on the XIAO to see the boot banner. Expected lines:

```
[sd] mounted, ... MB total
[cam] ready
[mic] ready
[photos] resuming at img_000001.jpg
[audio]  resuming at rec_000001.wav
[wifi] AP up: SSID=owl IP=192.168.4.1
[mdns] http://owl.local/
[http] listening on :80
```

If any of those say `failed` or you see a continuous LED blink instead of a slow heartbeat — jump to [Troubleshooting](#troubleshooting).

---

### 2. Capturing photos and audio

The board can be powered by USB or any 5 V source — once flashed, your Mac is no longer required.

**To take a single photo**

1. Make sure the LED is doing the slow heartbeat (one blink every ~2 s) — that means the AP is up and idle.
2. **Short-press BOOT.** The LED will blink once briefly to confirm the capture. The JPEG lands at `/photos/img_NNNNNN.jpg` on the SD.

To minimize blur, hold the camera steady for ~1 s around the press — `captureOnePhoto()` drops two frames before saving so the camera has time to refocus on the current scene.

**To record audio**

1. **Long-press BOOT** — hold the button for ≥ 2.5 s.
2. The LED switches to **solid on** when recording starts. Talk, sing, etc.
3. **Long-press BOOT again** to stop. The LED returns to the slow heartbeat. The WAV is finalized to `/audio/rec_NNNNNN.wav`.

You can short-press to capture a photo *while* recording — both subsystems run independently.

**Counters and persistence**

- File counters survive power cycles. After plugging back in, the next photo continues from `img_NNNNNN.jpg + 1` based on whatever's already on the SD.
- Capped at 999999 per kind. Past that the board logs and refuses to write rather than silently overwrite.

---

### 3. Retrieving files from your laptop or phone

This is the part that makes the device useful — no card-reader required, no USB cable.

**a. Join the `owl` WiFi network**

Click your Mac's WiFi icon (or your phone's WiFi settings) and pick **`owl`** from the list. Password: `owlowlowl`.

Your device will say "Connected, no internet". That is correct — the AP has no upstream uplink. To get internet back later, switch your WiFi back to your usual network.

**b. Open the gallery**

In any browser, go to:

```
http://owl.local/
```

If `owl.local` doesn't resolve (some networks/setups disable mDNS), use the IP directly:

```
http://192.168.4.1/
```

You'll see a dark-themed page with two sections: **Photos** (thumbnail grid) and **Audio** (stacked players).

**c. Browse, play, download, delete**

- **Click a thumbnail** → opens the full-size JPEG in a new tab.
- **Click play on an audio card** → streams the WAV from the device.
- **Hover any card** → ↓ (download) and ✕ (delete) buttons appear. ↓ saves the file to your Downloads folder with its original name. ✕ asks "Delete <filename>?" and removes the file from the SD on confirmation.

The page polls `/list` every 5 seconds, so any new captures you make on the device while the page is open appear automatically — no manual refresh needed.

---

### 4. Cleaning up

When you're done:

- **Stop any in-progress audio recording** (long-press BOOT) so the WAV header gets finalized cleanly. A yank without stopping leaves an unparsable header (the raw PCM data is still on disk and can be recovered with `ffmpeg`).
- **Pull the USB cable** to power off. There's no shutdown ritual — the firmware is stateless beyond the SD card files.
- **Reconnect your Mac/phone to your normal WiFi.** While joined to `owl` you have no internet.

If you want to wipe and start fresh: pop the microSD card out and delete `/photos/` and `/audio/` from your laptop.

---

## Buttons & LED reference

### Buttons

| Gesture | What happens |
| --- | --- |
| Short-press BOOT (< 2.5 s) | Capture one JPEG to `/photos/`. |
| Long-press BOOT (≥ 2.5 s) | Toggle audio recording: first long-press starts, next stops. |
| Both can interleave | A short-press during recording captures a photo without interrupting the WAV. |

### LED — normal states

| Pattern | Meaning |
| --- | --- |
| Off | Booting, before the AP comes up. |
| Slow heartbeat (~100 ms on, ~1.9 s off) | Idle. AP up, ready for input. |
| Single 100 ms blink (overlay) | A photo was just captured successfully. |
| Solid on | Audio recording in progress. |
| 5× 50 ms strobe | Capture or write error (camera, SD, or WAV). HTTP server stays up. |

### LED — fatal faults (firmware halts, only a power-cycle fixes it)

| Blink rate | What failed | Fix |
| --- | --- | --- |
| ~4 Hz (toggles every 120 ms) | SD mount or `/photos`/`/audio` mkdir | Insert/reseat card. Confirm ≤ 32 GB and FAT32. |
| ~1.2 Hz (toggles every 400 ms) | Camera init | Reseat the camera ribbon cable on the Sense board. |
| ~0.7 Hz (toggles every 700 ms) | PDM mic init | Reseat the mic — same ribbon cable typically. |

---

## Web interface

Available URLs once you're joined to `owl`:

| URL | Returns |
| --- | --- |
| `http://owl.local/` (or `/192.168.4.1/`) | The HTML gallery. |
| `http://owl.local/list` | JSON listing: `{"photos":[...], "audio":[...]}`. |
| `http://owl.local/photo/<name>` | One JPEG. `GET` to view, `DELETE` to remove. |
| `http://owl.local/audio/<name>` | One WAV. Same verbs. The actively-recording WAV returns 503. |

---

## HTTP API (curl)

For when you want to script it:

```bash
# list everything on the SD
curl http://owl.local/list

# pretty-print
curl -s http://owl.local/list | python3 -m json.tool

# download one photo
curl -o ~/Downloads/test.jpg http://owl.local/photo/img_000001.jpg

# download one audio recording
curl -o ~/Downloads/test.wav http://owl.local/audio/rec_000001.wav

# delete one photo
curl -X DELETE http://owl.local/photo/img_000001.jpg

# bulk download all photos to a local folder
mkdir -p ~/Downloads/owl
curl -s http://owl.local/list | python3 -c '
import json, sys, urllib.request, os
data = json.load(sys.stdin)
for n in data["photos"]:
    urllib.request.urlretrieve(f"http://owl.local/photo/{n}", f"{os.path.expanduser(\"~/Downloads/owl\")}/{n}")
    print(n)'
```

---

## Serial monitor (debugging)

The firmware logs at **115200 baud** over USB CDC. The board appears as `/dev/cu.usbmodem*` on macOS or `/dev/ttyACM*` on Linux.

```bash
ls /dev/cu.usbmodem*                                           # find the port
arduino-cli monitor -p /dev/cu.usbmodem101 -c baudrate=115200  # open monitor
```

Exit with **Ctrl-C**. Useful logged events:

```
[photo] saved /photos/img_000003.jpg (47821 bytes)        # successful capture
[audio] recording -> /audio/rec_000002.wav                 # recording started
[audio] stopped, 757760 PCM bytes                          # recording stopped
[boot] long-press detected (no action until TODO step 8)   # only on older builds
[wifi] AP up: SSID=owl IP=192.168.4.1                      # AP came up
[mdns] http://owl.local/                                   # mDNS registered
```

Quick sanity check: `bytes ÷ 32000 = duration in seconds` for audio (16 kHz × 16-bit mono).

---

## Troubleshooting

### `owl` doesn't show up in my WiFi list

- Wait 5–10 s after powering the board — the AP comes up after camera/mic init.
- Check the serial monitor for `[wifi] AP up`. If you see `softAP failed`, power-cycle.
- Make sure FQBN includes `PSRAM=opi`. Without PSRAM, WiFi + camera can't co-exist and softAP silently fails.

### I'm on `owl` but `http://owl.local/` won't load

- Try `http://192.168.4.1/` directly. If *that* works, mDNS is the issue (some networks block Bonjour multicast). The IP form always works.
- Check your Mac's WiFi icon to confirm you're really on `owl` and not silently falling back to a remembered network.

### I can ping `192.168.4.1` from a different WiFi network

Coincidence — `192.168.4.1` is a private IP that exists on millions of separate networks (NYU campus, home routers, hotspots). The TTL gives it away: ESP32 replies have `ttl=64`, distant devices have `ttl=251` or similar after several router hops. To be sure you're talking to the ESP32, run:

```bash
arp -a 192.168.4.1
```

The MAC should be `e8:f6:0a:8c:f5:c4` (or whatever was printed during your flash). If it's anything else, you're not actually on `owl`.

### Audio file shows `0:00 / 0:00` in the gallery

The duration appears once you press **play**. The audio element loads only metadata on page load (`preload="metadata"`); some browsers don't render the duration until playback begins.

### Photos are blurry

The camera's auto-exposure takes a couple of frames to settle, and any wearable motion during the capture window (~500 ms total) shows as blur. To get sharper shots:

- Hold the device still for ~1 s around the BOOT press.
- Aim at well-lit subjects — low light forces longer exposures.
- The `cfg.jpeg_quality` setting in `owl_esp32.ino` is `12`. Lowering to `8` improves quality slightly at the cost of larger file size.

### Flash fails with `No module named esptool`

Some Python venv is shadowing the system `esptool`. The project's `flash.sh` calls the standalone `esptool.py` binary on `$PATH` to dodge this — make sure `which esptool.py` returns a real path. If not, `pip3 install esptool` outside any venv.

### Flash's first pass logs an error

That's intentional. `arduino-cli upload` is run once and **expected to fail** — its DTR/RTS pulse parks the chip in ROM mode for the actual flash. Look for `==> done. Firmware should be running.` at the end of the second pass.

### LED is doing a fast continuous blink

Means a fatal init failed. Match the rate against the [LED fault table](#led--fatal-faults-firmware-halts-only-a-power-cycle-fixes-it) above to identify which subsystem.

### I want to wipe everything

Pop the microSD into your Mac. Drag `photos/` and `audio/` to the trash. Reinsert and power-cycle the board — the counters reset to 1.

---

## Configuration knobs

In `owl_esp32.ino`:

| Setting | Default | Notes |
| --- | --- | --- |
| `AP_SSID` | `"owl"` | WiFi network name. |
| `AP_PASSWORD` | `"owlowlowl"` | Must be ≥ 8 chars for WPA2. **Hardcoded in source — visible to anyone reading this repo.** |
| `AUDIO_SAMPLE_RATE` | `16000` | Hz. 16-bit mono WAV. |
| `LONG_PRESS_MS` | `2500` | Hold time to trigger audio toggle. |
| `cfg.frame_size` | `FRAMESIZE_SVGA` | 800×600. UXGA (1600×1200) also supported but slower over WiFi. |
| `cfg.jpeg_quality` | `12` | 0–63, lower number = higher quality + larger file. |
| `LED_HB_ON_MS` / `LED_HB_OFF_MS` | `100` / `1900` | Heartbeat timing. |

Recompile + reflash after any change (`./flash.sh`).

---

## Limitations

- **AP password is in source.** Acceptable for a hobby device on a private 1-client AP. Not acceptable for anything sensitive.
- **No internet uplink.** While joined to `owl`, your device has no internet.
- **No real-time clock.** No NTP, so files are sequentially numbered, not timestamped.
- **Power loss during recording** leaves the WAV header malformed (raw PCM is still on disk and recoverable with `ffmpeg`).
- **Single-client AP in practice.** The `WebServer` library is synchronous — if two clients hit it at the same time the second waits for the first.
- **Camera image quality is mediocre.** OV2640 + tiny sensor + indoor light = grainy, motion-sensitive shots. Good enough for "what was this person wearing"; not so good for face recognition without serious image processing.
- **For multi-session recording + face/transcription pipelines**, switch to the `main` branch — that has a totally different firmware and a Python pipeline (`software/`) for offline face clustering and Whisper transcription. Not present on this branch.

---

`TODO.md` documents the build narrative for this branch (Steps 1–10, all complete) — useful if you want to understand or replay the design choices.
