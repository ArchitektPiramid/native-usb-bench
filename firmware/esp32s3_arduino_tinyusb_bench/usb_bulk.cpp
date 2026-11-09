#include "usb_transport.h"
#include "usb_transport_internal.h"

#include <cstring>

#include "device/usbd_pvt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

namespace NativeUsbEsp32 {
using detail::vendor;
namespace {

constexpr size_t kBulkOutDrainBufferSize = 256;

uint8_t g_bulk_out_drain[kBulkOutDrainBufferSize] __attribute__((aligned(4)));
uint8_t g_vendor_bulk_in_endpoint = 0;
uint8_t g_vendor_bulk_out_endpoint = 0;
bool g_vendor_mounted_last_loop = false;
volatile bool g_tx_busy = false;
volatile uint32_t g_bulk_in_transfers = 0;
volatile uint32_t g_bulk_out_transfers = 0;


bool discoverVendorBulkEndpoints() {
  if (g_vendor_bulk_in_endpoint != 0 && g_vendor_bulk_out_endpoint != 0) {
    return true;
  }

  const uint8_t* cfg = tud_descriptor_configuration_cb(0);
  if (cfg == nullptr || cfg[0] < 9 || cfg[1] != TUSB_DESC_CONFIGURATION) {
    return false;
  }

  const uint16_t total_length = static_cast<uint16_t>(cfg[2]) |
                                (static_cast<uint16_t>(cfg[3]) << 8);
  size_t offset = cfg[0];
  bool in_vendor_interface = false;

  while (offset + 2 <= total_length) {
    const uint8_t len = cfg[offset];
    const uint8_t type = cfg[offset + 1];
    if (len < 2 || offset + len > total_length) {
      break;
    }

    if (type == TUSB_DESC_INTERFACE && len >= 9) {
      in_vendor_interface = (cfg[offset + 5] == TUSB_CLASS_VENDOR_SPECIFIC);
    } else if (in_vendor_interface && type == TUSB_DESC_ENDPOINT && len >= 7) {
      const uint8_t address = cfg[offset + 2];
      const uint8_t attributes = cfg[offset + 3] & 0x03;
      if (attributes == TUSB_XFER_BULK) {
        if ((address & 0x80u) != 0u) {
          g_vendor_bulk_in_endpoint = address;
        } else {
          g_vendor_bulk_out_endpoint = address;
        }
      }
    }

    offset += len;
  }

  return g_vendor_bulk_in_endpoint != 0 && g_vendor_bulk_out_endpoint != 0;
}

bool waitBulkInIdle(uint32_t timeout_ms) {
  if (!discoverVendorBulkEndpoints()) {
    return false;
  }

  const uint32_t started = millis();
  while (usbd_edpt_busy(0, g_vendor_bulk_in_endpoint)) {
    if (!vendor.mounted()) {
      return false;
    }
    if (static_cast<uint32_t>(millis() - started) >= timeout_ms) {
      return false;
    }
    taskYIELD();
  }
  return true;
}

bool directBulkInTransfer(const uint8_t* data, size_t size, uint32_t timeout_ms) {
  if (!vendor.mounted() || data == nullptr || size == 0 || size > 0xFFFFu) {
    return false;
  }
  if (!discoverVendorBulkEndpoints() || !waitBulkInIdle(timeout_ms)) {
    return false;
  }

  const uint32_t started = millis();
  while (!usbd_edpt_claim(0, g_vendor_bulk_in_endpoint)) {
    if (!vendor.mounted()) {
      return false;
    }
    if (static_cast<uint32_t>(millis() - started) >= timeout_ms) {
      return false;
    }
    taskYIELD();
  }

  g_tx_busy = true;
  const bool submitted = usbd_edpt_xfer(
      0,
      g_vendor_bulk_in_endpoint,
      const_cast<uint8_t*>(data),
      static_cast<uint16_t>(size),
      false);

  if (!submitted) {
    g_tx_busy = false;
    return false;
  }

  const bool completed = waitBulkInIdle(timeout_ms);
  g_tx_busy = false;
  if (completed) {
    ++g_bulk_in_transfers;
  }
  return completed;
}

void drainReservedBulkOut() {
  if (!vendor.mounted()) {
    return;
  }

  const uint32_t available = tud_vendor_n_available(0);
  if (available == 0) {
    return;
  }

  size_t remaining = available;
  while (remaining > 0) {
    const uint32_t request =
        remaining < sizeof(g_bulk_out_drain) ? remaining : sizeof(g_bulk_out_drain);
    const uint32_t count = tud_vendor_n_read(0, g_bulk_out_drain, request);
    if (count == 0) {
      break;
    }
    remaining -= count;
    ++g_bulk_out_transfers;
  }
}

}  // namespace

namespace detail {

bool bulkInTransferInProgress() { return g_tx_busy; }
uint32_t bulkInTransferCount() { return g_bulk_in_transfers; }
uint32_t bulkOutTransferCount() { return g_bulk_out_transfers; }

}  // namespace detail

void task() {
  const bool mounted = vendor.mounted();
  if (mounted && !g_vendor_mounted_last_loop) {
    (void)discoverVendorBulkEndpoints();
    // This one-shot re-arm was required by the proven ESP32-S3 transport path
    // on Arduino-ESP32 3.3.x. Never flush continuously while streaming.
    tud_vendor_n_read_flush(0);
  }

  if (mounted) {
    drainReservedBulkOut();
  } else {
    g_vendor_bulk_in_endpoint = 0;
    g_vendor_bulk_out_endpoint = 0;
  }

  g_vendor_mounted_last_loop = mounted;
}

bool configured() {
  return vendor.mounted();
}

bool txBusy() {
  if (!vendor.mounted() || !discoverVendorBulkEndpoints()) {
    return false;
  }
  return g_tx_busy || usbd_edpt_busy(0, g_vendor_bulk_in_endpoint);
}

bool writeFrame(const uint8_t* data, uint16_t length) {
  if (length < NativeUsbProtocol::kFrameHeaderSize ||
      length > BenchConfig::kMaxFrameSize ||
      (length % BenchConfig::kUsbMaxPacket) != 0) {
    return false;
  }
  return directBulkInTransfer(data, length, BenchConfig::kUsbWriteTimeoutMs);
}

}  // namespace NativeUsbEsp32
