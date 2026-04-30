# TODO — `snapshot` branch: still-photo + WiFi-AP firmware

> **Status: complete.** All 10 steps were implemented and verified on hardware. This document is preserved as the design narrative. See `README.md` for end-user docs.

| Step | What | Status |
| --- | --- | --- |
| 1 | Single-shot photo, drop MSC + recording state machine | ✅ |
| 2 | WiFi AP `owl` at boot, LED heartbeat | ✅ |
| 3 | Minimal HTTP server (root → "owl_esp32 ready") | ✅ |
| 4 | `GET /list` returns JSON of photo filenames | ✅ |
| 5 | `GET /photo/<name>` streams JPEG, with path-traversal guard | ✅ |
| 6 | HTML gallery at `/`, 5 s polling, lazy-loaded thumbs | ✅ |
| 7 | mDNS → `owl.local` | ✅ |
| 8 | Long-press BOOT toggles audio recording, gallery shows audio | ✅ |
| 9 | Polish — counter caps, LED patterns, fault halts | ✅ |
| 10 | Documentation pass (this file + README rewrite) | ✅ |

**Bonus, not in original plan:**
- Per-file delete (`DELETE /photo/<name>`, `DELETE /audio/<name>`) + `✕` button on cards.
- Per-file download (`↓` button using HTML5 `download` attribute).
- Photo stale-frame fix (drop two frames before each capture so the snapshot reflects current scene, not last press).
- `flash.sh` switched to standalone `esptool.py` so it works regardless of which Python venv is active.

---

Goal: replace the always-on audio+image recorder with a "press BOOT to take a single JPEG, long-press to start/stop audio recording, download both from a self-hosted WiFi AP" flow.

## Target user flow

1. Power the board over USB.
2. Mac (or phone) sees a new WiFi network **`owl`** in its scan list.
3. Connect with password **`owlowlowl`**.
4. Open `http://owl.local/` (or `http://192.168.4.1/`) in Safari.
5. **Short-press BOOT** → ESP32 captures one JPEG, saves to SD, the gallery page picks it up.
6. **Long-press BOOT (≥2.5 s)** → starts a WAV recording. Long-press again to stop. WAV appears in the gallery.
7. Click any photo or audio file in the page to download it to the Mac.

No session folders. No reboot-into-MSC ritual. The board is just an HTTP photo + audio server with a physical shutter and a hold-to-record button.

## Design decisions (locked)

| Decision | Choice | Rationale |
| --- | --- | --- |
| WiFi mode | AP (board hosts the network) | Works anywhere, no router. |
| SSID / password | `owl` / `owlowlowl` | Per spec. Hardcoded in `owl_esp32.ino`. |
| Storage | SD card, FAT32, ≤32 GB | Already wired up and proven. |
| Photo path scheme | `/photos/img_NNNNNN.jpg`, monotonically increasing | No RTC, so no timestamp prefix. NNNNNN persists across reboots by scanning the dir at boot. |
| Audio path scheme | `/audio/rec_NNNNNN.wav`, monotonically increasing | Same scheme as photos. |
| Frame size | SVGA (800×600), JPEG quality 10 | UXGA tempting but slow over WiFi-AP; SVGA is the right first-cut. |
| Audio format | 16 kHz, 16-bit mono WAV | Same as `main` branch — Whisper expects this. |
| HTTP lib | `WebServer.h` (sync, ships with arduino-esp32) | Simpler than async; fine for single-client downloads. |
| mDNS hostname | `owl.local` | Quality-of-life, ~10 lines of code. |
| Short-press BOOT | One photo. Allowed at any time, including while audio is recording. | Per spec. |
| Long-press BOOT (≥2.5 s) | Toggle audio recording start/stop. | Per spec. |
| LED meaning | Slow heartbeat (100/1900 ms) = AP up & idle. Solid on = audio recording active. Single 100 ms blink = photo captured (overlays heartbeat or recording). 5× 50 ms strobe = capture/write error. Solid permanent = fatal init failure. | Visual debug without serial. |

