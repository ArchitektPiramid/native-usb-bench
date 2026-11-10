#include "rom_usb.h"
#include "descriptors.h"
#include <Arduino.h>
#include <cstring>
#include <cerrno>

// ESP32-S3 ROM USB device stack: no TinyUSB headers or API calls.
#include "esp32s3/rom/usb/usb_device.h"
#include "esp32s3/rom/usb/cdc_acm.h"
#include "esp32s3/rom/usb/usb_dc.h"
#include "esp_intr_alloc.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "hal/usb_wrap_ll.h"
#include "hal/usb_dwc_ll.h"
#include "esp32s3/rom/usb/usb_os_glue.h"
#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace RomUsb {
namespace {
using NativeUsbProtocol::BenchmarkConfig;
using NativeUsbProtocol::RuntimeStatus;
using NativeUsbProtocol::PayloadGenerator;
constexpr uint8_t kEpIn = 0x81;
constexpr uint8_t kEpOut = 0x01;
constexpr uint16_t kInfoSize = 144;
constexpr uint16_t kCapsSize = 32;
constexpr uint16_t kStatusSize = 48;
constexpr uint16_t kCfgSize = 24;

static uint8_t response[160] __attribute__((aligned(4)));
// Keep ROM EP0 scratch buffers separate from application configuration data.
// USB Chapter 9 and vendor requests may exceed the 24-byte SET_CONFIG payload.
static uint8_t ep0_payload[256] __attribute__((aligned(4)));
static uint8_t ep0_vendor[256] __attribute__((aligned(4)));
static uint8_t config_rx[kCfgSize] __attribute__((aligned(4)));
static bool config_waiting = false;
static uint8_t out_packet[64] __attribute__((aligned(4)));
static uint8_t usb_string[128] __attribute__((aligned(4)));
static BenchmarkConfig config {PayloadGenerator::Xorshift32,
                               static_cast<uint8_t>(NativeUsbProtocol::kModeIn),
                               BenchConfig::kDefaultFrameSize, BenchConfig::kSeed};
static RuntimeStatus runtime {false,0,0,0,0};
static volatile bool connected = false;
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
// The ROM core may enumerate successfully without enabling the vendor Bulk IN EP.
static volatile bool ep1_ready = false;
static uint32_t ep1_last_attempt = 0;
static uint32_t bulk_in_count = 0;
static uint32_t bulk_out_count = 0;
static uint32_t command_count = 0;
static intr_handle_t usb_interrupt = nullptr;
static portMUX_TYPE usb_lock = portMUX_INITIALIZER_UNLOCKED;

static bool usb_started = false;
static int init_error = 0;

// ROM USB requires OS callbacks before usb_enable().
void usbDisableInterrupts() { portENTER_CRITICAL_SAFE(&usb_lock); }
void usbEnableInterrupts() { portEXIT_CRITICAL_SAFE(&usb_lock); }
int usbWait(int delay_us) {
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING || xPortInIsrContext()) {
    if (delay_us > 0) esp_rom_delay_us(delay_us);
    return delay_us > 0 ? delay_us : 1;
  }
  if (delay_us <= 0) { taskYIELD(); return 1; }
  if (delay_us < 1000) { esp_rom_delay_us(delay_us); return delay_us; }
  const TickType_t ticks = pdMS_TO_TICKS((delay_us + 999) / 1000);
  vTaskDelay(ticks ? ticks : 1);
  return (ticks ? ticks : 1) * portTICK_PERIOD_MS * 1000;
}

struct Command { uint8_t code, arg; };
static Command commands[BenchConfig::kCommandQueueLength];
static volatile uint8_t command_head = 0;
static volatile uint8_t command_tail = 0;

