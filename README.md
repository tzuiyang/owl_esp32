# owl_esp32

`owl_esp32` is an end-to-end wearable recorder prototype for the Seeed XIAO ESP32-S3 Sense. The firmware records slow-FPS camera frames and microphone audio to a microSD card; the Python software pipeline turns those session folders into a best face crop, an audio transcript, and a local SQLite people/encounters database.

## What Gets Captured

- Video frame data is stored as individual JPEG images, not as a video file.
- Audio is stored as 16 kHz, 16-bit mono WAV.
- Each recording session is written to a separate folder on the microSD card.

Expected SD layout:

```text
/2026-04-22_001/
  image/
    img_000001.jpg
    img_000002.jpg
    ...
  audio/
    rec_001.wav
```

The date in the folder name is the firmware compile date, not real wall-clock time.

## Hardware Required

- Seeed XIAO ESP32-S3 Sense with camera, microphone, and microSD expansion board.
- microSD card, 32 GB or smaller, formatted FAT32.
- USB-C cable that supports data.
- macOS or Linux development machine with Python 3 and Arduino tooling.

## Firmware Setup

Install the ESP32 Arduino core:

```bash
brew install arduino-cli
pip3 install esptool

arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.8
```

Flash the board from the repo root:

```bash
./flash.sh
```

The required board target is:

```text
esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default
```

`PSRAM=opi` is required for camera framebuffers. `USBMode=default` is required for USB Mass Storage mode.

## Recording Workflow

1. Insert the FAT32 microSD card.
2. Power the board over USB.
3. Open a serial monitor if you want logs.
4. Short-press BOOT once to start recording.
5. Speak and point the camera at a test subject.
6. Short-press BOOT again to stop recording.
7. Confirm the serial log shows image writes and a closed WAV file.

During recording:

- Audio is continuously appended to `audio/rec_001.wav`.
- One JPEG frame is written to `image/` every 2 seconds.
- Each JPEG is closed immediately after writing.
- The WAV header is finalized when recording stops.

## Getting Data Off The Board

Long-press BOOT while the firmware is running. The board reboots into flash-drive mode and exposes the microSD card over USB.

On macOS, it usually appears in Finder as `NO NAME` unless the card has a label.

Copy the session folders into a local working directory:

```bash
mkdir -p data
cp -R /Volumes/NO\ NAME/2026-* data/
```

You can also process directly from the mounted card, but copying locally is safer because the software writes `best_face.jpg` and `owl.sqlite`.

## Software Setup

Set up the Python pipeline:

```bash
cd software
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

The first run downloads model files for InsightFace and Faster Whisper.

## Process Sessions

From the `software/` directory, process copied sessions:

```bash
.venv/bin/python -m owl process --input-dir ../data
```

What the pipeline does:

1. Walks every session folder in `--input-dir`.
2. Reads `image/` frames.
3. Detects and embeds faces with InsightFace.
4. Clusters faces within the session.
5. Picks the largest face cluster as the main subject.
6. Saves the best crop as `best_face.jpg`.
7. Reads `audio/rec_*.wav`.
8. Transcribes audio with Faster Whisper.
9. Writes people and encounter records to `owl.sqlite`.

Expected output per processed session:

```text
data/
  2026-04-22_001/
    image/
    audio/
    best_face.jpg
  owl.sqlite
```

Re-running the same command is idempotent. If a session path already exists in the database, it is skipped.

## Inspect Results

List recognized people:

```bash
cd software
.venv/bin/python -m owl list --db-path ../data/owl.sqlite
```

Show one person and their encounters:

```bash
.venv/bin/python -m owl show 1 --db-path ../data/owl.sqlite
```

Inspect the database directly:

```bash
sqlite3 ../data/owl.sqlite "SELECT id, name, first_met, last_met FROM people;"
sqlite3 ../data/owl.sqlite "SELECT id, session_dir, audio_paths, length(transcript) FROM encounters;"
```

## End-to-End Engineer Smoke Test

Use this checklist to verify the full system:

1. Flash succeeds with `./flash.sh`.
2. Serial output shows SD, camera, and microphone ready.
3. Short BOOT press starts recording.
4. At least two `img_*.jpg` files are written while recording.
5. Short BOOT press stops recording and closes the WAV.
6. Long BOOT press exposes the microSD as a USB drive.
7. Session folders copy successfully to `data/`.
8. `.venv/bin/python -m owl process --input-dir ../data` completes.
9. Each usable session gets `best_face.jpg`.
10. `../data/owl.sqlite` contains at least one `people` row and one `encounters` row.
11. `owl show 1` prints audio file paths and a transcript, or an empty transcript if the clip had no clear speech.

For face-only debugging, skip transcription:

```bash
cd software
.venv/bin/python -m owl process --input-dir ../data --no-transcribe
```

## Important Notes

- This project records still JPEG frames, not continuous video.
- The firmware does not upload data over Wi-Fi.
- The Python pipeline is offline and runs on the development machine.
- If power is lost during recording, JPEGs already written should remain readable. The WAV may need repair because its header is patched only when recording stops.
- `software/PROJECT.md` documents pipeline details and tuning flags.
- `PROJECT.md` documents firmware behavior, flashing details, and hardware notes.