## Build order (each step is independently flashable + verifiable)

Each step has a **build it** section (what changes in the firmware) and a **verify it** section (how you confirm it works before moving on). If a step's verify fails, fix before proceeding — later steps assume it.

---

### Step 1 — Strip down: single-shot photo capture, no AP yet

**Build it**

- In `owl_esp32.ino`:
  - Remove the recording state machine and the session-folder logic. Photos go in `/photos/` at the SD root.
  - Replace the BOOT short-press handler with: short-press → call `captureOnePhoto()` once → return.
  - Long-press BOOT is reserved for Step 8 (audio recording). Until then it can be a no-op.
  - Remove the long-press → MSC reboot path entirely. No MSC mode on this branch.
  - At boot, scan `/photos/` to find the highest existing `img_NNNNNN.jpg` and start `g_nextPhotoId` from there + 1.
  - Keep camera init, SD-mount retry ladder, and LED helpers.
  - **Keep** the microphone / I2S / WAV code in the file (or a separate `audio.cpp` if you want to split it). Do not call its init from `setup()` yet — we re-wire it in Step 8. The point: don't delete code we'll need next week.

**Verify it**

1. `./flash.sh` succeeds.
2. Serial log on boot shows: SD mounted, camera ready, `[photos] resuming at img_NNNNNN.jpg`, `ready — short-press BOOT for photo`.
3. Press BOOT once. Serial log shows `[photo] saved /photos/img_000001.jpg (NNN bytes)`.
4. Press BOOT again. Serial shows `img_000002.jpg`.
5. Power-cycle. First press writes `img_000003.jpg` (counter persisted via dir scan, **not** wiped).
6. Pop the SD into your Mac and confirm the JPEGs open and are SVGA.
7. LED behavior: dark on idle, single 100 ms blink per successful capture.

---

### Step 2 — Bring up the WiFi AP at boot, no HTTP yet

**Build it**

- Add `#include <WiFi.h>`.
- In `setup()`, after camera init:
  ```cpp
  WiFi.softAP("owl", "owlowlowl");
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[wifi] AP up: SSID=owl IP=%s\n", apIP.toString().c_str());
  ```
- LED idle pattern → slow heartbeat (100 ms on, 1900 ms off).

**Verify it**

1. Flash, watch serial — expect `[wifi] AP up: SSID=owl IP=192.168.4.1`.
2. On the Mac, click the WiFi icon: **`owl`** appears.
3. Connect with `owlowlowl`. Mac shows "Connected, no internet" — correct, the AP has no upstream.
4. `ping 192.168.4.1` → 4/4 replies, latency ~5–20 ms.
5. LED is doing the slow heartbeat.
6. Confirm Step 1 still works — short-press BOOT writes a photo while the AP is up.

**Common gotcha**: if the SSID never appears, log `psramFound()` — should be `true`. Also confirm the FQBN still has `PSRAM=opi`.

---

### Step 3 — Minimal HTTP server, root route returns "hello"

**Build it**

- `#include <WebServer.h>` and `WebServer g_http(80);`.
- In `setup()`, after `WiFi.softAP(...)`:
  ```cpp
  g_http.on("/", []() {
    g_http.send(200, "text/plain", "owl_esp32 ready");
  });
  g_http.begin();
  ```
- In `loop()`, call `g_http.handleClient();` every iteration.

**Verify it**

1. Mac connected to `owl`.
2. `curl -v http://192.168.4.1/` → `HTTP/1.1 200 OK`, body `owl_esp32 ready`.
3. BOOT-button capture from Step 1 still works while the server is running.
4. Disconnect Mac, reconnect, `curl` again — works without rebooting the board.

---

### Step 4 — `/list` returns JSON of photo filenames

**Build it**

- Route `/list` walks `/photos/` and returns `["img_000001.jpg","img_000002.jpg",...]`.
- Audio entries get added in Step 8; for now this is photos-only.
- Keep the iteration tight — use `dir.openNextFile()` and close each file as you go.

**Verify it**