void fixedString(uint8_t* dst, uint32_t length, const char* src) {
  memset(dst, 0, length);
  if (src) strncpy(reinterpret_cast<char*>(dst), src, length - 1);
}
void buildInfo() {
  memset(response, 0, kInfoSize);
  memcpy(response, "NDI1", 4);
  NativeUsbProtocol::putU16LE(response + 4, kInfoSize);
  response[6] = 1; // Protocol major
  response[7] = 0;
  response[8] = BenchConfig::kFirmwareMajor;
  response[9] = BenchConfig::kFirmwareMinor;
  response[10] = BenchConfig::kFirmwarePatch;
  NativeUsbProtocol::putU32LE(response + 12, getCpuFrequencyMhz() * 1000000u);
  NativeUsbProtocol::putU16LE(response + 16, BenchConfig::kTargetId);
  NativeUsbProtocol::putU16LE(response + 18, BenchConfig::kMcuWireId);
  fixedString(response + 20, 24, BenchConfig::kTargetName);
  fixedString(response + 44, 24, BenchConfig::kMcuName);
  fixedString(response + 68, 24, BenchConfig::kFramework);
  fixedString(response + 92, 24, BenchConfig::kUsbStack);
  fixedString(response + 116, 24, BenchConfig::kBoard);
}
void buildCapabilities() {
  memset(response, 0, kCapsSize);
  memcpy(response, "NCP1", 4);
  NativeUsbProtocol::putU16LE(response + 4, kCapsSize);
  response[6] = 1;
  NativeUsbProtocol::putU32LE(response + 8, NativeUsbProtocol::kModeIn);
  NativeUsbProtocol::putU32LE(response + 12, 7);
  NativeUsbProtocol::putU32LE(response + 16, 64);
  NativeUsbProtocol::putU32LE(response + 20, BenchConfig::kMaxFrameSize);
  NativeUsbProtocol::putU32LE(response + 24, BenchConfig::kDefaultFrameSize);
  NativeUsbProtocol::putU32LE(response + 28, 7);
}
void buildStatus() {
  memset(response, 0, kStatusSize);
  memcpy(response, "NST1", 4);
  NativeUsbProtocol::putU16LE(response + 4, kStatusSize);
  response[6] = 1;
  response[7] = connected ? 3 : 1;
  response[8] = connected ? 1 : 0;
  response[9] = runtime.streaming;
  response[10] = tx_busy;
  NativeUsbProtocol::putU32LE(response + 12, runtime.frames_generated);
  NativeUsbProtocol::putU32LE(response + 16, runtime.frames_sent);
  NativeUsbProtocol::putU32LE(response + 20, runtime.frames_dropped);
  NativeUsbProtocol::putU64LE(response + 24, runtime.bytes_sent);
  NativeUsbProtocol::putU32LE(response + 32, bulk_in_count);
  NativeUsbProtocol::putU32LE(response + 36, bulk_out_count);
  NativeUsbProtocol::putU32LE(response + 40, command_count);
  NativeUsbProtocol::putU32LE(response + 44, millis());
}
void buildConfig() {
  memset(response, 0, kCfgSize);
  memcpy(response, "NCF1", 4);
  NativeUsbProtocol::putU16LE(response + 4, kCfgSize);
  response[6] = 1;
  response[7] = static_cast<uint8_t>(config.generator);
  response[8] = config.mode;
  NativeUsbProtocol::putU32LE(response + 12, config.frame_size);
  NativeUsbProtocol::putU32LE(response + 16, config.seed);
  NativeUsbProtocol::putU32LE(response + 20, 1);
}

void parseConfigIfReady() {
  if (memcmp(config_rx, "NCF1", 4) != 0 ||
      NativeUsbProtocol::getU16LE(config_rx + 4) != kCfgSize || config_rx[6] != 1) return;
  const uint32_t frame = NativeUsbProtocol::getU32LE(config_rx + 12);
  if (config_rx[7] > 2 || config_rx[8] != NativeUsbProtocol::kModeIn ||
      frame < 64 || frame > BenchConfig::kMaxFrameSize || frame % BenchConfig::kMps) return;
  if (runtime.streaming || tx_busy) return;
  config = {static_cast<PayloadGenerator>(config_rx[7]), config_rx[8], frame,
            NativeUsbProtocol::getU32LE(config_rx + 16)};
  memset(config_rx, 0, sizeof(config_rx)); // Consume once after the OUT data stage.
  config_waiting = false;
}

bool enqueueCommand(uint8_t command, uint8_t arg) {
  const uint8_t next = (command_head + 1) % BenchConfig::kCommandQueueLength;
  if (next == command_tail) return false;
  commands[command_head] = {command,arg};
  command_head = next;
  ++command_count;
  return true;
}

