#include "benchmark.h"
#include <Arduino.h>
#include <cstring>
#include "../core/config.h"
#include "../generators/generators.h"
#include "../usb/rom_usb.h"

namespace Bench {
namespace {
alignas(4) uint8_t frames[2][BenchConfig::kMaxFrameSize];
uint8_t next_buffer = 0;
bool ready = false;
bool busy = false;
bool count_current = false;
bool streaming = false;
uint16_t ready_length = 0;
uint16_t active_length = 0;
uint32_t sequence = 0;
uint32_t generated = 0;
uint32_t sent = 0;
uint32_t dropped = 0;
uint64_t bytes = 0;

void publish() {
  RomUsb::setRuntime({streaming, generated, sent, dropped, bytes});
}

void resetCounters() {
  sequence = generated = sent = dropped = 0;
  bytes = 0;
  ready = false;
  count_current = false;  // Ignore a transfer from a previous run.
  publish();
}

void processCommands() {
  uint8_t command = 0, argument = 0;
  while (RomUsb::takeCommand(&command, &argument)) {
    (void)argument;
    switch (command) {
      case NativeUsbProtocol::kCmdStart:
        streaming = true;
        break;
      case NativeUsbProtocol::kCmdStop:
        streaming = false;
        if (ready) { ++dropped; ready = false; }
        break;
      case NativeUsbProtocol::kCmdResetCounters:
        resetCounters();
        break;
      default:
        break;
    }
    publish();
  }
}

void makeFrame(uint8_t* dst, const NativeUsbProtocol::BenchmarkConfig& cfg) {
  const uint32_t payload = cfg.frame_size - NativeUsbProtocol::kFrameHeaderSize;
  memcpy(dst, NativeUsbProtocol::kFrameMagic, 4);
  dst[4] = NativeUsbProtocol::kProtocolMajor;
  dst[5] = NativeUsbProtocol::kDataType;
  NativeUsbProtocol::putU16LE(dst + 6, static_cast<uint16_t>(cfg.generator));
  NativeUsbProtocol::putU32LE(dst + 8, sequence++);
  NativeUsbProtocol::putU32LE(dst + 12, payload);
  PayloadGenerators::fill(dst + 16, payload, cfg.generator, cfg.seed, sequence - 1);
  ++generated;
}

void pump() {
  if (busy) {
    bool done = false;
    if (RomUsb::pollBulkIn(&done)) {
      busy = false;
      if (count_current) {
        if (done) { ++sent; bytes += active_length; }
        else { ++dropped; }
        publish();
      }
      count_current = false;
    }
  }
  if (!streaming || !RomUsb::configured()) {
    if (ready) { ++dropped; ready = false; publish(); }
    return;
  }
  if (!ready) {
    const auto cfg = RomUsb::getConfig();
    if (cfg.frame_size < 64 || cfg.frame_size > BenchConfig::kMaxFrameSize) return;
    ready_length = static_cast<uint16_t>(cfg.frame_size);
    makeFrame(frames[next_buffer], cfg);
    ready = true;
    publish();
  }
  if (busy) return;  // Next buffer is ready while ROM handles previous transfer.
  if (RomUsb::startBulkIn(frames[next_buffer], ready_length)) {
    active_length = ready_length;
    busy = true;
    count_current = true;
    next_buffer ^= 1;
    ready = false;
  }
}
} // namespace

void begin() {
#if NUB_ROMUSB_LOG_LEVEL >= 1
  Serial.begin(115200);  // Logs only over USB-UART, never native USB CDC.
#endif
  const bool started = RomUsb::begin();
#if NUB_ROMUSB_LOG_LEVEL >= 1
  if (!started) Serial.println("[ROMUSB] Initialization failed");
#else
  (void)started;
#endif
  publish();
}
void tick() {
  RomUsb::task();
  if (streaming && !RomUsb::configured()) {
    streaming = false;
    ready = false;
    publish();
  }
  processCommands();
  pump();
  if (!streaming) {
    delay(1); // Let FreeRTOS schedule idle work.
  } else {
    static uint16_t since_yield = 0;
    if (++since_yield >= NUB_ROMUSB_LOOP_YIELD_EVERY) {
      since_yield = 0;
      delay(0); // Cooperate with FreeRTOS without yielding on every packet.
    }
  }
}
} // namespace Bench