1. Take 2 photos via BOOT.
2. `curl http://192.168.4.1/list` → `["img_000001.jpg","img_000002.jpg"]` (order may vary).
3. Take a 3rd photo. `/list` now has 3 entries.
4. Empty SD: response is `[]`.

---

### Step 5 — `/photo/<name>` streams the JPEG

**Build it**

- Use `g_http.onNotFound(...)` to dispatch on `/photo/<name>`.
- Resolve to `/photos/<name>` on SD.
- **Reject** any name containing `/` or `..` — return 404. Path traversal off `/photos/` must not be possible.
- Use `g_http.streamFile(f, "image/jpeg")`.

**Verify it**

1. Pick any name from `/list`, e.g. `img_000001.jpg`.
2. `curl -o /tmp/test.jpg http://192.168.4.1/photo/img_000001.jpg && open /tmp/test.jpg` → photo opens in Preview.
3. `curl -i http://192.168.4.1/photo/nope.jpg` → `404`.
4. `curl -i http://192.168.4.1/photo/../foo` → `404` (traversal rejected — **not** a 200 reading something outside `/photos/`).
5. From Safari, paste `http://192.168.4.1/photo/img_000001.jpg` → renders inline.

---

### Step 6 — HTML index page with click-to-view gallery

**Build it**

- Replace `/` so it returns inline HTML that:
  - Fetches `/list` via `fetch()`.
  - Renders each name as a clickable link to `/photo/<name>`.
  - Polls `/list` every 5 s so new captures appear without a manual refresh.
- Inline as a `const char[]` template; no SD-served static assets.
- Audio entries get added in Step 8; for now the page is photos-only.

**Verify it**

1. Mac on `owl`, open `http://192.168.4.1/` in Safari.
2. Page renders, shows the photos taken so far as clickable links.
3. Click a name → opens the JPEG in a new tab.
4. While the page is open, press BOOT on the device. Within ~5 s, the new photo name appears in the list.

---

### Step 7 — mDNS hostname `owl.local`

**Build it**

- `#include <ESPmDNS.h>` and after `WiFi.softAP(...)`:
  ```cpp
  if (MDNS.begin("owl")) Serial.println("[mdns] owl.local");
  else Serial.println("[mdns] start failed");
  MDNS.addService("http", "tcp", 80);
  ```

**Verify it**

1. Mac connected to `owl`.
2. `ping owl.local` resolves to the AP IP.
3. `http://owl.local/` opens in Safari and shows the gallery from Step 6.
4. From an iPhone on `owl`, `http://owl.local` also works (Bonjour is the same protocol).

---

### Step 8 — Long-press BOOT toggles audio recording

**Build it**

- Re-enable mic/I2S init in `setup()`, after camera init: `initMic()` returns false → solid-on fault LED.
- Add audio state: `bool g_recording = false; uint32_t g_nextAudioId = 1;`. At boot, scan `/audio/` for highest `rec_NNNNNN.wav` and resume `g_nextAudioId` from there.
- BOOT button handler:
  - Short tap (released before 2500 ms) → `captureOnePhoto()` (unchanged from Step 1).
  - Held ≥ 2500 ms → `toggleRecording()`. First long-press starts recording; next long-press stops.
- `toggleRecording()`:
  - Start: open `/audio/rec_NNNNNN.wav`, write a placeholder WAV header, begin pulling I2S samples in `loop()` and appending. LED → solid on.
  - Stop: stop I2S, finalize the WAV header (patch RIFF/data lengths), close file. LED → back to slow heartbeat.
- During recording, `loop()` must still call `g_http.handleClient()` and still allow short-press photo captures. The mic read loop should be cooperative, not blocking.
- Extend `/list` to return both photos and audio:
  ```json
  {"photos":["img_000001.jpg",...], "audio":["rec_000001.wav",...]}
  ```
  (This is a breaking change to the response shape from Step 4 — update the gallery JS in this same step.)