// Vendor requests: ROM handles USB Chapter 9, we handle NativeUSB EP0 only.
int vendorRequest(usb_setup_packet* setup, int32_t* length, uint8_t** data) {
  if (!setup || !length || !data) return -EINVAL;
  if (setup->bRequest == 0x20 &&
      (setup->bmRequestType == 0xC0 || setup->bmRequestType == 0xC1)) {
    if (setup->wIndex == 4) {
      *length = sizeof(RomUsbDescriptors::kMsCompatId);
      *data = const_cast<uint8_t*>(RomUsbDescriptors::kMsCompatId);
      return 0;
    }
    if (setup->wIndex == 5) {
      *length = sizeof(RomUsbDescriptors::kMsProperties);
      *data = const_cast<uint8_t*>(RomUsbDescriptors::kMsProperties);
      return 0;
    }
  }
  if (setup->wIndex != 0) return -EINVAL;
  if (setup->bmRequestType == 0x40 && setup->bRequest == NativeUsbProtocol::kReqCommand &&
      setup->wLength == 0) {
    if (!enqueueCommand(setup->wValue & 0xFF, setup->wValue >> 8)) return -EBUSY;
    *length = 0;
    return 0;
  }
  if (setup->bmRequestType == 0x40 && setup->bRequest == NativeUsbProtocol::kReqSetConfig &&
      setup->wLength == kCfgSize) {
    // The ROM control engine writes the OUT data into this persistent buffer.
    if (!config_waiting) {
      memset(config_rx, 0, sizeof(config_rx));
      config_waiting = true;
    }
    *length = kCfgSize;
    *data = config_rx;
    return 0;
  }
  if (setup->bmRequestType != 0xC0) return -EINVAL;
  switch (setup->bRequest) {
    case NativeUsbProtocol::kReqDeviceInfo: buildInfo(); *length = kInfoSize; break;
    case NativeUsbProtocol::kReqCapabilities: buildCapabilities(); *length = kCapsSize; break;
    case NativeUsbProtocol::kReqStatus: buildStatus(); *length = kStatusSize; break;
    case NativeUsbProtocol::kReqConfig: buildConfig(); *length = kCfgSize; break;
    default: return -EINVAL;
  }
  *data = response;
  return 0;
}

// Handle string descriptors including Microsoft WCID index 0xEE.
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

void interfaceConfigured(uint8_t) { connected = true; }
void statusCallback(usb_dc_status_code status, uint8_t*) {
  if (status == USB_DC_RESET || status == USB_DC_DISCONNECTED) {
    connected = false;
    tx_busy = false;
    tx_done = true;
    tx_result = -1;
    tx_inflight = 0;
    tx_packet_done = false;
    tx_frame = nullptr;
    ep1_ready = false;
  }
  if (status == USB_DC_CONFIGURED) {
    connected = true;
    ep1_ready = false;
  }
}
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
  if (tx_busy && tx_inflight != 0) tx_packet_done = true;
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

