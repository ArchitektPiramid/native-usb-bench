#include <Arduino.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error This firmware targets the ESP32-S3 USB-OTG Full-Speed controller.
#endif
#if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 1
#error Select USB-OTG (TinyUSB) in Tools > USB Mode; this sketch does not call TinyUSB.
#endif
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#error Disable USB CDC On Boot; ROM USB handler must own the native USB PHY.
#endif

#include "src/benchmark/benchmark.h"

// User settings: choose a named ROM DCD write size. 64 B is the known-good baseline.
// Larger sizes are experimental and deliberately have NO automatic fallback.
#define NUB_BULK_WRITE_SIZE RomUsb::BulkWriteSize::Bytes64
#define NUB_LOOP_YIELD_EVERY 64

// For compile-time diagnostics/IRAM choices, edit src/core/config.h;
// .ino-only #defines are not visible to separately compiled .cpp modules.
void setup() {
  Bench::begin({NUB_BULK_WRITE_SIZE, NUB_LOOP_YIELD_EVERY});
}
void loop() { Bench::tick(); }
