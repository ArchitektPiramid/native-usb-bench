#pragma once
#include <stdint.h>
#include "../protocol/nativeusb_protocol.h"
namespace RomUsb {
// Select a fixed write request size; no automatic size negotiation or fallback.
enum class BulkWriteSize : uint16_t {
  Bytes64 = 64,
  Bytes128 = 128,
  Bytes256 = 256,
  Bytes512 = 512,
  Bytes1024 = 1024,
  Bytes2048 = 2048,
  Bytes4096 = 4096,
};
// Call before begin(); the value is held constant for each firmware run.
void setBulkWriteSize(BulkWriteSize size);
// Initialize the ROM USB stack, OS callbacks and hardware interrupt.
bool begin();
// Pump nonblocking Bulk transfers from the Arduino loop.
void task();
bool configured();
// The caller owns frame memory until pollBulkIn reports completion.
bool startBulkIn(uint8_t* frame, uint16_t size);
bool pollBulkIn(bool* completed);
NativeUsbProtocol::BenchmarkConfig getConfig();
void setRuntime(const NativeUsbProtocol::RuntimeStatus& runtime);
bool takeCommand(uint8_t* command, uint8_t* argument);
}
