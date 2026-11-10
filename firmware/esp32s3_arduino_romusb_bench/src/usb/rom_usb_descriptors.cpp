#include "rom_usb_internal.h"
#include "descriptors.h"
#include <cstring>
#include <cerrno>

namespace RomUsb {
namespace Internal {
namespace {
static uint8_t usb_string[128] __attribute__((aligned(4)));
}
// Windows WCID and standard USB descriptor requests on EP0.
int customRequest(usb_setup_packet* setup, int32_t* length, uint8_t** data) {
  if (!setup || !length || !data || setup->bmRequestType != 0x80 ||
      setup->bRequest != 6) return -EINVAL;

  // Handle critical Chapter 9 descriptors here, rather than relying on the
  // ROM fallback. The ROM still handles SET_ADDRESS and SET_CONFIGURATION.
  const uint8_t type = static_cast<uint8_t>(setup->wValue >> 8);
  const uint8_t index = static_cast<uint8_t>(setup->wValue);
  if (type == 1 && index == 0) {
    *length = setup->wLength < 18 ? setup->wLength : 18;
    *data = const_cast<uint8_t*>(RomUsbDescriptors::kDescriptors);
    return 0;
  }
  if (type == 2 && index == 0) {
    constexpr int32_t kConfigLength = 32;
    *length = setup->wLength < kConfigLength ? setup->wLength : kConfigLength;
    *data = const_cast<uint8_t*>(RomUsbDescriptors::kDescriptors + 18);
    return 0;
  }
  if (type != 3) return -EINVAL;
  if (index == 0xEE) {
    *length = sizeof(RomUsbDescriptors::kMsString);
    *data = const_cast<uint8_t*>(RomUsbDescriptors::kMsString);
    return 0;
  }
  if (index == 0) {
    static uint8_t lang[] = {4,3,0x09,0x04};
    *length = sizeof(lang);
    *data = lang;
    return 0;
  }
  const char* str = nullptr;
  switch (index) {
    case 1: str = "NativeUSB Bench"; break;
    case 2: str = "NativeUSB Bench ESP32-S3 ROM"; break;
    case 3: str = "NUBROM-S3"; break;
    case 4: str = "NativeUSB Vendor"; break;
    default: return -EINVAL;
  }
  const size_t count = strnlen(str, 62);
  usb_string[0] = 2 + count * 2;
  usb_string[1] = 3;
  for (size_t i = 0; i < count; ++i) {
    usb_string[2 + i * 2] = static_cast<uint8_t>(str[i]);
    usb_string[3 + i * 2] = 0;
  }
  *length = usb_string[0];
  *data = usb_string;
  return 0;
}

} // namespace Internal
} // namespace RomUsb
