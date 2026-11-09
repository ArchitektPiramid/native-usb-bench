#pragma once

#include <Arduino.h>

#include "nativeusb_protocol.h"

namespace NativeUsbEsp32 {

bool begin();
void task();

bool configured();
bool txBusy();
bool writeFrame(const uint8_t* data, uint16_t length);

bool takeCommand(uint8_t* command, uint8_t* argument);
NativeUsbProtocol::BenchmarkConfig getConfig();
void updateRuntimeStatus(const NativeUsbProtocol::RuntimeStatus& status);

}  // namespace NativeUsbEsp32
