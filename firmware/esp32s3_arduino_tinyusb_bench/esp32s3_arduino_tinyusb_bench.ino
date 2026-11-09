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

#include "config.h"
#include "nativeusb_protocol.h"
#include "payload_generators.h"
#include "usb_transport.h"

namespace Bench {

alignas(4) uint8_t g_frame[BenchConfig::kMaxFrameSize];
bool g_streaming = false;
uint32_t g_sequence = 0;
uint32_t g_frames_generated = 0;
uint32_t g_frames_sent = 0;
uint32_t g_frames_dropped = 0;
uint64_t g_bytes_sent = 0;

void publishRuntimeStatus() {
  NativeUsbProtocol::RuntimeStatus status{
      g_streaming,
      g_frames_generated,
      g_frames_sent,
      g_frames_dropped,
      g_bytes_sent,
  };
  NativeUsbEsp32::updateRuntimeStatus(status);
}

void resetCounters() {
  g_sequence = 0;
  g_frames_generated = 0;
  g_frames_sent = 0;
  g_frames_dropped = 0;
  g_bytes_sent = 0;
  publishRuntimeStatus();
}

uint32_t buildFrame(const NativeUsbProtocol::BenchmarkConfig& config) {
  const uint32_t frame_size = config.frame_size;
  const uint32_t payload_size = frame_size - NativeUsbProtocol::kFrameHeaderSize;

  memcpy(&g_frame[0], NativeUsbProtocol::kFrameMagic, 4);
  g_frame[4] = NativeUsbProtocol::kProtocolMajor;
  g_frame[5] = NativeUsbProtocol::kDataType;
  NativeUsbProtocol::putU16LE(
      &g_frame[6], static_cast<uint16_t>(static_cast<uint8_t>(config.generator)));
  NativeUsbProtocol::putU32LE(&g_frame[8], g_sequence);
  NativeUsbProtocol::putU32LE(&g_frame[12], payload_size);

  PayloadGenerators::fill(&g_frame[NativeUsbProtocol::kFrameHeaderSize], payload_size,
                          config.generator, config.seed, g_sequence);
  ++g_frames_generated;
  return frame_size;
}

void processCommands() {
  uint8_t command = 0;
  uint8_t argument = 0;
  bool changed = false;

  while (NativeUsbEsp32::takeCommand(&command, &argument)) {
    (void)argument;
    switch (command) {
      case NativeUsbProtocol::kCmdStart:
        g_streaming = true;
        changed = true;
        break;
      case NativeUsbProtocol::kCmdStop:
        g_streaming = false;
        changed = true;
        break;
      case NativeUsbProtocol::kCmdResetCounters:
        resetCounters();
        changed = false;
        break;
      case NativeUsbProtocol::kCmdPing:
      default:
        break;
    }
  }

  if (changed) {
    publishRuntimeStatus();
  }
}

void pumpStream() {
  if (!g_streaming || !NativeUsbEsp32::configured() || NativeUsbEsp32::txBusy()) {
    return;
  }

  const NativeUsbProtocol::BenchmarkConfig config = NativeUsbEsp32::getConfig();
  const uint32_t frame_size = buildFrame(config);

  if (NativeUsbEsp32::writeFrame(g_frame, static_cast<uint16_t>(frame_size))) {
    ++g_frames_sent;
    g_bytes_sent += frame_size;
  } else {
    ++g_frames_dropped;
  }

  // Sequence tracks generated application frames. A failed USB submission is
  // intentionally visible to the host as a sequence gap.
  ++g_sequence;
  publishRuntimeStatus();
}

}  // namespace Bench

void setup() {
  NativeUsbEsp32::begin();
  Bench::publishRuntimeStatus();
}

void loop() {
  NativeUsbEsp32::task();
  Bench::processCommands();
  Bench::pumpStream();
  delay(0);
}
