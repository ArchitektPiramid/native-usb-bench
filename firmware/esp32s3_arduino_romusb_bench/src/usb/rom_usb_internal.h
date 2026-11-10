#pragma once
#include <stdint.h>
#include "esp32s3/rom/usb/usb_device.h"
#include "esp32s3/rom/usb/usb_dc.h"

// Internal boundaries between ROM USB device, control and Bulk modules.
// Application code should use rom_usb.h instead.
namespace RomUsb {
namespace Internal {
constexpr uint8_t kEpIn = 0x81;
constexpr uint8_t kEpOut = 0x01;
extern volatile bool connected;
extern bool usb_started;

int vendorRequest(usb_setup_packet* setup, int32_t* length, uint8_t** data);
int customRequest(usb_setup_packet* setup, int32_t* length, uint8_t** data);
void inCallback(uint8_t ep, usb_dc_ep_cb_status_code status);
void outCallback(uint8_t ep, usb_dc_ep_cb_status_code status);
void resetBulkState();
void invalidateBulkEndpoint();
bool ensureBulkInEndpoint();
void serviceBulkIn();
void bulkDiagnosticTick();
bool bulkBusy();
uint32_t bulkInCount();
uint32_t bulkOutCount();
uint32_t runtimeFramesSent();
}  // namespace Internal
}  // namespace RomUsb
