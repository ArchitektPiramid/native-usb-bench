#pragma once
#include <stdint.h>
#include "../protocol/nativeusb_protocol.h"
namespace RomUsb {
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
