// owl_esp32 — record audio (PDM mic) + slow-FPS JPEG frames to SD card.
// Also supports "flash drive mode" that exposes the microSD over USB so you
// can browse the files from your laptop without an SD card reader.
//
// Target board: Seeed XIAO ESP32-S3 Sense
// FQBN: esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=default
//
// Workflow:
//   * After power-on the board starts in recorder mode.
//       - Short tap BOOT (<~1s)         : start / stop one recording session.
//       - Long press BOOT (>=2.5s)      : reboot into flash-drive mode.
//     Each recording session is saved under
//     /<YYYY-MM-DD>_<NNN>/image/*.jpg and .../audio/rec_001.wav.
//
//   * In flash-drive mode:
//       - The microSD appears as a USB drive on your Mac (Finder/"NO NAME").
//       - LED blinks slowly (~1 Hz) the whole time.
//       - Short tap BOOT to reboot back into recorder mode.
//
// Note: we cannot use "hold BOOT while plugging in USB" as a mode trigger —
// the ESP32-S3's ROM samples GPIO0 at power-on and enters flash-download mode
// when it's low, so our firmware never gets a chance to run.

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <ESP_I2S.h>
#include "esp_camera.h"
#include "camera_pins.h"

#include "USB.h"
#include "USBMSC.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_system.h"
#include <Preferences.h>

// Mode flag lives in NVS (flash-backed) so it reliably survives esp_restart()
// regardless of RAM clearing quirks in arduino-esp32's startup path.
static Preferences g_prefs;
static const char* NVS_NS       = "owl";
static const char* NVS_KEY_MODE = "nextmode";   // 0 = recorder, 1 = msc

// --------- user-tweakable settings ----------
static const uint32_t IMAGE_INTERVAL_MS = 2000;   // snap a frame every 2s while recording
static const uint32_t AUDIO_SAMPLE_RATE = 16000;  // 16 kHz mono 16-bit WAV
static const int      PIN_BUTTON        = 0;      // BOOT button on XIAO S3
static const int      PIN_LED           = LED_BUILTIN;   // GPIO21 on XIAO S3
static const int      LED_ON_LEVEL      = LOW;           // active-LOW
static const int      LED_OFF_LEVEL     = HIGH;

// --------- SD pins for XIAO ESP32-S3 Sense (SDMMC 1-bit) ----------
static const int SD_CLK = 7;
static const int SD_CMD = 9;
static const int SD_D0  = 8;

// --------- PDM mic pins for XIAO ESP32-S3 Sense ----------
static const int PDM_CLK  = 42;
static const int PDM_DATA = 41;

// ============================================================
// MSC (flash drive) mode — exposes the SD over USB
// ============================================================

static USBMSC       s_msc;
static sdmmc_card_t s_mscCard;
static bool         s_mscCardOk = false;

static bool mscInitCard() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags        = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  host.slot         = SDMMC_HOST_SLOT_1;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 1;
  slot.clk   = (gpio_num_t)SD_CLK;
  slot.cmd   = (gpio_num_t)SD_CMD;
  slot.d0    = (gpio_num_t)SD_D0;
  slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  if (sdmmc_host_init() != ESP_OK) return false;
  if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK) return false;
  if (sdmmc_card_init(&host, &s_mscCard) != ESP_OK) return false;
  return true;
}

static int32_t mscOnWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t size) {
  if (!s_mscCardOk) return -1;
  uint32_t n = size / s_mscCard.csd.sector_size;
  return (sdmmc_write_sectors(&s_mscCard, buf, lba, n) == ESP_OK) ? (int32_t)size : -1;
}

static int32_t mscOnRead(uint32_t lba, uint32_t offset, void* buf, uint32_t size) {
  if (!s_mscCardOk) return -1;
  uint32_t n = size / s_mscCard.csd.sector_size;
  return (sdmmc_read_sectors(&s_mscCard, buf, lba, n) == ESP_OK) ? (int32_t)size : -1;
}

static bool mscOnStartStop(uint8_t power_condition, bool start, bool load_eject) {
  return true;
}

