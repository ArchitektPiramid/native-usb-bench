#include "usb_transport.h"

#include "../core/config.h"
#include "usb_transport_internal.h"

namespace NativeUsbEsp32 {
namespace detail {

USBVendor vendor(BenchConfig::kUsbMaxPacket);

}  // namespace detail

bool begin() {
  detail::makeSerialNumber();

  detail::vendor.onRequest(detail::vendorRequestCallback);
  detail::vendor.setRxBufferSize(512);
  detail::vendor.begin();

  return detail::startUsbDevice();
}

}  // namespace NativeUsbEsp32
