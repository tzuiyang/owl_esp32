// owl_esp32 — snapshot branch
//
// Short-press BOOT → capture one JPEG to /photos/img_NNNNNN.jpg on the SD card.
// Long-press BOOT → reserved for audio recording (wired up in TODO.md Step 8).
// WiFi AP + HTTP gallery → TODO.md Steps 2–7.
//
// Target board: Seeed XIAO ESP32-S3 Sense
// FQBN: esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <ESP_I2S.h>
#include <WiFi.h>
#include <WebServer.h>
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

// --------- WiFi AP ----------
static const char* AP_SSID     = "owl";
static const char* AP_PASSWORD = "owlowlowl";   // ≥8 chars for WPA2

// --------- HTTP server ----------
static WebServer g_http(80);

// Gallery page served at /. Polls /list every 5 s so new photos appear
// without a manual reload. The script builds DOM nodes via textContent
// and encodeURIComponent — never innerHTML — so a stray filename on the
// SD can't inject script.
static const char INDEX_HTML[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>owl</title>
<style>
 body{font:14px/1.4 system-ui,sans-serif;margin:1.5em;background:#0a0a0a;color:#eee}
 h1{font-size:1.1em;font-weight:600;margin:0 0 .8em}
 #count{opacity:.6;font-weight:400}
 .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:.6em}
 .card{position:relative;border:1px solid #222;background:#111}
 .card:hover{background:#1a1a2a;border-color:#345}
 .card a{display:block;text-decoration:none;color:#9cf}
 .card img{display:block;width:100%;height:auto;background:#000}
 .card .name{padding:.4em .5em;font-family:ui-monospace,monospace;font-size:.8em;text-align:center}
 .card .del{position:absolute;top:.3em;right:.3em;width:1.6em;height:1.6em;border:0;border-radius:50%;background:rgba(0,0,0,.6);color:#fff;font-size:.9em;cursor:pointer;opacity:0;transition:opacity .15s}
 .card:hover .del{opacity:1}
 .card .del:hover{background:#a33}
 .empty{opacity:.5;font-style:italic}
</style></head><body>
<h1>owl gallery <span id="count"></span></h1>
<div id="g" class="grid"></div>
<script>
async function refresh(){
  try{
    const r = await fetch('/list', {cache:'no-store'});
    const names = await r.json();
    document.getElementById('count').textContent = '(' + names.length + ')';
    const g = document.getElementById('g');
    g.replaceChildren();
    if(!names.length){
      const d = document.createElement('div');
      d.className = 'empty';
      d.textContent = 'no photos yet — short-press BOOT to capture';
      g.appendChild(d);
      return;
    }
    for(const n of names){
      const url = '/photo/' + encodeURIComponent(n);
      const card = document.createElement('div');
      card.className = 'card';
      const a = document.createElement('a');
      a.href = url; a.target = '_blank';
      const img = document.createElement('img');
      img.src = url; img.loading = 'lazy'; img.alt = n;
      const cap = document.createElement('div');
      cap.className = 'name'; cap.textContent = n;
      a.appendChild(img); a.appendChild(cap);
      const del = document.createElement('button');
      del.className = 'del';
      del.type = 'button';
      del.title = 'delete';
      del.textContent = '✕';
      del.addEventListener('click', async (e) => {
        e.preventDefault(); e.stopPropagation();
        if(!confirm('Delete ' + n + '?')) return;
        del.disabled = true;
        try{
          const r = await fetch(url, {method:'DELETE'});
          if(r.ok){ card.remove(); document.getElementById('count').textContent = '(' + (g.children.length) + ')'; }
          else { alert('delete failed: HTTP ' + r.status); del.disabled = false; }
        }catch(err){ alert('delete failed: ' + err); del.disabled = false; }
      });
      card.appendChild(a); card.appendChild(del);
      g.appendChild(card);
    }
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

// I2S handle reserved for Step 8 audio recording. initMic() below is currently
// uncalled but kept compilable so Step 8 can re-enable it without ABI churn.
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
static void ledHeartbeatTick() {
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

// Reserved for Step 8 (audio recording on long-press BOOT).
static bool initMic() {
  I2S.setPinsPdmRx(PDM_CLK, PDM_DATA);
  if (!I2S.begin(I2S_MODE_PDM_RX, AUDIO_SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[mic] I2S.begin failed");
    return false;
  }
  return true;
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

  if (!initCamera()) {
    Serial.println("[cam] halting");
    haltBlinking(400);
  }
  Serial.println("[cam] ready");

  scanNextPhotoId();
  Serial.printf("[photos] resuming at img_%06u.jpg\n", (unsigned)g_nextPhotoId);

  // Bring up the WiFi AP. Failure is non-fatal — photos still work offline.
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("[wifi] softAP failed - continuing without AP");
  } else {
    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("[wifi] AP up: SSID=%s IP=%s\n",
                  AP_SSID, apIP.toString().c_str());
  }

  // HTTP routes. Audio support and combined-list shape arrive in Step 8.
  g_http.on("/", []() {
    g_http.send(200, "text/html", INDEX_HTML);
  });
  g_http.on("/list", []() {
    String body = "[";
    bool first = true;
    File dir = SD_MMC.open(PHOTOS_DIR);
    if (dir && dir.isDirectory()) {
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
          String name = f.name();
          int slash = name.lastIndexOf('/');
          if (slash >= 0) name = name.substring(slash + 1);
          if (name.startsWith("img_") && name.endsWith(".jpg")) {
            if (!first) body += ",";
            body += "\"";
            body += name;
            body += "\"";
            first = false;
          }
        }
        f.close();
      }
    }
    body += "]";
    g_http.send(200, "application/json", body);
  });
  // /photo/<name> — GET streams a single JPEG, DELETE removes it from SD.
  // Reject any name containing '/' or '..' so the lookup can't escape /photos/.
  g_http.onNotFound([]() {
    static const String prefix = "/photo/";
    String uri = g_http.uri();
    if (!uri.startsWith(prefix)) {
      g_http.send(404, "text/plain", "not found\n");
      return;
    }
    String name = uri.substring(prefix.length());
    if (name.length() == 0 ||
        name.indexOf('/')  >= 0 ||
        name.indexOf("..") >= 0) {
      g_http.send(404, "text/plain", "not found\n");
      return;
    }
    String full = String(PHOTOS_DIR) + "/" + name;

    HTTPMethod m = g_http.method();
    if (m == HTTP_GET) {
      File f = SD_MMC.open(full, FILE_READ);
      if (!f || f.isDirectory()) {
        if (f) f.close();
        g_http.send(404, "text/plain", "no such photo\n");
        return;
      }
      g_http.streamFile(f, "image/jpeg");
      f.close();
    } else if (m == HTTP_DELETE) {
      if (!SD_MMC.exists(full)) {
        g_http.send(404, "text/plain", "no such photo\n");
        return;
      }
      if (SD_MMC.remove(full)) {
        Serial.printf("[photo] deleted %s\n", full.c_str());
        g_http.send(204, "text/plain", "");
      } else {
        Serial.printf("[photo] delete failed %s\n", full.c_str());
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

  // Long-press fires exactly once at the threshold while held.
  if (g_btnStable == LOW && !g_longFired &&
      (now - g_btnPressedAt) >= LONG_PRESS_MS) {
    g_longFired = true;
    Serial.println("[boot] long-press detected (no action until TODO step 8)");
  }

  g_http.handleClient();
  ledHeartbeatTick();
  delay(5);
}