// In MSC mode, slow-blink the LED and poll BOOT. A short tap reboots back
// into recorder mode.
static void runMscLoop() {
  int lastRaw = HIGH;
  uint32_t pressedAt = 0;
  uint32_t ledT = 0;
  bool ledOn = false;
  while (true) {
    uint32_t now = millis();
    if (now - ledT >= (ledOn ? 900 : 100)) {
      ledT = now;
      ledOn = !ledOn;
      digitalWrite(PIN_LED, ledOn ? LED_ON_LEVEL : LED_OFF_LEVEL);
    }
    int raw = digitalRead(PIN_BUTTON);
    if (raw == LOW && lastRaw == HIGH) {
      pressedAt = now;
    } else if (raw == HIGH && lastRaw == LOW) {
      if (now - pressedAt > 30) {
        Serial.println("[msc] BOOT tapped - rebooting into recorder");
        g_prefs.begin(NVS_NS, false);
        g_prefs.putUChar(NVS_KEY_MODE, 0);
        g_prefs.end();
        delay(80);
        esp_restart();
      }
    }
    lastRaw = raw;
    delay(5);
  }
}

// ============================================================
// Recorder mode — state
// ============================================================

I2SClass  I2S;
String    g_sessionDir;
String    g_imageDir;
String    g_audioDir;
uint32_t  g_imageCounter = 0;
uint32_t  g_audioCounter = 0;

bool      g_recording    = false;
File      g_wavFile;
uint32_t  g_wavDataBytes = 0;
uint32_t  g_lastImageMs  = 0;

int       g_btnStable    = HIGH;   // debounced state
int       g_btnRaw       = HIGH;   // last raw reading
uint32_t  g_btnRawChgAt  = 0;      // when raw last changed (for debounce)
uint32_t  g_btnPressedAt = 0;      // when stable became LOW (bounce-proof timer)
bool      g_longFired    = false;  // did we already handle the long-press?

// ============================================================
// Helpers
// ============================================================

static String compileDateIso() {
  const char* d = __DATE__;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mname[4] = { d[0], d[1], d[2], 0 };
  const char* p = strstr(months, mname);
  int mm = p ? ((p - months) / 3) + 1 : 1;
  int dd = atoi(d + 4);
  int yy = atoi(d + 7);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", yy, mm, dd);
  return String(buf);
}

static String nextSessionDir(const String& datePrefix) {
  int maxN = 0;
  File root = SD_MMC.open("/");
  if (root && root.isDirectory()) {
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
      if (!f.isDirectory()) continue;
      String name = f.name();
      if (name.startsWith("/")) name = name.substring(1);
      if (name.startsWith(datePrefix + "_")) {
        int n = name.substring(datePrefix.length() + 1).toInt();
        if (n > maxN) maxN = n;
      }
    }
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "/%s_%03d", datePrefix.c_str(), maxN + 1);
  return String(buf);
}

static bool ensureDir(const String& path) {
  if (SD_MMC.exists(path)) return true;
  return SD_MMC.mkdir(path);
}

static bool prepareNewSession() {
  String date = compileDateIso();
  g_sessionDir = nextSessionDir(date);
  g_imageDir   = g_sessionDir + "/image";
  g_audioDir   = g_sessionDir + "/audio";

  if (!ensureDir(g_sessionDir)) {
    Serial.printf("[sd] mkdir %s failed\n", g_sessionDir.c_str());
    return false;
  }
  if (!ensureDir(g_imageDir)) {
    Serial.printf("[sd] mkdir %s failed\n", g_imageDir.c_str());
    return false;
  }
  if (!ensureDir(g_audioDir)) {
    Serial.printf("[sd] mkdir %s failed\n", g_audioDir.c_str());
    return false;
  }

  g_imageCounter = 0;
  g_audioCounter = 0;
  Serial.printf("[sd] session dir: %s\n", g_sessionDir.c_str());
  return true;
}

// ============================================================
// WAV streaming
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
  cfg.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;   // no FB-OVF spam
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

