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
void setup() { Bench::begin(); }
void loop() { Bench::tick(); }
