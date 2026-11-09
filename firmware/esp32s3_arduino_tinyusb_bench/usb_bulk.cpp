#include "usb_transport.h"
#include "usb_transport_internal.h"

#include "device/usbd_pvt.h"
#include "config.h"

namespace NativeUsbEsp32 {
using detail::vendor;
namespace {

constexpr size_t kBulkOutDrainBufferSize = 256;

uint8_t g_bulk_out_drain[kBulkOutDrainBufferSize] __attribute__((aligned(4)));
uint8_t g_vendor_bulk_in_endpoint = 0;
uint8_t g_vendor_bulk_out_endpoint = 0;
bool g_vendor_mounted_last_loop = false;
bool g_tx_in_flight = false;
bool g_tx_timed_out = false;
uint32_t g_tx_started_ms = 0;
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

// Submit one frame; TinyUSB keeps the buffer until the endpoint becomes idle.
bool submitBulkIn(const uint8_t* data, size_t size) {
  if (!vendor.mounted() || data == nullptr || size == 0 || size > 0xFFFFu ||
      !discoverVendorBulkEndpoints() || g_tx_in_flight ||
      usbd_edpt_busy(0, g_vendor_bulk_in_endpoint) ||
      !usbd_edpt_claim(0, g_vendor_bulk_in_endpoint)) {
    return false;
  }

  if (!usbd_edpt_xfer(0, g_vendor_bulk_in_endpoint,
                     const_cast<uint8_t*>(data), static_cast<uint16_t>(size), false)) {
    return false;
  }

  g_tx_in_flight = true;
  g_tx_timed_out = false;
  g_tx_started_ms = millis();
  return true;
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

bool bulkInTransferInProgress() { return g_tx_in_flight; }
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

BulkInState pollBulkIn() {
  if (!g_tx_in_flight) {
    return BulkInState::Idle;
  }
  if (!vendor.mounted()) {
    g_tx_in_flight = false;
    return BulkInState::Failed;
  }
  if (usbd_edpt_busy(0, g_vendor_bulk_in_endpoint)) {
    // Do not reuse a timed-out buffer while the controller still owns it.
    if (static_cast<uint32_t>(millis() - g_tx_started_ms) >=
        BenchConfig::kUsbWriteTimeoutMs) {
      g_tx_timed_out = true;
    }
    return BulkInState::Pending;
  }

  g_tx_in_flight = false;
  if (g_tx_timed_out) {
    return BulkInState::Failed;
  }
  ++g_bulk_in_transfers;
  return BulkInState::Completed;
}

bool txBusy() {
  if (!vendor.mounted() || !discoverVendorBulkEndpoints()) {
    return false;
  }
  return g_tx_in_flight || usbd_edpt_busy(0, g_vendor_bulk_in_endpoint);
}

bool writeFrame(const uint8_t* data, uint16_t length) {
  if (length < NativeUsbProtocol::kFrameHeaderSize ||
      length > BenchConfig::kMaxFrameSize ||
      (length % BenchConfig::kUsbMaxPacket) != 0) {
    return false;
  }
  return submitBulkIn(data, length);
}

}  // namespace NativeUsbEsp32
