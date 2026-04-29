// TinyUSB CDC smoke test. With USBMode=default,CDCOnBoot=default the core
// auto-starts TinyUSB + CDC in initVariant() before setup() runs.
// Expected: ioreg shows a NEW USB device (NOT "USB JTAG/serial debug unit"),
// and "tick" prints appear on the new CDC port.
void setup() {
  Serial.begin(115200);
  delay(2000);
  pinMode(21, OUTPUT);
  Serial.println("tinyusb cdc: hello");
}
uint32_t t = 0;
void loop() {
  if (millis() - t > 1000) {
    t = millis();
    Serial.printf("tick %lu\n", (unsigned long)t);
    digitalWrite(21, !digitalRead(21));
  }
}
