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

// TinyUSB owns the active buffer; prepare the next frame in the other one.
alignas(4) uint8_t g_frames[2][BenchConfig::kMaxFrameSize];
uint8_t g_next_buffer = 0;
bool g_frame_ready = false;
uint16_t g_ready_size = 0;
bool g_frame_in_flight = false;
bool g_count_in_flight = false;
uint16_t g_in_flight_size = 0;
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
  g_frame_ready = false;
  g_count_in_flight = false;  // Ignore the previous stream's pending completion.
  g_sequence = 0;
  g_frames_generated = 0;
  g_frames_sent = 0;
  g_frames_dropped = 0;
  g_bytes_sent = 0;
  publishRuntimeStatus();
}

uint16_t buildFrame(const NativeUsbProtocol::BenchmarkConfig& config,
                    uint8_t* frame) {
  const uint32_t frame_size = config.frame_size;
  const uint32_t payload_size = frame_size - NativeUsbProtocol::kFrameHeaderSize;

  memcpy(&frame[0], NativeUsbProtocol::kFrameMagic, 4);
  frame[4] = NativeUsbProtocol::kProtocolMajor;
  frame[5] = NativeUsbProtocol::kDataType;
  NativeUsbProtocol::putU16LE(
      &frame[6], static_cast<uint16_t>(static_cast<uint8_t>(config.generator)));
  NativeUsbProtocol::putU32LE(&frame[8], g_sequence);
  NativeUsbProtocol::putU32LE(&frame[12], payload_size);

  PayloadGenerators::fill(&frame[NativeUsbProtocol::kFrameHeaderSize], payload_size,
                          config.generator, config.seed, g_sequence);
  ++g_frames_generated;
  return static_cast<uint16_t>(frame_size);
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
        if (g_frame_ready) {
          ++g_frames_dropped;  // Staged frame was never submitted.
          g_frame_ready = false;
        }
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

void pollTransfer() {
  if (!g_frame_in_flight) {
    return;
  }
  const auto result = NativeUsbEsp32::pollBulkIn();
  if (result == NativeUsbEsp32::BulkInState::Pending) {
    return;
  }

  g_frame_in_flight = false;
  if (g_count_in_flight) {
    if (result == NativeUsbEsp32::BulkInState::Completed) {
      ++g_frames_sent;
      g_bytes_sent += g_in_flight_size;
    } else {
      ++g_frames_dropped;
    }
    publishRuntimeStatus();
  }
  g_count_in_flight = false;
}

void pumpStream() {
  if (!g_streaming || !NativeUsbEsp32::configured()) {
    if (g_frame_ready) {
      ++g_frames_dropped;
      g_frame_ready = false;
      publishRuntimeStatus();
    }
    return;
  }

  if (!g_frame_ready) {
    const auto config = NativeUsbEsp32::getConfig();
    g_ready_size = buildFrame(config, g_frames[g_next_buffer]);
    ++g_sequence;
    g_frame_ready = true;
    publishRuntimeStatus();
  }

  if (NativeUsbEsp32::txBusy()) {
    return;  // Next frame is already prepared while the current one transfers.
  }

  if (NativeUsbEsp32::writeFrame(g_frames[g_next_buffer], g_ready_size)) {
    g_in_flight_size = g_ready_size;
    g_frame_in_flight = true;
    g_count_in_flight = true;
    g_next_buffer ^= 1;
  } else {
    ++g_frames_dropped;
    publishRuntimeStatus();
  }
  g_frame_ready = false;
}

}  // namespace Bench

void setup() {
  NativeUsbEsp32::begin();
  Bench::publishRuntimeStatus();
}

void loop() {
  NativeUsbEsp32::task();
  Bench::pollTransfer();
  Bench::processCommands();
  Bench::pumpStream();
  delay(0);
}
