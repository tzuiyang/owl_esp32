# owl_esp32

A tiny audio+image recorder that runs on a **Seeed XIAO ESP32-S3 Sense** and
writes everything to a microSD card. When you want to review what was
recorded you don't need an SD card reader — the board itself exposes the
microSD over USB as a flash drive.

---

## What it does

- **Record**: press BOOT once to start, once more to stop. While recording it
  streams audio from the onboard PDM microphone (16 kHz / 16-bit mono WAV)
  and snaps a JPEG from the camera every 2 seconds (SVGA 800×600).
- **Browse**: long-press BOOT (~3 s) to reboot the board into *flash drive
  mode*. The microSD appears in Finder as a USB drive (labelled "NO NAME"
  unless you've labelled it). No ejecting the card, no card reader.
- **Organise on disk**: every recording creates a new session folder named
  `YYYY-MM-DD_NNN` where the date is the firmware's build date and `NNN`
  auto-increments so each conversation stays separate.

---

## Hardware

- **Seeed XIAO ESP32-S3 Sense** (the module **plus** the Sense expansion
  board with camera + microphone + microSD slot).
- **microSD card**, **≤ 32 GB, formatted FAT32**. Bigger / exFAT cards do
  not reliably talk to the ESP32-S3's SDMMC controller.
- **USB-C cable** to your laptop.

Pin map (already baked into the sketch, listed here for reference):

| Function             | GPIO              |
| -------------------- | ----------------- |
| Camera (OV2640)      | 10–18, 38–40, 47, 48 — see `camera_pins.h` |
| Camera SCCB SDA/SCL  | 40 / 39           |
| PDM microphone CLK   | 42                |
| PDM microphone DATA  | 41                |
| microSD CLK          | 7                 |
| microSD CMD          | 9                 |
| microSD D0           | 8                 |
| BOOT button          | 0                 |
| User LED             | 21 (active LOW)   |

---

## Using the board

### First-time setup

1. Insert a FAT32 microSD into the Sense expansion board.
2. Plug the XIAO into your laptop via USB-C.
3. Flash the firmware (see *Building & flashing* below).

### Everyday workflow

| Action                                  | How                                                       |
| --------------------------------------- | --------------------------------------------------------- |
| **Start / stop a recording**            | Short tap BOOT.                                           |
| **View recorded files on your laptop**  | Long-press BOOT for ~3 s. LED flashes 10× to acknowledge, the board reboots into flash-drive mode. A "NO NAME" drive appears in Finder. |
| **Go back to recording**                | Eject the drive in Finder **first**, then short-tap BOOT. |

Status LED:

- **Off** — idle (recorder mode, nothing recording).
- **Solid on** — recording in progress.
- **10 quick flashes** — long-press acknowledged, about to reboot into MSC.
- **Slow blink (~1 Hz)** — flash-drive mode active.

### SD card layout

```
/2026-04-22_001/
    image/
        img_000001.jpg
        img_000002.jpg
        ...
    audio/
        rec_001.wav
/2026-04-22_002/
    ...
```

- `YYYY-MM-DD` is the firmware's compile date — not a wall-clock time. If
  you want true real-time dates, add NTP + Wi-Fi credentials (not wired up
  by default).
- `NNN` starts at `001` for each compile-date prefix and skips over any
  folders already on the card for the same date.
- Each short-tap start/stop cycle creates one session folder. Image filenames
  start at `img_000001.jpg`, and the audio file is `audio/rec_001.wav`.

---

## Building & flashing

### Requirements

- `arduino-cli` (install via Homebrew: `brew install arduino-cli`)
- Python 3 with `esptool` (install: `pip3 install esptool`)
- The ESP32 board package for Arduino:
  ```bash
  arduino-cli config add board_manager.additional_urls \
    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  arduino-cli core update-index
  arduino-cli core install esp32:esp32@3.3.8
  ```

### Flash it

Just run the script:

```bash
./flash.sh
```

That's it. The script compiles, handles the reset dance, and reports "done".

### Board settings (FQBN)

If you flash from the Arduino IDE or another tool, the target must be:

```
esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default
```

The critical flags are `PSRAM=opi` (required for the camera framebuffers)
and `USBMode=default` (TinyUSB — needed for USB Mass Storage).

---

## Files in this project

```
owl_esp32.ino    Main sketch: recorder + flash-drive mode.
camera_pins.h    OV2640 pin map for the Sense expansion board.
flash.sh         Build + flash wrapper that works around the reset quirk.
_test_msc/       Minimal MSC-only sketch (kept for reference, not needed).
_test_blink/     Minimal TinyUSB-CDC sketch (kept for reference, not needed).
PROJECT.md       This file.
```

---

## Under the hood: gotchas worth remembering

### Flashing: `--after hard-reset` is broken, use `--after watchdog-reset`

On the XIAO ESP32-S3's native USB-Serial/JTAG interface, `esptool`'s
standard `--after hard-reset` toggles GPIO0 in a way that re-enters the ROM
download mode (`boot:0x0`) instead of running the freshly flashed app.
`--after watchdog-reset` uses an internal watchdog to reset the CPU without
touching the strapping pins — the chip boots normally. `flash.sh` handles
this automatically.

### We cannot use "hold BOOT while plugging in" as a mode trigger

On ESP32-S3 the ROM samples GPIO0 at power-on to decide between SPI boot
and flash-download mode. Holding the BOOT button while plugging in USB
therefore lands the chip in flash-download mode **before our firmware gets
to run at all** — the user just sees the red power LED and nothing else.
That's why the mode selector is "long-press while already running" and not
"hold at power-on".

### Mode flag lives in NVS, not RTC memory

arduino-esp32 3.3.8's startup path zeroes `RTC_DATA_ATTR` and (in our
testing) doesn't preserve `RTC_NOINIT_ATTR` reliably across `esp_restart()`
either. NVS (`Preferences`) is flash-backed and survives every reset
including cold power-on, which is what we actually want here.

### Camera framebuffer overflow spam

The camera is configured with `CAMERA_GRAB_WHEN_EMPTY` + `fb_count = 1` so
the DMA only fills a buffer when we ask for a frame. The alternative
(`CAMERA_GRAB_LATEST` + `fb_count = 2`) keeps rolling and floods the
serial log with `cam_hal: FB-OVF` warnings because we only consume frames
every 2 s.

### SD mount reliability

Cold boots usually mount first try; soft resets sometimes need the SD
slot's voltage to settle. We retry up to 8× with the first two attempts
at the slow `SDMMC_FREQ_PROBING` (400 kHz) clock, then step up to
`SDMMC_FREQ_DEFAULT` (20 MHz). If mount still fails after retries, the
LED double-flashes fast forever — check the card is seated, is ≤ 32 GB,
and is formatted FAT32.

### MSC + FAT are mutually exclusive

In flash-drive mode we bypass the Arduino `SD_MMC` wrapper and talk to
the card through the raw `sdmmc_host_*` API. FAT is never mounted on the
ESP32 side while the Mac owns the disk, which avoids double-caching and
FAT corruption.