void serviceBulkIn() {
  if (!tx_busy || !tx_frame || tx_done) return;

  if (tx_packet_done) {
    tx_packet_done = false;
    tx_offset = static_cast<uint16_t>(tx_offset + tx_inflight);
    tx_inflight = 0;
    tx_last_progress = millis();
  }
  if (tx_inflight != 0) {
    if (millis() - tx_last_progress > 1500) {
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
  const uint32_t requested = remaining > NUB_ROMUSB_BULK_CHUNK_BYTES
      ? NUB_ROMUSB_BULK_CHUNK_BYTES : remaining;
  uint32_t scheduled = 0;
  // Protect ownership even if ROM invokes the completion synchronously.
  tx_inflight = static_cast<uint16_t>(requested);
  const int rc = usb_dc_ep_write(kEpIn, tx_frame + tx_offset, requested, &scheduled);
  if (rc == 0 && scheduled > 0 && scheduled <= requested &&
      scheduled % BenchConfig::kMps == 0) {
    // A full FIFO may limit a write to fewer complete 64 B USB packets.
    // Only advance by bytes actually accepted, after the completion callback.
    tx_inflight = static_cast<uint16_t>(scheduled);
#if NUB_ROMUSB_LOG_LEVEL >= 2
    ++tx_chunks;
#endif
    tx_last_progress = millis();
    return;
  }
  tx_inflight = 0;
  if (rc == -EAGAIN || rc == -EBUSY || (rc == 0 && scheduled == 0)) {
    return; // FIFO busy; retry without advancing the frame buffer.
  }
  ++tx_errors;
  tx_last_error = rc ? rc : -EMSGSIZE;
  tx_result = -1;
  tx_done = true;
}

void usbIsr(void*) {
  usb_dc_check_poll_for_interrupts();
}

static usb_ep_cfg_data endpoints[] = {
    {inCallback, kEpIn},
    {outCallback, kEpOut},
};
static usb_cfg_data usb_config = {
    RomUsbDescriptors::kDescriptors,
    RomUsbDescriptors::kDescriptors + RomUsbDescriptors::kInterfaceOffset,
    interfaceConfigured,
    statusCallback,
    {nullptr, vendorRequest, customRequest, ep0_payload, ep0_vendor},
    2,
    endpoints,
};
}  // namespace

bool begin() {
  // Restore ROM USB ownership after any bootloader CDC session.
  usb_dev_deinit();
  usb_dw_ctrl_deinit();
  uart_acm_dev = nullptr;
  rom_usb_osglue.int_dis_proc = usbDisableInterrupts;
  rom_usb_osglue.int_ena_proc = usbEnableInterrupts;
  rom_usb_osglue.wait_proc = usbWait;

  usb_wrap_ll_phy_enable_pad(&USB_WRAP, true);
  usb_wrap_ll_phy_enable_external(&USB_WRAP, false);

  const esp_err_t irq_rc = esp_intr_alloc(
      ETS_USB_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_INTRDISABLED,
      usbIsr, nullptr, &usb_interrupt);
  if (irq_rc != ESP_OK) {
    init_error = static_cast<int>(irq_rc);
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] IRQ allocation failed: %d\n", init_error);
#endif
    return false;
  }
  // Both calls are necessary: the first installs descriptors/EP0 handlers,
  // the second attaches the USB controller to the bus.
  init_error = usb_set_config(&usb_config);
  if (init_error != 0) {
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] usb_set_config failed: %d\n", init_error);
#endif
    esp_intr_free(usb_interrupt);
    usb_interrupt = nullptr;
    return false;
  }
  init_error = usb_enable(&usb_config);
  if (init_error != 0) {
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] usb_enable failed: %d\n", init_error);
#endif
    usb_deconfig();
    esp_intr_free(usb_interrupt);
    usb_interrupt = nullptr;
    return false;
  }
  usb_dc_check_poll_for_interrupts();
  usb_dwc_ll_gahbcfg_en_global_intr(USB_DWC_LL_GET_HW(0));
  const esp_err_t enable_rc = esp_intr_enable(usb_interrupt);
  if (enable_rc != ESP_OK) {
    init_error = static_cast<int>(enable_rc);
#if NUB_ROMUSB_LOG_LEVEL >= 1
    Serial.printf("[ROMUSB] IRQ enable failed: %d\n", init_error);
#endif
    usb_disable();
    usb_deconfig();
    esp_intr_free(usb_interrupt);
    usb_interrupt = nullptr;
    return false;
  }
  usb_started = true;
#if NUB_ROMUSB_LOG_LEVEL >= 2
  Serial.println("[ROMUSB] Ready");
#endif
  return true;
}

void task() {
  // Avoid scanning the EP0 config buffer during high-rate Bulk IN streaming.
  if (config_waiting) parseConfigIfReady();
  if (usb_started && connected && !ep1_ready) ensureBulkInEndpoint();
  serviceBulkIn();
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
                  static_cast<unsigned long>(runtime.frames_sent));
  }
#endif
}
bool configured() { return usb_started && connected; }

bool startBulkIn(uint8_t* frame, uint16_t size) {
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
  tx_frame = frame;
  tx_last_progress = millis();
  ++bulk_in_count;
  return true;
}
bool pollBulkIn(bool* completed) {
  if (!tx_done) return false;
  const int result = tx_result;
  tx_done = false;
  tx_busy = false;
  tx_frame = nullptr;
  if (completed) *completed = result == tx_requested;
  return true;
}
BenchmarkConfig getConfig() { return config; }
void setRuntime(const RuntimeStatus& status) { runtime = status; }
bool takeCommand(uint8_t* code, uint8_t* arg) {
  if (!code || !arg || command_tail == command_head) return false;
  const auto cmd = commands[command_tail];
  command_tail = (command_tail + 1) % BenchConfig::kCommandQueueLength;
  *code = cmd.code;
  *arg = cmd.arg;
  return true;
}
}  // namespace RomUsb
