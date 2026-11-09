#include "usb_transport.h"

#include <Arduino.h>
#include <USB.h>
#include <USBVendor.h>
#include <cstring>

#include "device/usbd_pvt.h"
#include "esp32-hal-tinyusb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

USBVendor Vendor(BenchConfig::kUsbMaxPacket);

namespace NativeUsbEsp32 {
namespace {

using NativeUsbProtocol::BenchmarkConfig;
using NativeUsbProtocol::PayloadGenerator;
using NativeUsbProtocol::RuntimeStatus;

constexpr uint8_t kMsOsVendorCode = 0x20;
constexpr uint16_t kDeviceInfoSize = 144;
constexpr uint16_t kCapabilitiesSize = 32;
constexpr uint16_t kConfigSize = 24;
constexpr uint16_t kStatusSize = 48;
constexpr size_t kBulkOutDrainBufferSize = 256;

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

portMUX_TYPE g_state_mux = portMUX_INITIALIZER_UNLOCKED;

BenchmarkConfig g_config{
    BenchConfig::kDefaultGenerator,
    static_cast<uint8_t>(NativeUsbProtocol::kModeIn),
    BenchConfig::kDefaultFrameSize,
    BenchConfig::kDefaultSeed,
};
RuntimeStatus g_runtime{false, 0, 0, 0, 0};

struct CommandEntry {
  uint8_t command;
  uint8_t argument;
};
CommandEntry g_command_queue[BenchConfig::kCommandQueueSize]{};
volatile size_t g_command_head = 0;
volatile size_t g_command_tail = 0;

uint8_t g_config_rx_buffer[kConfigSize] __attribute__((aligned(4)));
uint8_t g_device_info_buffer[kDeviceInfoSize] __attribute__((aligned(4)));
uint8_t g_capabilities_buffer[kCapabilitiesSize] __attribute__((aligned(4)));
uint8_t g_config_buffer[kConfigSize] __attribute__((aligned(4)));
uint8_t g_status_buffer[kStatusSize] __attribute__((aligned(4)));
uint8_t g_bulk_out_drain[kBulkOutDrainBufferSize] __attribute__((aligned(4)));

char g_serial_number[24] = "NUBS3-UNSET";
uint8_t g_vendor_bulk_in_endpoint = 0;
uint8_t g_vendor_bulk_out_endpoint = 0;
bool g_vendor_mounted_last_loop = false;
volatile bool g_tx_busy = false;
volatile uint32_t g_bulk_in_transfers = 0;
volatile uint32_t g_bulk_out_transfers = 0;
volatile uint32_t g_control_commands = 0;

void copyFixedString(uint8_t* dst, size_t size, const char* text) {
  memset(dst, 0, size);
  if (text == nullptr || size == 0) {
    return;
  }
  strncpy(reinterpret_cast<char*>(dst), text, size - 1);
}

bool isSupportedGenerator(uint8_t value) {
  return value <= static_cast<uint8_t>(PayloadGenerator::Lfsr32);
}

bool isValidConfig(const BenchmarkConfig& config) {
  return isSupportedGenerator(static_cast<uint8_t>(config.generator)) &&
         config.mode == static_cast<uint8_t>(NativeUsbProtocol::kModeIn) &&
         config.frame_size >= BenchConfig::kMinFrameSize &&
         config.frame_size <= BenchConfig::kMaxFrameSize &&
         config.frame_size >= NativeUsbProtocol::kFrameHeaderSize &&
         (config.frame_size % BenchConfig::kUsbMaxPacket) == 0;
}

void makeSerialNumber() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(g_serial_number, sizeof(g_serial_number), "NUBS3-%012llX",
           static_cast<unsigned long long>(mac & 0xFFFFFFFFFFFFULL));
}

void buildDeviceInfo() {
  memset(g_device_info_buffer, 0, sizeof(g_device_info_buffer));
  memcpy(&g_device_info_buffer[0], "NDI1", 4);
  NativeUsbProtocol::putU16LE(&g_device_info_buffer[4], kDeviceInfoSize);
  g_device_info_buffer[6] = NativeUsbProtocol::kProtocolMajor;
  g_device_info_buffer[7] = NativeUsbProtocol::kProtocolMinor;
  g_device_info_buffer[8] = BenchConfig::kFirmwareMajor;
  g_device_info_buffer[9] = BenchConfig::kFirmwareMinor;
  g_device_info_buffer[10] = BenchConfig::kFirmwarePatch;
  g_device_info_buffer[11] = 0;
  NativeUsbProtocol::putU32LE(
      &g_device_info_buffer[12], static_cast<uint32_t>(getCpuFrequencyMhz()) * 1000000u);
  NativeUsbProtocol::putU16LE(&g_device_info_buffer[16], BenchConfig::kTargetId);
  NativeUsbProtocol::putU16LE(&g_device_info_buffer[18], BenchConfig::kMcuWireId);
  copyFixedString(&g_device_info_buffer[20], 24, BenchConfig::kTargetName);
  copyFixedString(&g_device_info_buffer[44], 24, BenchConfig::kMcuName);
  copyFixedString(&g_device_info_buffer[68], 24, BenchConfig::kFrameworkName);
  copyFixedString(&g_device_info_buffer[92], 24, BenchConfig::kUsbStackName);
  copyFixedString(&g_device_info_buffer[116], 24, BenchConfig::kBoardName);
}

void buildCapabilities() {
  memset(g_capabilities_buffer, 0, sizeof(g_capabilities_buffer));
  memcpy(&g_capabilities_buffer[0], "NCP1", 4);
  NativeUsbProtocol::putU16LE(&g_capabilities_buffer[4], kCapabilitiesSize);
  g_capabilities_buffer[6] = 1;
  g_capabilities_buffer[7] = 0;
  NativeUsbProtocol::putU32LE(&g_capabilities_buffer[8], NativeUsbProtocol::kModeIn);
  const uint32_t generators =
      NativeUsbProtocol::generatorBit(PayloadGenerator::Ramp8) |
      NativeUsbProtocol::generatorBit(PayloadGenerator::Xorshift32) |
      NativeUsbProtocol::generatorBit(PayloadGenerator::Lfsr32);
  NativeUsbProtocol::putU32LE(&g_capabilities_buffer[12], generators);
  NativeUsbProtocol::putU32LE(&g_capabilities_buffer[16], BenchConfig::kMinFrameSize);
  NativeUsbProtocol::putU32LE(&g_capabilities_buffer[20], BenchConfig::kMaxFrameSize);
  NativeUsbProtocol::putU32LE(&g_capabilities_buffer[24], BenchConfig::kDefaultFrameSize);
  NativeUsbProtocol::putU32LE(&g_capabilities_buffer[28], 0x00000007u);
}

void buildConfig() {
  BenchmarkConfig config{};
  portENTER_CRITICAL(&g_state_mux);
  config = g_config;
  portEXIT_CRITICAL(&g_state_mux);

  memset(g_config_buffer, 0, sizeof(g_config_buffer));
  memcpy(&g_config_buffer[0], "NCF1", 4);
  NativeUsbProtocol::putU16LE(&g_config_buffer[4], kConfigSize);
  g_config_buffer[6] = 1;
  g_config_buffer[7] = static_cast<uint8_t>(config.generator);
  g_config_buffer[8] = config.mode;
  g_config_buffer[9] = 0;
  NativeUsbProtocol::putU16LE(&g_config_buffer[10], 0);
  NativeUsbProtocol::putU32LE(&g_config_buffer[12], config.frame_size);
  NativeUsbProtocol::putU32LE(&g_config_buffer[16], config.seed);
  NativeUsbProtocol::putU32LE(&g_config_buffer[20], 0x00000001u);
}

void buildStatus() {
  RuntimeStatus runtime{};
  portENTER_CRITICAL(&g_state_mux);
  runtime = g_runtime;
  portEXIT_CRITICAL(&g_state_mux);

  const bool mounted = Vendor.mounted();
  memset(g_status_buffer, 0, sizeof(g_status_buffer));
  memcpy(&g_status_buffer[0], "NST1", 4);
  NativeUsbProtocol::putU16LE(&g_status_buffer[4], kStatusSize);
  g_status_buffer[6] = 1;
  g_status_buffer[7] = mounted ? 3U : 1U;  // mirror configured/default semantics used by STM32
  g_status_buffer[8] = mounted ? 1U : 0U;
  g_status_buffer[9] = runtime.streaming ? 1U : 0U;
  g_status_buffer[10] = g_tx_busy ? 1U : 0U;
  g_status_buffer[11] = 0;
  NativeUsbProtocol::putU32LE(&g_status_buffer[12], runtime.frames_generated);
  NativeUsbProtocol::putU32LE(&g_status_buffer[16], runtime.frames_sent);
  NativeUsbProtocol::putU32LE(&g_status_buffer[20], runtime.frames_dropped);
  NativeUsbProtocol::putU64LE(&g_status_buffer[24], runtime.bytes_sent);
  NativeUsbProtocol::putU32LE(&g_status_buffer[32], g_bulk_in_transfers);
  NativeUsbProtocol::putU32LE(&g_status_buffer[36], g_bulk_out_transfers);
  NativeUsbProtocol::putU32LE(&g_status_buffer[40], g_control_commands);
  NativeUsbProtocol::putU32LE(&g_status_buffer[44], millis());
}

bool parseConfig(const uint8_t* data, size_t length, BenchmarkConfig* out) {
  if (data == nullptr || out == nullptr || length != kConfigSize ||
      memcmp(data, "NCF1", 4) != 0 ||
      NativeUsbProtocol::getU16LE(&data[4]) != kConfigSize || data[6] != 1) {
    return false;
  }

  BenchmarkConfig config{};
  config.generator = static_cast<PayloadGenerator>(data[7]);
  config.mode = data[8];
  config.frame_size = NativeUsbProtocol::getU32LE(&data[12]);
  config.seed = NativeUsbProtocol::getU32LE(&data[16]);

  if (!isValidConfig(config)) {
    return false;
  }

  *out = config;
  return true;
}

bool enqueueCommand(uint8_t command, uint8_t argument) {
  bool ok = false;
  portENTER_CRITICAL(&g_state_mux);
  const size_t next = (g_command_head + 1u) % BenchConfig::kCommandQueueSize;
  if (next != g_command_tail) {
    g_command_queue[g_command_head] = CommandEntry{command, argument};
    g_command_head = next;
    ++g_control_commands;
    ok = true;
  }
  portEXIT_CRITICAL(&g_state_mux);
  return ok;
}

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
    if (!Vendor.mounted()) {
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
  if (!Vendor.mounted() || data == nullptr || size == 0 || size > 0xFFFFu) {
    return false;
  }
  if (!discoverVendorBulkEndpoints() || !waitBulkInIdle(timeout_ms)) {
    return false;
  }

  const uint32_t started = millis();
  while (!usbd_edpt_claim(0, g_vendor_bulk_in_endpoint)) {
    if (!Vendor.mounted()) {
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
  return Vendor.sendResponse(rhport, request,
                             const_cast<uint8_t*>(descriptor), response_length);
}

bool handleNativeUsbRequest(uint8_t rhport, uint8_t stage,
                            arduino_usb_control_request_t const* request) {
  if (request == nullptr || request->bmRequestType != REQUEST_TYPE_VENDOR ||
      request->bmRequestRecipient != REQUEST_RECIPIENT_DEVICE || request->wIndex != 0) {
    return false;
  }

  if (request->bRequest == NativeUsbProtocol::kReqCommand &&
      request->bmRequestDirection == REQUEST_DIRECTION_OUT && request->wLength == 0) {
    if (stage == REQUEST_STAGE_SETUP) {
      return Vendor.sendResponse(rhport, request);
    }
    if (stage == REQUEST_STAGE_ACK) {
      return enqueueCommand(static_cast<uint8_t>(request->wValue & 0xFFu),
                            static_cast<uint8_t>((request->wValue >> 8) & 0xFFu));
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqStatus &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildStatus();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_status_buffer));
      return Vendor.sendResponse(rhport, request, g_status_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqDeviceInfo &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildDeviceInfo();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_device_info_buffer));
      return Vendor.sendResponse(rhport, request, g_device_info_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqCapabilities &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildCapabilities();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_capabilities_buffer));
      return Vendor.sendResponse(rhport, request, g_capabilities_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqConfig &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildConfig();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_config_buffer));
      return Vendor.sendResponse(rhport, request, g_config_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqSetConfig &&
      request->bmRequestDirection == REQUEST_DIRECTION_OUT && request->wLength == kConfigSize) {
    if (stage == REQUEST_STAGE_SETUP) {
      memset(g_config_rx_buffer, 0, sizeof(g_config_rx_buffer));
      // For control OUT with data, USBVendor writes the DATA stage into this
      // buffer before calling us again for REQUEST_STAGE_ACK.
      return Vendor.sendResponse(rhport, request, g_config_rx_buffer,
                                 sizeof(g_config_rx_buffer));
    }
    if (stage == REQUEST_STAGE_ACK) {
      BenchmarkConfig parsed{};
      if (parseConfig(g_config_rx_buffer, sizeof(g_config_rx_buffer), &parsed)) {
        portENTER_CRITICAL(&g_state_mux);
        const bool streaming = g_runtime.streaming;
        if (!streaming) {
          g_config = parsed;
        }
        portEXIT_CRITICAL(&g_state_mux);
      }
      ++g_control_commands;
      return true;
    }
    return true;
  }

