#include "rom_usb.h"
#include "rom_usb_internal.h"
#include <Arduino.h>
#include <cerrno>
#include "../core/config.h"
#include "hal/usb_dwc_ll.h"
#include "../core/rom_usb_profile.h"

namespace RomUsb {
namespace Internal {
namespace {
static uint8_t out_packet[64] __attribute__((aligned(4)));
static volatile bool tx_busy = false;
static volatile bool tx_done = false;
static volatile int tx_result = -1;
static volatile uint16_t tx_requested = 0;
// One ROM DCD write owns the frame buffer until completion. A write may
// contain multiple full-speed 64-byte packets; the ISR only signals completion.
static uint8_t* tx_frame = nullptr;
static volatile uint16_t tx_offset = 0;
static volatile uint16_t tx_inflight = 0;
static volatile bool tx_packet_done = false;
#if NUB_ROMUSB_LOG_LEVEL >= 2
static volatile uint32_t tx_chunks = 0;
static volatile uint32_t tx_callbacks = 0;
#endif
static volatile uint32_t tx_errors = 0;
static volatile int tx_last_error = 0;
static uint32_t tx_last_progress = 0;
// The ROM may reject or only partly accept multi-packet writes. Keep the
// smaller working size across frames until the next USB reset.
static uint16_t tx_chunk_limit = NUB_ROMUSB_BULK_CHUNK_BYTES;
static uint8_t tx_fifo_busy_retries = 0;

// Halve the request while keeping it aligned to whole USB FS packets.
void shrinkBulkChunk() {
  const uint16_t half_packets = static_cast<uint16_t>(tx_chunk_limit / (2 * BenchConfig::kMps));
  tx_chunk_limit = half_packets ? static_cast<uint16_t>(half_packets * BenchConfig::kMps)
                                : BenchConfig::kMps;
}
static uint8_t tx_timeout_checks = 0;
// The ROM core may enumerate successfully without enabling the vendor Bulk IN EP.
static volatile bool ep1_ready = false;
static uint32_t ep1_last_attempt = 0;
static uint32_t bulk_in_count = 0;
static uint32_t bulk_out_count = 0;

} // namespace

void outCallback(uint8_t ep, usb_dc_ep_cb_status_code status) {
  if (status != USB_DC_EP_DATA_OUT) return;
  uint32_t length = 0;
  if (usb_dc_ep_read(ep, out_packet, sizeof(out_packet), &length) == 0) ++bulk_out_count;
}
void inCallback(uint8_t ep, usb_dc_ep_cb_status_code status) {
  // Accept the address or the index for ROM versions that omit direction.
  if ((ep != kEpIn && ep != (kEpIn & 0x0Fu)) || status != USB_DC_EP_DATA_IN) return;
#if NUB_ROMUSB_LOG_LEVEL >= 2
  ++tx_callbacks;
#endif
  if (tx_busy && tx_inflight != 0) {
#if NUB_ROMUSB_PROFILE
    RomUsbProfile::callbackFromIsr();
#endif
    tx_packet_done = true;
  }
}

// Activate EP1 only after Windows has selected configuration 1. Keep the ROM
// Chapter 9/EP0 configuration untouched; it already enumerates correctly.
bool ensureBulkInEndpoint() {
  if (!usb_started || !connected) {
    ep1_ready = false;
    return false;
  }
  // Reset/configuration callbacks invalidate this cache. Avoid a ROM call and
  // repeated endpoint register reads on every iteration of the Arduino loop.
  if (ep1_ready) return true;
  if (usb_dev_get_configuration() != 1 || tx_busy) return false;

  const uint32_t now = millis();
  if (ep1_last_attempt && now - ep1_last_attempt < 1000) return false;
  ep1_last_attempt = now ? now : 1;

  auto* const hw = USB_DWC_LL_GET_HW(0);
  constexpr uint32_t kEp1Irq = 1u << 1;
  constexpr uint32_t kEpActive = 1u << 15;
  const bool active = (hw->in_eps[0].diepctl_reg.val & kEpActive) != 0;
  const usb_dc_ep_cfg_data desc{kEpIn, BenchConfig::kMps, USB_DC_EP_BULK};
  const int cfg_rc = active ? 0 : usb_dc_ep_configure(&desc);
  const int cb_rc = cfg_rc == 0 ? usb_dc_ep_set_callback(kEpIn, inCallback) : -1;
  const int en_rc = cb_rc == 0 ? usb_dc_ep_enable(kEpIn) : -1;
  if (cfg_rc == 0 && cb_rc == 0 && en_rc == 0) {
    // Some ROM revisions leave Bulk IN masked after SET_CONFIGURATION.
    // Never change the already-working EP0 configuration.
    if (!(hw->daintmsk_reg.val & kEp1Irq)) hw->daintmsk_reg.val |= kEp1Irq;
    if (!hw->diepmsk_reg.xfercomplmsk) hw->diepmsk_reg.xfercomplmsk = 1;
  }
  ep1_ready = cfg_rc == 0 && cb_rc == 0 && en_rc == 0 &&
              (hw->in_eps[0].diepctl_reg.val & kEpActive) &&
              (hw->daintmsk_reg.val & kEp1Irq);
#if NUB_ROMUSB_LOG_LEVEL >= 1
  if (!ep1_ready) {
    Serial.printf("[ROMUSB] EP1 init failed: config=%d cb=%d enable=%d\n",
                  cfg_rc, cb_rc, en_rc);
  }
#endif
  return ep1_ready;
}

// Advance only after the previous DCD write completes; keep frame order intact.
void serviceBulkIn() {
  if (!tx_busy || !tx_frame || tx_done) return;

  if (tx_packet_done) {
    tx_packet_done = false;
#if NUB_ROMUSB_PROFILE
    RomUsbProfile::callbackServiced();
#endif
    tx_offset = static_cast<uint16_t>(tx_offset + tx_inflight);
    tx_inflight = 0;
    tx_last_progress = millis();
  }
  if (tx_inflight != 0) {
    // A stalled endpoint is rare. Sample its watchdog every 16 loop passes
    // instead of reading the system clock on each busy poll.
    if ((++tx_timeout_checks & 15u) == 0u &&
        millis() - tx_last_progress > 1500) {
      ++tx_errors;
      tx_last_error = -ETIMEDOUT;
      tx_result = -1;
      tx_done = true;
    }
    return;
  }
  if (tx_offset == tx_requested) {
    tx_result = tx_requested;
    tx_done = true;
    return;
  }

  const uint32_t remaining = tx_requested - tx_offset;
  const uint32_t requested = remaining > tx_chunk_limit
      ? tx_chunk_limit : remaining;
  uint32_t scheduled = 0;
  // Protect ownership even if ROM invokes the completion synchronously.
  tx_inflight = static_cast<uint16_t>(requested);
#if NUB_ROMUSB_PROFILE
  const uint32_t write_started = RomUsbProfile::cycles();
#endif
  const int rc = usb_dc_ep_write(kEpIn, tx_frame + tx_offset, requested, &scheduled);
#if NUB_ROMUSB_PROFILE
  RomUsbProfile::recordWrite(RomUsbProfile::cycles() - write_started,
                             requested, scheduled, rc);
#endif
  if (rc == 0 && scheduled > 0 && scheduled <= requested &&
      scheduled % BenchConfig::kMps == 0) {
    // A full FIFO may limit a write to fewer complete 64 B USB packets.
    // Only advance by bytes actually accepted, after the completion callback.
    tx_inflight = static_cast<uint16_t>(scheduled);
    tx_fifo_busy_retries = 0;
    if (scheduled < requested && scheduled >= BenchConfig::kMps) {
      tx_chunk_limit = static_cast<uint16_t>(scheduled);
    }
#if NUB_ROMUSB_LOG_LEVEL >= 2
    ++tx_chunks;
#endif
    tx_last_progress = millis();
    tx_timeout_checks = 0;
    return;
  }
  tx_inflight = 0;
  if (rc == -EAGAIN || rc == -EBUSY || (rc == 0 && scheduled == 0)) {
    // Fall back after repeated FIFO backpressure; never discard queued data.
    if (++tx_fifo_busy_retries >= 8 && tx_chunk_limit > BenchConfig::kMps) {
      shrinkBulkChunk();
      tx_fifo_busy_retries = 0;
    }
    return;
  }
  if ((rc == -EMSGSIZE || rc == -ENOSPC || rc == -EINVAL) &&
      tx_chunk_limit > BenchConfig::kMps) {
    // No bytes were accepted. Retry this same offset with a smaller write.
    shrinkBulkChunk();
    tx_fifo_busy_retries = 0;
    return;
  }
  ++tx_errors;
  tx_last_error = rc ? rc : -EMSGSIZE;
  tx_result = -1;
  tx_done = true;
}


// Reset stale transfer ownership when the host resets or disconnects.
void resetBulkState() {
  tx_busy = false;
  tx_done = true;
  tx_result = -1;
  tx_inflight = 0;
  tx_packet_done = false;
  tx_frame = nullptr;
  tx_chunk_limit = NUB_ROMUSB_BULK_CHUNK_BYTES;
  tx_fifo_busy_retries = 0;
  tx_timeout_checks = 0;
  ep1_ready = false;
}
void invalidateBulkEndpoint() { ep1_ready = false; }
bool bulkBusy() { return tx_busy; }
uint32_t bulkInCount() { return bulk_in_count; }
uint32_t bulkOutCount() { return bulk_out_count; }
void bulkDiagnosticTick() {
#if NUB_ROMUSB_LOG_LEVEL >= 1
  static int last_reported_error = 0;
  if (tx_last_error && tx_last_error != last_reported_error) {
    last_reported_error = tx_last_error;
    Serial.printf("[ROMUSB] Bulk IN error: %d (offset=%u/%u)\n",
                  last_reported_error, static_cast<unsigned>(tx_offset),
                  static_cast<unsigned>(tx_requested));
  }
#endif
#if NUB_ROMUSB_LOG_LEVEL >= 2
  static uint32_t last_report = 0;
  const uint32_t now = millis();
  if (now - last_report >= 5000) {
    last_report = now;
    Serial.printf("[ROMUSB] cfg=%d chunks=%lu completed=%lu errors=%lu frames=%lu\n",
                  usb_dev_get_configuration(),
                  static_cast<unsigned long>(tx_chunks),
                  static_cast<unsigned long>(tx_callbacks),
                  static_cast<unsigned long>(tx_errors),
                  static_cast<unsigned long>(runtimeFramesSent()));
  }
#endif
}
} // namespace Internal

bool startBulkIn(uint8_t* frame, uint16_t size) {
  using namespace Internal;
  if (!configured() || tx_busy || !frame || size == 0 || !ensureBulkInEndpoint()) return false;
  // Use packet-level ROM DCD; higher-level usb_transfer() may stall on this target.
  uint8_t stalled = 0;
  if (usb_dc_ep_is_stalled(kEpIn, &stalled) == 0 && stalled) {
    tx_last_error = -EPIPE;
    return false;
  }
  tx_busy = true;
  tx_done = false;
  tx_packet_done = false;
  tx_result = -1;
  tx_requested = size;
  tx_offset = 0;
  tx_inflight = 0;
  tx_timeout_checks = 0;
  tx_frame = frame;
  tx_last_progress = millis();
  ++bulk_in_count;
  // Queue the first DCD write now, not after another full Arduino loop.
  // Do not call usb_dc_ep_write() from the ROM interrupt callback.
  serviceBulkIn();
  return true;
}
bool pollBulkIn(bool* completed) {
  using namespace Internal;
  if (!tx_done) return false;
  const int result = tx_result;
  tx_done = false;
  tx_busy = false;
  tx_frame = nullptr;
  if (completed) *completed = result == tx_requested;
  return true;
}
} // namespace RomUsb
