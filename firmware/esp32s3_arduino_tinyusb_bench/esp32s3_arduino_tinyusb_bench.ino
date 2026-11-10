#include <Arduino.h>

#ifndef ARDUINO_USB_MODE
#error This ESP32 target has no native USB device mode.
#elif ARDUINO_USB_MODE == 1
#error Set Tools -> USB Mode to "USB-OTG (TinyUSB)".
#endif

#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
#error Disable "USB CDC On Boot" so the NativeUSB Vendor interface remains interface 0.
#endif

#if defined(ARDUINO_USB_MSC_ON_BOOT) && ARDUINO_USB_MSC_ON_BOOT
#error Disable "USB MSC On Boot" so the NativeUSB Vendor interface remains interface 0.
#endif

#if defined(ARDUINO_USB_DFU_ON_BOOT) && ARDUINO_USB_DFU_ON_BOOT
#error Disable "USB DFU On Boot" so the NativeUSB Vendor interface remains interface 0.
#endif

#include "src/benchmark/benchmark.h"

void setup() {
  Bench::begin();
}

void loop() {
  Bench::tick();
}