- Add `/audio/<name>` route, mirror of `/photo/<name>`, served as `audio/wav`. Same path-traversal guard.
- Update the gallery HTML to also list audio links (each name becomes an `<a href="/audio/<name>">` rendering an inline `<audio controls>` would be nicer, but a plain link is fine for first cut).

**Verify it**

1. Boot board, confirm mic init logged successfully and AP/HTTP/photos still work.
2. Long-press BOOT (hold ~3 s). LED goes solid on. Serial: `[audio] recording rec_000001.wav`.
3. Talk for ~10 s.
4. Long-press BOOT again. LED → heartbeat. Serial: `[audio] stopped, NNN bytes`.
5. While recording: short-press BOOT once. LED briefly blinks (overlay), photo saved, recording continues. Confirm `img_NNN.jpg` was written and the WAV is still growing.
6. After stopping, `curl http://192.168.4.1/list` → JSON object with both `photos` and `audio` arrays.
7. `curl -o /tmp/test.wav http://192.168.4.1/audio/rec_000001.wav && afplay /tmp/test.wav` → you hear what you said.
8. Reload `http://owl.local/` — gallery shows photos and audio files.
9. Power-cycle, long-press BOOT → next file is `rec_000002.wav` (counter persisted via dir scan).
10. Edge: long-press immediately followed by power loss — confirm the half-finalized WAV is at least readable (header may be wrong, but raw PCM is intact). `afplay` may fail; this is acceptable.

**Common gotchas**

- Don't try to `streamFile` on a WAV that's still being written. Either: (a) only show finalized files in `/list` (track an "open recording" flag and exclude the current one), or (b) return 503 if the requested file is the active recording.
- I2S buffer overruns will show up as serial spam during heavy WiFi activity. If it's bad, drop the I2S sample rate or queue size — but try the naive version first.

---

### Step 9 — Polish & error handling

**Build it**

- LED patterns finalized:
  - Slow heartbeat (100 ms on / 1900 ms off): AP up, idle.
  - Solid on: audio recording.
  - Single 100 ms blink: photo captured (overlays heartbeat or solid-on).
  - 5× 50 ms strobe: capture or write error.
  - Solid permanent: fatal init failure (SD or camera or mic).
- Defensive checks: bail with a fault LED if SD `/photos/` or `/audio/` can't be created at boot.
- Cap `g_nextPhotoId` and `g_nextAudioId` at 999999 — log and refuse to write rather than wrapping silently.
- Confirm `loop()` runs `g_http.handleClient()` at least once every ~50 ms even during a capture or while recording. A long mic read should be split into smaller chunks.

**Verify it**

1. Pull the SD card, reboot — board halts with solid LED, serial says SD failed.
2. Reinsert SD, reboot, normal slow heartbeat.
3. Block the camera lens, capture — photo still saves (just dark). LED single-blink.
4. Fill the SD to 99% (intentionally), capture — write fails, you see the 5× strobe and a serial error, but AP/HTTP server stays up and existing files are still downloadable.
5. Long-record (~5 min) while occasionally short-pressing for photos and reloading the gallery in Safari. Nothing crashes; no I2S overflow spam in serial.

---

### Step 10 — Documentation pass (only when 1–9 are green)

- Update `README.md`:
  - Replace "Recording Workflow" section with "WiFi gallery workflow".
  - Update the LED Indicators table to match Step 9.
  - Note `owl` / `owlowlowl` AP credentials and that they're hardcoded.
  - Note that this branch doesn't support MSC mode — point users to `main` for the MSC + multi-modal recorder firmware.
- Add a tiny "Connect & download" section showing the Mac-side flow (join WiFi, open `owl.local`).

---

## Notes & deferred

- **AP password in source** is fine for a hobby device but visible to anyone reading this public repo. Future work could move it to NVS-configurable via a one-time setup BLE step or USB serial command. Out of scope for this branch.
- **Battery / sleep**: not addressed here. Device is USB-powered. Light-sleep with WiFi AP up is non-trivial.
- **Remote shutter on the gallery page**: explicitly deferred — physical BOOT only for now. Easy to add later as a `POST /capture` route that calls the same `captureOnePhoto()`.