  return false;
}

bool vendorRequestCallback(uint8_t rhport, uint8_t stage,
                           arduino_usb_control_request_t const* request) {
  if (handleMsOsRequest(rhport, stage, request)) {
    return true;
  }
  return handleNativeUsbRequest(rhport, stage, request);
}

void drainReservedBulkOut() {
  if (!Vendor.mounted()) {
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

bool begin() {
  makeSerialNumber();

  Vendor.onRequest(vendorRequestCallback);
  Vendor.setRxBufferSize(512);
  Vendor.begin();

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

void task() {
  const bool mounted = Vendor.mounted();
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
  return Vendor.mounted();
}

bool txBusy() {
  if (!Vendor.mounted() || !discoverVendorBulkEndpoints()) {
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

bool takeCommand(uint8_t* command, uint8_t* argument) {
  if (command == nullptr || argument == nullptr) {
    return false;
  }

  bool available = false;
  portENTER_CRITICAL(&g_state_mux);
  if (g_command_tail != g_command_head) {
    const CommandEntry entry = g_command_queue[g_command_tail];
    g_command_tail = (g_command_tail + 1u) % BenchConfig::kCommandQueueSize;
    *command = entry.command;
    *argument = entry.argument;
    available = true;
  }
  portEXIT_CRITICAL(&g_state_mux);
  return available;
}

BenchmarkConfig getConfig() {
  BenchmarkConfig config{};
  portENTER_CRITICAL(&g_state_mux);
  config = g_config;
  portEXIT_CRITICAL(&g_state_mux);
  return config;
}

void updateRuntimeStatus(const RuntimeStatus& status) {
  portENTER_CRITICAL(&g_state_mux);
  g_runtime = status;
  portEXIT_CRITICAL(&g_state_mux);
}

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