static void captureOneImage() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[cam] fb_get failed"); return; }
  g_imageCounter++;
  char name[64];
  snprintf(name, sizeof(name), "%s/img_%06u.jpg",
           g_imageDir.c_str(), (unsigned)g_imageCounter);
  File f = SD_MMC.open(name, FILE_WRITE);
  if (!f) {
    Serial.printf("[cam] open %s failed\n", name);
  } else {
    f.write(fb->buf, fb->len);
    f.close();
    Serial.printf("[cam] %s  (%u bytes)\n", name, (unsigned)fb->len);
  }
  esp_camera_fb_return(fb);
}

// ============================================================
// Audio
// ============================================================

static bool initMic() {
  I2S.setPinsPdmRx(PDM_CLK, PDM_DATA);
  if (!I2S.begin(I2S_MODE_PDM_RX, AUDIO_SAMPLE_RATE,
                 I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("[mic] I2S.begin failed");
    return false;
  }
  return true;
}

static bool audioStart() {
  g_audioCounter++;
  char name[64];
  snprintf(name, sizeof(name), "%s/rec_%03u.wav",
           g_audioDir.c_str(), (unsigned)g_audioCounter);
  g_wavFile = SD_MMC.open(name, FILE_WRITE);
  if (!g_wavFile) {
    Serial.printf("[mic] open %s failed\n", name);
    return false;
  }
  writeWavHeaderPlaceholder(g_wavFile);
  g_wavDataBytes = 0;
  Serial.printf("[mic] recording -> %s\n", name);
  return true;
}

static void audioStop() {
  if (!g_wavFile) return;
  patchWavHeader(g_wavFile, g_wavDataBytes);
  g_wavFile.close();
  Serial.printf("[mic] closed WAV (%u PCM bytes)\n", (unsigned)g_wavDataBytes);
}

static void audioPump() {
  if (!g_wavFile) return;
  static uint8_t buf[2048];
  size_t n = I2S.readBytes((char*)buf, sizeof(buf));
  if (n > 0) {
    g_wavFile.write(buf, n);
    g_wavDataBytes += n;
  }
}

// ============================================================
// Recorder mode — state machine
// ============================================================

static void startRecording() {
  if (g_recording) return;
  if (!prepareNewSession()) return;
  if (!audioStart()) return;
  g_recording    = true;
  g_lastImageMs  = 0;
  digitalWrite(PIN_LED, LED_ON_LEVEL);
  Serial.println("==> RECORDING");
}

static void stopRecording() {
  if (!g_recording) return;
  g_recording = false;
  audioStop();
  digitalWrite(PIN_LED, LED_OFF_LEVEL);
  Serial.println("==> STOPPED");
}

// ============================================================
// Setup / loop
// ============================================================

void setup() {
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LED_OFF_LEVEL);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  Serial.begin(115200);
  // With CDCOnBoot=Enabled the core has already called USB.begin() once; our
  // extra USB.begin() below re-enumerates with MSC added when needed.
  delay(1200);

  // Read the NVS-backed mode flag and consume it so the next reboot defaults
  // to recorder mode unless the user explicitly asks for MSC again.
  g_prefs.begin(NVS_NS, false);
  const uint8_t nvsMode = g_prefs.getUChar(NVS_KEY_MODE, 0);
  g_prefs.putUChar(NVS_KEY_MODE, 0);
  g_prefs.end();
  const bool mscMode = (nvsMode == 1);
  Serial.printf("[boot] reset_reason=%d nvsMode=%u mscMode=%d\n",
                (int)esp_reset_reason(), (unsigned)nvsMode, (int)mscMode);

  if (mscMode) {
    Serial.println("==> FLASH DRIVE MODE");
    s_mscCardOk = mscInitCard();
    if (!s_mscCardOk) {
      Serial.println("[msc] SD init failed - halting");
      while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(120); }
    }
    Serial.printf("[msc] card: %llu MB\n",
                  ((uint64_t)s_mscCard.csd.capacity * s_mscCard.csd.sector_size)
                      / (1024ULL * 1024ULL));

    s_msc.vendorID("owl");          // <= 8 chars (SCSI)
    s_msc.productID("owl_esp32");   // <= 16 chars
    s_msc.productRevision("1.0");   // <= 4 chars
    s_msc.onRead(mscOnRead);
    s_msc.onWrite(mscOnWrite);
    s_msc.onStartStop(mscOnStartStop);
    s_msc.mediaPresent(true);
    s_msc.begin((uint32_t)s_mscCard.csd.capacity, s_mscCard.csd.sector_size);

    USB.begin();   // re-enumerate with MSC interface added
    Serial.println("[msc] ready - check Finder for 'NO NAME' (or volume label)");
    runMscLoop();   // never returns
  }

  // ---- recorder mode ----
  Serial.println("owl_esp32 booting (recorder mode)");

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
    while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(120); }
  }
  Serial.printf("[sd] mounted, %llu MB total, %llu MB used\n",
                SD_MMC.totalBytes() / (1024ULL * 1024ULL),
                SD_MMC.usedBytes()  / (1024ULL * 1024ULL));

  Serial.println("[sd] ready; session dir will be created when recording starts");

  if (!initCamera()) {
    Serial.println("[cam] halting");
    while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(400); }
  }
  Serial.println("[cam] ready");

  if (!initMic()) {
    Serial.println("[mic] halting");
    while (true) { digitalWrite(PIN_LED, !digitalRead(PIN_LED)); delay(700); }
  }
  Serial.println("[mic] ready");

  Serial.println("Press BOOT to start/stop recording. "
                 "Long-press BOOT while running for flash-drive mode.");
}

