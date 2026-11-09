#include "usb_transport_internal.h"

#include <USB.h>
#include <cstring>

#include "config.h"

namespace NativeUsbEsp32 {
using detail::vendor;
namespace {

constexpr uint8_t kMsOsVendorCode = 0x20;
// Microsoft OS 1.0 string descriptor (index 0xEE).
const uint16_t kMsOsStringDescriptor[] = {
    0x0312,
    0x004D, 0x0053, 0x0046, 0x0054, 0x0031, 0x0030, 0x0030,
    0x0020,
};
static_assert(sizeof(kMsOsStringDescriptor) == 18, "MS OS string size");

// Microsoft OS 1.0 Extended Compatible ID descriptor. Interface 0 advertises
// WINUSB so Windows can bind the inbox Winusb.sys driver without Zadig/INF.
const uint8_t kMsExtCompatIdDescriptor[] = {
    0x28, 0x00, 0x00, 0x00,
    0x00, 0x01,
    0x04, 0x00,
    0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    0x00,
    0x01,
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static_assert(sizeof(kMsExtCompatIdDescriptor) == 40, "MS OS compat ID size");

// Microsoft OS 1.0 Extended Properties descriptor.
// DeviceInterfaceGUID = {D34C9B26-6F7B-4C2B-9E23-8D46A5B51C72}
const uint8_t kMsExtPropertiesDescriptor[] = {
    0x8E, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x00, 0x01, 0x00,
    0x84, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x28, 0x00,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,
    'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,
    'G',0,'U',0,'I',0,'D',0,0,0,
    0x4E, 0x00, 0x00, 0x00,
    '{',0,'D',0,'3',0,'4',0,'C',0,'9',0,'B',0,'2',0,'6',0,'-',0,
    '6',0,'F',0,'7',0,'B',0,'-',0,'4',0,'C',0,'2',0,'B',0,'-',0,
    '9',0,'E',0,'2',0,'3',0,'-',0,'8',0,'D',0,'4',0,'6',0,'A',0,
    '5',0,'B',0,'5',0,'1',0,'C',0,'7',0,'2',0,'}',0,0,0,
};
static_assert(sizeof(kMsExtPropertiesDescriptor) == 142,
              "MS OS properties size");

char g_serial_number[24] = "NUBS3-UNSET";

}  // namespace

namespace detail {

void makeSerialNumber() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(g_serial_number, sizeof(g_serial_number), "NUBS3-%012llX",
           static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
}

bool startUsbDevice() {
  USB.VID(BenchConfig::kVendorId);
  USB.PID(BenchConfig::kProductId);
  USB.usbClass(0x00);
  USB.usbSubClass(0x00);
  USB.usbProtocol(0x00);
  USB.usbVersion(0x0200);
  USB.firmwareVersion(BenchConfig::kUsbDeviceVersionBcd);
  USB.manufacturerName(BenchConfig::kManufacturer);
  USB.productName(BenchConfig::kProduct);
  USB.serialNumber(g_serial_number);

  // No CDC/MSC/DFU and no WebUSB landing-page descriptor here. Automatic
  // Windows binding is provided directly by the MS OS 1.0 descriptors above.
  return USB.begin();
}

bool handleMsOsRequest(uint8_t rhport, uint8_t stage,
                       arduino_usb_control_request_t const* request) {
  if (request == nullptr ||
      request->bmRequestDirection != REQUEST_DIRECTION_IN ||
      request->bmRequestType != REQUEST_TYPE_VENDOR ||
      (request->bmRequestRecipient != REQUEST_RECIPIENT_DEVICE &&
       request->bmRequestRecipient != REQUEST_RECIPIENT_INTERFACE) ||
      request->bRequest != kMsOsVendorCode) {
    return false;
  }

  if (stage != REQUEST_STAGE_SETUP) {
    return true;
  }

  const uint8_t* descriptor = nullptr;
  size_t descriptor_length = 0;
  switch (request->wIndex) {
    case 0x0004:
      descriptor = kMsExtCompatIdDescriptor;
      descriptor_length = sizeof(kMsExtCompatIdDescriptor);
      break;
    case 0x0005:
      descriptor = kMsExtPropertiesDescriptor;
      descriptor_length = sizeof(kMsExtPropertiesDescriptor);
      break;
    default:
      return false;
  }

  const size_t response_length =
      request->wLength < descriptor_length ? request->wLength : descriptor_length;
  return vendor.sendResponse(rhport, request,
                             const_cast<uint8_t*>(descriptor), response_length);
}

}  // namespace detail

const uint16_t* msOsStringDescriptorForTinyUsb() {
  return kMsOsStringDescriptor;
}

const char* serialNumberForTinyUsb() {
  return g_serial_number;
}

}  // namespace NativeUsbEsp32

// Arduino-ESP32 provides this TinyUSB string callback as a weak symbol. The
// sketch overrides it to add Microsoft OS string index 0xEE while preserving
// the normal manufacturer/product/serial/interface/configuration strings.
extern "C" uint16_t const* tud_descriptor_string_cb(uint8_t index,
                                                     uint16_t langid) {
  (void)langid;
  static uint16_t descriptor[127];

  if (index == 0xEE) {
    return NativeUsbEsp32::msOsStringDescriptorForTinyUsb();
  }

  if (index == 0) {
    descriptor[0] = 0x0304;
    descriptor[1] = 0x0409;
    return descriptor;
  }

  const char* text = nullptr;
  switch (index) {
    case 1:
      text = BenchConfig::kManufacturer;
      break;
    case 2:
      text = BenchConfig::kProduct;
      break;
    case 3:
      text = NativeUsbEsp32::serialNumberForTinyUsb();
      break;
    case 4:
      text = BenchConfig::kInterfaceName;
      break;
    case 5:
      text = "NativeUSB Bench Config";
      break;
    default:
      return nullptr;
  }

  size_t count = strlen(text);
  if (count > 126) {
    count = 126;
  }
  for (size_t i = 0; i < count; ++i) {
    descriptor[1 + i] = static_cast<uint8_t>(text[i]);
  }
  descriptor[0] = static_cast<uint16_t>((0x03u << 8) | (2u * count + 2u));
  return descriptor;
}
