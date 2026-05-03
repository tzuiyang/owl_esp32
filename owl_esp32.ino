// owl_esp32 — snapshot branch
//
// Short-press BOOT → capture one JPEG to /photos/img_NNNNNN.jpg on the SD card.
// Long-press BOOT  → toggle WAV recording to /audio/rec_NNNNNN.wav (start/stop).
// WiFi AP "owl"    → exposes a thumbnail gallery + audio player at http://192.168.4.1/.
//
// Target board: Seeed XIAO ESP32-S3 Sense
// FQBN: esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <vector>
#include "esp_camera.h"
#include "camera_pins.h"

// --------- user-tweakable settings ----------
static const uint32_t AUDIO_SAMPLE_RATE = 16000;  // 16 kHz mono 16-bit WAV (Step 8)
static const int      PIN_BUTTON        = 0;      // BOOT button on XIAO S3
static const int      PIN_LED           = LED_BUILTIN;   // GPIO21 on XIAO S3
static const int      LED_ON_LEVEL      = LOW;           // active-LOW
static const int      LED_OFF_LEVEL     = HIGH;
static const uint32_t LONG_PRESS_MS     = 2500;

// --------- SD pins for XIAO ESP32-S3 Sense (SDMMC 1-bit) ----------
static const int SD_CLK = 7;
static const int SD_CMD = 9;
static const int SD_D0  = 8;

// --------- PDM mic pins (used in Step 8) ----------
static const int PDM_CLK  = 42;
static const int PDM_DATA = 41;

// --------- Photo state ----------
static const char* PHOTOS_DIR = "/photos";
static uint32_t    g_nextPhotoId = 1;

// --------- Audio state ----------
static const char* AUDIO_DIR = "/audio";
static uint32_t    g_nextAudioId  = 1;
static bool        g_recording    = false;
static File        g_wavFile;
static uint32_t    g_wavDataBytes = 0;

// --------- WiFi AP ----------
static const char* AP_SSID     = "owl";
static const char* AP_PASSWORD = "owlowlowl";   // ≥8 chars for WPA2

// --------- HTTP server ----------
static WebServer g_http(80);

// MIME-type guess for serving static UI files from SD's /web/ folder.
static const char* contentTypeFor(const String& path) {
  int dot = path.lastIndexOf('.');
  if (dot < 0) return "application/octet-stream";
  String ext = path.substring(dot);
  if (ext == ".html" || ext == ".htm") return "text/html";
  if (ext == ".css")   return "text/css";
  if (ext == ".js")    return "application/javascript";
  if (ext == ".json")  return "application/json";
  if (ext == ".ico")   return "image/x-icon";
  if (ext == ".png")   return "image/png";
  if (ext == ".svg")   return "image/svg+xml";
  return "application/octet-stream";
}

// --------- Face-recognition annotations (RAM only) ----------
// Populated by the host-side watcher via POST /annotate. Cleared on reboot.
struct Annotation {
  String filename;
  String name;
  float  dist;
};
static std::vector<Annotation> g_annotations;