// Button semantics in recorder mode:
//   * Short tap (debounced press + release, held < LONG_PRESS_MS) -> toggle
//     recording.
//   * Long press (held >= LONG_PRESS_MS) -> reboot into flash-drive mode.
// g_btnPressedAt is set only on the debounced press edge, so momentary
// contact bounces don't reset the long-press timer while the button is held.
static const uint32_t LONG_PRESS_MS = 2500;

static void rebootIntoMsc() {
  Serial.println("[recorder] long-press -> rebooting into flash-drive mode");
  if (g_recording) stopRecording();
  SD_MMC.end();
  // fast LED flash = long-press acknowledged (visible even without serial)
  for (int i = 0; i < 10; ++i) {
    digitalWrite(PIN_LED, LED_ON_LEVEL);  delay(50);
    digitalWrite(PIN_LED, LED_OFF_LEVEL); delay(50);
  }
  g_prefs.begin(NVS_NS, false);
  g_prefs.putUChar(NVS_KEY_MODE, 1);
  g_prefs.end();
  delay(80);
  esp_restart();
}

void loop() {
  uint32_t now = millis();
  int raw = digitalRead(PIN_BUTTON);

  // Track the raw-change timestamp for debouncing.
  if (raw != g_btnRaw) {
    g_btnRaw      = raw;
    g_btnRawChgAt = now;
  }

  // Once raw has been stable for 30 ms, promote it to the debounced state.
  if ((now - g_btnRawChgAt) >= 30 && raw != g_btnStable) {
    int prev     = g_btnStable;
    g_btnStable  = raw;
    if (prev == HIGH && raw == LOW) {
      g_btnPressedAt = now;   // anchor the long-press timer
      g_longFired    = false;
    } else if (prev == LOW && raw == HIGH) {
      // Release: short tap only if we didn't already handle a long press.
      if (!g_longFired) {
        if (g_recording) stopRecording();
        else             startRecording();
      }
    }
  }

  // Long-press check: stable LOW held past threshold fires exactly once.
  if (g_btnStable == LOW && !g_longFired &&
      (now - g_btnPressedAt) >= LONG_PRESS_MS) {
    g_longFired = true;
    rebootIntoMsc();  // never returns
  }

  if (g_recording) {
    audioPump();
    if (now - g_lastImageMs >= IMAGE_INTERVAL_MS) {
      g_lastImageMs = now;
      captureOneImage();
    }
  } else {
    delay(5);
  }
}
