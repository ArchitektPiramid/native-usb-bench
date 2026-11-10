#pragma once
#include <stdint.h>
#include "../protocol/nativeusb_protocol.h"
namespace RomUsb {
bool begin();
void task();
bool configured();
// Ownership of frame remains with caller until transfer completion.
bool startBulkIn(uint8_t* frame, uint16_t size);
bool pollBulkIn(bool* completed);
NativeUsbProtocol::BenchmarkConfig getConfig();
void setRuntime(const NativeUsbProtocol::RuntimeStatus& runtime);
bool takeCommand(uint8_t* command, uint8_t* argument);
}
