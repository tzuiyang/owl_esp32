// Minimal USB MSC test backed by the microSD on XIAO ESP32-S3 Sense.
// Uses CDCOnBoot=default (Enabled) so we get a serial log for diagnosis.
// Registers MSC in setup(), then calls USB.begin() again to re-enumerate.

#include <Arduino.h>
#include "USB.h"
#include "USBMSC.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

static const int SD_CLK = 7;
static const int SD_CMD = 9;
static const int SD_D0  = 8;

USBMSC      MSC;
sdmmc_card_t card;
bool        cardOk = false;

static int32_t onRead(uint32_t lba, uint32_t offset, void* buf, uint32_t size) {
  if (!cardOk) return -1;
  uint32_t n = size / card.csd.sector_size;
  return (sdmmc_read_sectors(&card, buf, lba, n) == ESP_OK) ? size : -1;
}
static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t size) {
  if (!cardOk) return -1;
  uint32_t n = size / card.csd.sector_size;
  return (sdmmc_write_sectors(&card, buf, lba, n) == ESP_OK) ? size : -1;
}
static bool onStartStop(uint8_t p, bool start, bool eject) { return true; }

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== test_msc starting ===");

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

  esp_err_t e1 = sdmmc_host_init();                       Serial.printf("host_init: %d\n", e1);
  esp_err_t e2 = sdmmc_host_init_slot(host.slot, &slot);  Serial.printf("init_slot: %d\n", e2);
  esp_err_t e3 = sdmmc_card_init(&host, &card);           Serial.printf("card_init: %d\n", e3);
  cardOk = (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK);
  Serial.printf("card capacity=%d sectors, sector_size=%d\n",
                card.csd.capacity, card.csd.sector_size);

  if (!cardOk) {
    Serial.println("SD init failed - MSC won't be backed by anything");
  }

  MSC.vendorID("owltest");
  MSC.productID("owl_sd");
  MSC.productRevision("1.0");
  MSC.onRead(onRead);
  MSC.onWrite(onWrite);
  MSC.onStartStop(onStartStop);
  MSC.mediaPresent(true);
  bool ok = MSC.begin(card.csd.capacity, card.csd.sector_size);
  Serial.printf("MSC.begin -> %s\n", ok ? "OK" : "FAIL");

  USB.begin();   // second call re-enumerates on many hosts
  Serial.println("USB.begin() done; if Finder doesn't show a drive, watch this log.");
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 2000) {
    t = millis();
    Serial.printf("alive t=%lu cardOk=%d\n", (unsigned long)t, (int)cardOk);
  }
}
