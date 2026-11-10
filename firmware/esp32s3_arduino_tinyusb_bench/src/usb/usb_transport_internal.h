#pragma once

#include <Arduino.h>
#include <USBVendor.h>
#include "esp32-hal-tinyusb.h"

namespace NativeUsbEsp32 {
namespace detail {

extern USBVendor vendor;

// USB descriptor and Microsoft OS 1.0 handling.
void makeSerialNumber();
bool startUsbDevice();
bool handleMsOsRequest(uint8_t rhport, uint8_t stage,
                       arduino_usb_control_request_t const* request);

// Shared EP0 callback; the application state remains owned by usb_control.cpp.
bool vendorRequestCallback(uint8_t rhport, uint8_t stage,
                           arduino_usb_control_request_t const* request);

// Read-only transport statistics for the EP0 status response.
bool bulkInTransferInProgress();
uint32_t bulkInTransferCount();
uint32_t bulkOutTransferCount();

}  // namespace detail
}  // namespace NativeUsbEsp32