// Gallery page served at /. Polls /list every 5 s so new photos & audio
// appear without a manual reload. DOM is built with createElement +
// textContent + encodeURIComponent — never innerHTML — so a stray filename
// on the SD can't inject script.
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>owl</title>
<style>
 body{font:14px/1.4 system-ui,sans-serif;margin:1.5em;background:#0a0a0a;color:#eee}
 h1{font-size:1.1em;font-weight:600;margin:0 0 .8em}
 h2{font-size:.95em;font-weight:600;margin:1.5em 0 .6em;opacity:.8}
 .count{opacity:.6;font-weight:400}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:.6em}
 .alist{display:flex;flex-direction:column;gap:.5em;max-width:640px}
 .card{position:relative;border:1px solid #222;background:#111}
 .card:hover{background:#1a1a2a;border-color:#345}
 .card .view{display:block;text-decoration:none;color:#9cf}
 .card img{display:block;width:100%;height:auto;background:#000}
 .card audio{display:block;width:100%}
 .meta{display:flex;align-items:center;gap:.4em;padding:.35em .5em .4em}
 .meta .name{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-family:ui-monospace,monospace;font-size:.8em;color:#bbb}
 .toolbar{display:flex;gap:.25em;opacity:0;transition:opacity .15s}
 .card:hover .toolbar{opacity:1}
 .toolbar a, .toolbar button{display:inline-flex;align-items:center;justify-content:center;width:1.6em;height:1.6em;border:0;border-radius:50%;background:rgba(255,255,255,.08);color:#eee;font-size:.9em;text-decoration:none;cursor:pointer;line-height:1}
 .toolbar .dl:hover{background:#363}
 .toolbar .del:hover{background:#a33}
 .annot{padding:.35em .5em;font-size:.78em;background:rgba(80,180,80,.12);color:#9f9;border-top:1px solid #2a4;font-family:ui-monospace,monospace;text-align:center}
 .empty{opacity:.5;font-style:italic;padding:.4em 0}
</style></head><body>
<h1>owl</h1>
<h2>Photos <span id="pc" class="count"></span></h2>
<div id="gp" class="grid"></div>
<h2>Audio <span id="ac" class="count"></span></h2>
<div id="ga" class="alist"></div>
<script>
function makeToolbar(card, url, name, recountFn){
  const t = document.createElement('div'); t.className='toolbar';
  const dl = document.createElement('a');
  dl.className='dl'; dl.href=url; dl.download=name; dl.title='download'; dl.textContent='↓';
  dl.addEventListener('click', e => e.stopPropagation());
  const del = document.createElement('button');
  del.className='del'; del.type='button'; del.title='delete'; del.textContent='✕';
  del.addEventListener('click', async (e) => {
    e.preventDefault(); e.stopPropagation();
    if(!confirm('Delete ' + name + '?')) return;
    del.disabled = true;
    try{
      const r = await fetch(url, {method:'DELETE'});
      if(r.ok){ card.remove(); recountFn(); }
      else { alert('delete failed: HTTP ' + r.status); del.disabled = false; }
    }catch(err){ alert('delete failed: ' + err); del.disabled = false; }
  });
  t.appendChild(dl); t.appendChild(del);
  return t;
}
function makeMeta(card, url, name, recountFn){
  const m = document.createElement('div'); m.className='meta';
  const cap = document.createElement('span'); cap.className='name'; cap.textContent=name;
  m.appendChild(cap); m.appendChild(makeToolbar(card, url, name, recountFn));
  return m;
}
function makePhotoCard(n, recountFn, annot){
  const url = '/photo/' + encodeURIComponent(n);
  const card = document.createElement('div'); card.className='card';
  const a = document.createElement('a'); a.className='view'; a.href=url; a.target='_blank';
  const img = document.createElement('img'); img.src=url; img.loading='lazy'; img.alt=n;
  a.appendChild(img);
  card.appendChild(a); card.appendChild(makeMeta(card, url, n, recountFn));
  if (annot && annot.name) {
    const conf = Math.max(0, Math.min(100, Math.round((1 - annot.dist) * 100)));
    const tag = document.createElement('div');
    tag.className = 'annot';
    tag.textContent = annot.name + ' — ' + conf + '% match';
    card.appendChild(tag);
  }
  return card;
}
function makeAudioCard(n, recountFn){
  const url = '/audio/' + encodeURIComponent(n);
  const card = document.createElement('div'); card.className='card';
  const player = document.createElement('audio'); player.controls=true; player.preload='metadata'; player.src=url;
  card.appendChild(player); card.appendChild(makeMeta(card, url, n, recountFn));
  return card;
}
async function refresh(){
  try{
    const [listR, annotR] = await Promise.all([
      fetch('/list', {cache:'no-store'}),
      fetch('/annotations', {cache:'no-store'}),
    ]);
    const data = await listR.json();
    const annot = await annotR.json();
    const gp = document.getElementById('gp');
    const ga = document.getElementById('ga');
    const pc = document.getElementById('pc');
    const ac = document.getElementById('ac');
    const recount = () => {
      pc.textContent = '(' + Array.from(gp.children).filter(c => !c.classList.contains('empty')).length + ')';
      ac.textContent = '(' + Array.from(ga.children).filter(c => !c.classList.contains('empty')).length + ')';
    };
    gp.replaceChildren();
    ga.replaceChildren();
    if(!data.photos.length){
      const d = document.createElement('div'); d.className='empty';
      d.textContent='no photos yet — short-press BOOT to capture';
      gp.appendChild(d);
    } else {
      for(const n of data.photos) gp.appendChild(makePhotoCard(n, recount, annot[n]));
    }
    if(!data.audio.length){
      const d = document.createElement('div'); d.className='empty';
      d.textContent='no audio yet — long-press BOOT (≥2.5s) to start, again to stop';
      ga.appendChild(d);
    } else {
      for(const n of data.audio) ga.appendChild(makeAudioCard(n, recount));
    }
    pc.textContent = '(' + data.photos.length + ')';
    ac.textContent = '(' + data.audio.length + ')';
  }catch(e){}
}
refresh(); setInterval(refresh, 5000);
</script></body></html>
)HTML";

// --------- LED heartbeat (non-blocking) ----------
// Idle pattern when AP is up: 100 ms on, 1900 ms off. Implemented in loop()
// via millis() so it doesn't block button reads or future HTTP handling.
static const uint32_t LED_HB_ON_MS  = 100;
static const uint32_t LED_HB_OFF_MS = 1900;
static uint32_t       g_ledHbT      = 0;
static bool           g_ledHbOn     = false;

// --------- Button debounce / press tracking ----------
static int      g_btnStable    = HIGH;
static int      g_btnRaw       = HIGH;
static uint32_t g_btnRawChgAt  = 0;
static uint32_t g_btnPressedAt = 0;
static bool     g_longFired    = false;

// PDM mic — initialized in setup(), drained from loop() while g_recording.
I2SClass I2S;

// ============================================================
// LED helpers
// ============================================================

static void ledFlash(uint32_t ms) {
  digitalWrite(PIN_LED, LED_ON_LEVEL);
  delay(ms);
  digitalWrite(PIN_LED, LED_OFF_LEVEL);
}

static void ledStrobe(int times, uint32_t periodMs) {
  for (int i = 0; i < times; ++i) {
    digitalWrite(PIN_LED, LED_ON_LEVEL);  delay(periodMs / 2);
    digitalWrite(PIN_LED, LED_OFF_LEVEL); delay(periodMs / 2);
  }
}

// Halt-blink pattern used for fatal init failures. Never returns.
static void haltBlinking(uint32_t periodMs) {
  while (true) {
    digitalWrite(PIN_LED, !digitalRead(PIN_LED));
    delay(periodMs);
  }
}

// Non-blocking heartbeat. Call from loop() every iteration; toggles the LED
// on the LED_HB_ON_MS / LED_HB_OFF_MS schedule. ledFlash() interrupts this
// briefly during photo captures; the heartbeat resumes on the next tick.
// While recording, the LED stays solid on instead of heartbeating.
static void ledHeartbeatTick() {
  if (g_recording) {
    digitalWrite(PIN_LED, LED_ON_LEVEL);
    return;
  }
  uint32_t now = millis();
  uint32_t target = g_ledHbOn ? LED_HB_ON_MS : LED_HB_OFF_MS;
  if (now - g_ledHbT >= target) {
    g_ledHbT  = now;
    g_ledHbOn = !g_ledHbOn;
    digitalWrite(PIN_LED, g_ledHbOn ? LED_ON_LEVEL : LED_OFF_LEVEL);
  }
}

// ============================================================
// WAV header utilities (used in Step 8)
// ============================================================

static void writeWavHeaderPlaceholder(File& f) {
  const uint32_t sampleRate = AUDIO_SAMPLE_RATE;
  const uint16_t numCh      = 1;
  const uint16_t bitsPer    = 16;
  const uint32_t byteRate   = sampleRate * numCh * bitsPer / 8;
  const uint16_t blockAlign = numCh * bitsPer / 8;
  uint8_t h[44] = {0};
  memcpy(h + 0,  "RIFF", 4);
  memcpy(h + 8,  "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);
  h[16] = 16;
  h[20] = 1;
  h[22] = (uint8_t)(numCh & 0xff);
  memcpy(h + 24, &sampleRate, 4);
  memcpy(h + 28, &byteRate,   4);
  h[32] = (uint8_t)(blockAlign & 0xff);
  h[34] = (uint8_t)(bitsPer & 0xff);
  memcpy(h + 36, "data", 4);
  f.write(h, sizeof(h));
}

static void patchWavHeader(File& f, uint32_t dataBytes) {
  uint32_t riffSize = 36 + dataBytes;
  f.seek(4);  f.write((uint8_t*)&riffSize,  4);
  f.seek(40); f.write((uint8_t*)&dataBytes, 4);
}

static bool initMic() {
  I2S.setPinsPdmRx(PDM_CLK, PDM_DATA);
  if (!I2S.begin(I2S_MODE_PDM_RX, AUDIO_SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[mic] I2S.begin failed");
    return false;
  }
  return true;
}

// Walk /audio/ once at boot to find the highest existing rec_NNNNNN.wav
// and seed g_nextAudioId so it persists across reboots.
static void scanNextAudioId() {
  uint32_t maxN = 0;
  File dir = SD_MMC.open(AUDIO_DIR);
  if (!dir || !dir.isDirectory()) {
    g_nextAudioId = 1;
    return;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.startsWith("rec_") && name.endsWith(".wav")) {
      String numPart = name.substring(4, name.length() - 4);
      uint32_t n = (uint32_t)numPart.toInt();
      if (n > maxN) maxN = n;
    }
    f.close();
  }
  g_nextAudioId = maxN + 1;
}

static void recordingStart() {
  if (g_recording) return;
  if (g_nextAudioId > 999999) {
    Serial.println("[audio] counter exhausted (rec_999999.wav) - refusing to start");
    ledStrobe(5, 100);
    return;
  }
  char path[64];
  snprintf(path, sizeof(path), "%s/rec_%06u.wav",
           AUDIO_DIR, (unsigned)g_nextAudioId);
  g_wavFile = SD_MMC.open(path, FILE_WRITE);
  if (!g_wavFile) {
    Serial.printf("[audio] open %s failed\n", path);
    ledStrobe(5, 100);
    return;
  }
  writeWavHeaderPlaceholder(g_wavFile);
  g_wavDataBytes = 0;
  g_recording = true;
  digitalWrite(PIN_LED, LED_ON_LEVEL);  // solid on while recording
  Serial.printf("[audio] recording -> %s\n", path);
}

static void recordingStop() {
  if (!g_recording) return;
  g_recording = false;
  patchWavHeader(g_wavFile, g_wavDataBytes);
  g_wavFile.close();
  Serial.printf("[audio] stopped, %u PCM bytes\n", (unsigned)g_wavDataBytes);
  g_nextAudioId++;
  // ledHeartbeatTick() will resume the heartbeat on the next loop iteration.
}

// Pull whatever I2S samples are available and append to the WAV. Cheap to
// call when not recording (early-out on g_recording).
static void recordingPump() {
  if (!g_recording) return;
  static uint8_t buf[2048];
  size_t n = I2S.readBytes((char*)buf, sizeof(buf));
  if (n > 0) {
    g_wavFile.write(buf, n);
    g_wavDataBytes += n;
  }
}

// ============================================================
// Camera
// ============================================================

static bool initCamera() {
  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer   = LEDC_TIMER_0;
  cfg.pin_d0       = Y2_GPIO_NUM;
  cfg.pin_d1       = Y3_GPIO_NUM;
  cfg.pin_d2       = Y4_GPIO_NUM;
  cfg.pin_d3       = Y5_GPIO_NUM;
  cfg.pin_d4       = Y6_GPIO_NUM;
  cfg.pin_d5       = Y7_GPIO_NUM;
  cfg.pin_d6       = Y8_GPIO_NUM;
  cfg.pin_d7       = Y9_GPIO_NUM;
  cfg.pin_xclk     = XCLK_GPIO_NUM;
  cfg.pin_pclk     = PCLK_GPIO_NUM;
  cfg.pin_vsync    = VSYNC_GPIO_NUM;
  cfg.pin_href     = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn     = PWDN_GPIO_NUM;
  cfg.pin_reset    = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.frame_size   = FRAMESIZE_SVGA;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;
  cfg.jpeg_quality = 12;
  cfg.fb_count     = 1;

  esp_err_t err = esp_camera_init(&cfg);
  if (err != ESP_OK) {
    Serial.printf("[cam] init FAILED 0x%x\n", err);
    return false;
  }
  return true;
}

// ============================================================
// Photo storage
// ============================================================

// Walk /photos/ once at boot to find the highest existing img_NNNNNN.jpg
// and seed g_nextPhotoId so it persists across reboots without an RTC.
static void scanNextPhotoId() {
  uint32_t maxN = 0;
  File dir = SD_MMC.open(PHOTOS_DIR);
  if (!dir || !dir.isDirectory()) {
    g_nextPhotoId = 1;
    return;
  }
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (name.startsWith("img_") && name.endsWith(".jpg")) {
      // "img_NNNNNN.jpg" — digits between index 4 and the trailing ".jpg"
      String numPart = name.substring(4, name.length() - 4);
      uint32_t n = (uint32_t)numPart.toInt();
      if (n > maxN) maxN = n;
    }
    f.close();
  }
  g_nextPhotoId = maxN + 1;
}

static void captureOnePhoto() {
  if (g_nextPhotoId > 999999) {
    Serial.println("[photo] counter exhausted (img_999999.jpg) - refusing to write");
    ledStrobe(5, 100);
    return;
  }
  // With CAMERA_GRAB_WHEN_EMPTY + fb_count=1 the DMA stops once the buffer
  // is filled, so the next fb_get returns whatever was captured at the
  // *previous* press — stale by however long ago that was. Drop two frames
  // first to flush the buffer and let AEC/AWB settle on the current scene.
  for (int i = 0; i < 2; ++i) {
    camera_fb_t* drop = esp_camera_fb_get();
    if (drop) esp_camera_fb_return(drop);
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[photo] fb_get failed");
    ledStrobe(5, 100);
    return;
  }

  char path[64];
  snprintf(path, sizeof(path), "%s/img_%06u.jpg",
           PHOTOS_DIR, (unsigned)g_nextPhotoId);

  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) {
    Serial.printf("[photo] open %s failed\n", path);
    esp_camera_fb_return(fb);
    ledStrobe(5, 100);
    return;
  }

  size_t written = f.write(fb->buf, fb->len);
  f.close();
  esp_camera_fb_return(fb);

  if (written != fb->len) {
    Serial.printf("[photo] short write %u/%u\n",
                  (unsigned)written, (unsigned)fb->len);
    ledStrobe(5, 100);
    return;
  }

  Serial.printf("[photo] saved %s (%u bytes)\n", path, (unsigned)written);
  g_nextPhotoId++;
  ledFlash(100);
}

// ============================================================
// Setup / loop
// ============================================================

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF_LEVEL);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  delay(1200);

  Serial.println("owl_esp32 booting (snapshot mode)");

  delay(500);  // SD warmup
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  bool mounted = false;
  for (int attempt = 1; attempt <= 8 && !mounted; ++attempt) {
    int freq = (attempt <= 2) ? SDMMC_FREQ_PROBING : SDMMC_FREQ_DEFAULT;
    mounted = SD_MMC.begin("/sdcard", true, false, freq, 5);
    if (!mounted) {
      Serial.printf("[sd] mount attempt %d (%d Hz) failed\n", attempt, freq);
      SD_MMC.end();
      delay(600);
    }
  }
  if (!mounted) {
    Serial.println("[sd] mount failed - halting");
    haltBlinking(120);
  }
  Serial.printf("[sd] mounted, %llu MB total, %llu MB used\n",
                SD_MMC.totalBytes() / (1024ULL * 1024ULL),
                SD_MMC.usedBytes()  / (1024ULL * 1024ULL));

  if (!SD_MMC.exists(PHOTOS_DIR) && !SD_MMC.mkdir(PHOTOS_DIR)) {
    Serial.println("[sd] mkdir /photos failed - halting");
    haltBlinking(120);
  }
  if (!SD_MMC.exists(AUDIO_DIR) && !SD_MMC.mkdir(AUDIO_DIR)) {
    Serial.println("[sd] mkdir /audio failed - halting");
    haltBlinking(120);
  }

  if (!initCamera()) {
    Serial.println("[cam] halting");
    haltBlinking(400);
  }
  Serial.println("[cam] ready");

  if (!initMic()) {
    Serial.println("[mic] halting");
    haltBlinking(700);
  }
  Serial.println("[mic] ready");

  scanNextPhotoId();
  Serial.printf("[photos] resuming at img_%06u.jpg\n", (unsigned)g_nextPhotoId);
  scanNextAudioId();
  Serial.printf("[audio]  resuming at rec_%06u.wav\n", (unsigned)g_nextAudioId);

  // Bring up the WiFi AP. Failure is non-fatal — photos still work offline.
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("[wifi] softAP failed - continuing without AP");
  } else {
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[wifi] AP up: SSID=%s IP=%s\n",
                  AP_SSID, apIP.toString().c_str());
    if (MDNS.begin("owl")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("[mdns] http://owl.local/");
    } else {
      Serial.println("[mdns] start failed");
    }
  }

  // GET / — try /web/index.html on SD first; if missing, fall back to the
  // INDEX_HTML embedded above. This lets users iterate UI by dropping new
  // files into /web/ on the SD card without reflashing the firmware.
  g_http.on("/", []() {
    File f = SD_MMC.open("/web/index.html", FILE_READ);
    if (f && !f.isDirectory()) {
      g_http.streamFile(f, "text/html");
      f.close();
      return;
    }
    if (f) f.close();
    g_http.send(200, "text/html", INDEX_HTML);
  });
  g_http.on("/list", []() {
    // Skip the actively-recording WAV (header isn't finalized yet).
    char activeName[32] = {0};
    if (g_recording) {
      snprintf(activeName, sizeof(activeName),
               "rec_%06u.wav", (unsigned)g_nextAudioId);
    }
    auto append = [&](String& body, const char* dir,
                      const char* prefix, const char* suffix,
                      const char* skip) {
      File d = SD_MMC.open(dir);
      if (!d || !d.isDirectory()) return;
      bool first = true;
      for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        if (!f.isDirectory()) {
          String name = f.name();
          int slash = name.lastIndexOf('/');
          if (slash >= 0) name = name.substring(slash + 1);
          if (name.startsWith(prefix) && name.endsWith(suffix) &&
              (skip[0] == '\0' || name != skip)) {
            if (!first) body += ",";
            body += "\""; body += name; body += "\"";
            first = false;
          }
        }
        f.close();
      }
    };
    String body = "{\"photos\":[";
    append(body, PHOTOS_DIR, "img_", ".jpg", "");
    body += "],\"audio\":[";
    append(body, AUDIO_DIR,  "rec_", ".wav", activeName);
    body += "]}";
    g_http.send(200, "application/json", body);
  });
  // POST /annotate?photo=img_NNNNNN.jpg&name=person1&dist=0.566
  // Upserts an annotation for one photo. Body params via form-urlencoded.
  g_http.on("/annotate", HTTP_POST, []() {
    String photo = g_http.arg("photo");
    String name  = g_http.arg("name");
    String distS = g_http.arg("dist");
    if (photo.length() == 0 || name.length() == 0 ||
        photo.indexOf('/') >= 0 || photo.indexOf("..") >= 0) {
      g_http.send(400, "text/plain", "missing or invalid params\n");
      return;
    }
    float dist = distS.toFloat();
    for (auto& a : g_annotations) {
      if (a.filename == photo) { a.name = name; a.dist = dist;
                                 g_http.send(200, "text/plain", "updated\n"); return; }
    }
    g_annotations.push_back({photo, name, dist});
    g_http.send(201, "text/plain", "added\n");
  });

  // GET /annotations — JSON map of photo filename → {name, dist}.
  g_http.on("/annotations", HTTP_GET, []() {
    String body = "{";
    bool first = true;
    for (auto& a : g_annotations) {
      if (!first) body += ",";
      body += "\""; body += a.filename; body += "\":{\"name\":\"";
      body += a.name; body += "\",\"dist\":"; body += String(a.dist, 3); body += "}";
      first = false;
    }
    body += "}";
    g_http.send(200, "application/json", body);
  });

  // /photo/<name> + /audio/<name> — GET streams the file, DELETE removes it.
  // Reject any name containing '/' or '..' so the lookup can't escape its dir.
  g_http.onNotFound([]() {
    String uri = g_http.uri();
    const char* dir         = nullptr;
    const char* contentType = nullptr;
    String name;
    if (uri.startsWith("/photo/")) {
      dir = PHOTOS_DIR; contentType = "image/jpeg"; name = uri.substring(7);
    } else if (uri.startsWith("/audio/")) {
      dir = AUDIO_DIR;  contentType = "audio/wav";  name = uri.substring(7);
    } else {
      // Try to serve as a top-level static file from /web/<filename>.
      // Only matches paths like "/style.css", "/app.js" — single segment,
      // no traversal. Sub-paths like "/assets/foo.js" not supported here.
      if (g_http.method() == HTTP_GET &&
          uri.length() > 1 &&
          uri.indexOf('/', 1) < 0 &&
          uri.indexOf("..") < 0) {
        String full = String("/web") + uri;
        File f = SD_MMC.open(full, FILE_READ);
        if (f && !f.isDirectory()) {
          g_http.streamFile(f, contentTypeFor(uri));
          f.close();
          return;
        }
        if (f) f.close();
      }
      g_http.send(404, "text/plain", "not found\n");
      return;
    }
    if (name.length() == 0 ||
        name.indexOf('/')  >= 0 ||
        name.indexOf("..") >= 0) {
      g_http.send(404, "text/plain", "not found\n");
      return;
    }

    HTTPMethod m = g_http.method();

    // Refuse to serve or delete the WAV that's currently being recorded —
    // header is unfinalized and the file is still being appended to.
    if (g_recording && dir == AUDIO_DIR) {
      char active[32];
      snprintf(active, sizeof(active),
               "rec_%06u.wav", (unsigned)g_nextAudioId);
      if (name == active) {
        g_http.send(503, "text/plain", "recording in progress\n");
        return;
      }
    }

    String full = String(dir) + "/" + name;
    if (m == HTTP_GET) {
      File f = SD_MMC.open(full, FILE_READ);
      if (!f || f.isDirectory()) {
        if (f) f.close();
        g_http.send(404, "text/plain", "not found\n");
        return;
      }
      g_http.streamFile(f, contentType);
      f.close();
    } else if (m == HTTP_DELETE) {
      if (!SD_MMC.exists(full)) {
        g_http.send(404, "text/plain", "not found\n");
        return;
      }
      if (SD_MMC.remove(full)) {
        Serial.printf("[http] deleted %s\n", full.c_str());
        // Drop any annotation tied to this photo.
        if (dir == PHOTOS_DIR) {
          for (auto it = g_annotations.begin(); it != g_annotations.end(); ++it) {
            if (it->filename == name) { g_annotations.erase(it); break; }
          }
        }
        g_http.send(204, "text/plain", "");
      } else {
        Serial.printf("[http] delete failed %s\n", full.c_str());
        g_http.send(500, "text/plain", "delete failed\n");
      }
    } else {
      g_http.send(405, "text/plain", "method not allowed\n");
    }
  });
  g_http.begin();
  Serial.println("[http] listening on :80");

  Serial.println("ready - short-press BOOT for photo "
                 "(long-press reserved for audio in TODO step 8)");
}

void loop() {
  uint32_t now = millis();
  int raw = digitalRead(PIN_BUTTON);

  // Track raw-edge timestamp for debouncing.
  if (raw != g_btnRaw) {
    g_btnRaw      = raw;
    g_btnRawChgAt = now;
  }

  // Promote to debounced state once stable for 30 ms.
  if ((now - g_btnRawChgAt) >= 30 && raw != g_btnStable) {
    int prev    = g_btnStable;
    g_btnStable = raw;
    if (prev == HIGH && raw == LOW) {
      // Press edge: anchor the long-press timer.
      g_btnPressedAt = now;
      g_longFired    = false;
    } else if (prev == LOW && raw == HIGH) {
      // Release edge: short tap if we didn't already long-fire.
      if (!g_longFired) {
        captureOnePhoto();
      }
    }
  }

  // Long-press fires exactly once at the threshold while held: toggles
  // audio recording. First long-press starts; second stops.
  if (g_btnStable == LOW && !g_longFired &&
      (now - g_btnPressedAt) >= LONG_PRESS_MS) {
    g_longFired = true;
    if (g_recording) recordingStop();
    else             recordingStart();
  }

  recordingPump();
  g_http.handleClient();
  ledHeartbeatTick();
  delay(5);
}
