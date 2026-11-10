#include "usb_transport.h"
#include "usb_transport_internal.h"

#include <cstring>

#include "../core/config.h"

namespace NativeUsbEsp32 {
using detail::vendor;
using NativeUsbProtocol::BenchmarkConfig;
using NativeUsbProtocol::PayloadGenerator;
using NativeUsbProtocol::RuntimeStatus;

namespace {

constexpr uint16_t kDeviceInfoSize = 144;
constexpr uint16_t kCapabilitiesSize = 32;
constexpr uint16_t kConfigSize = 24;
constexpr uint16_t kStatusSize = 48;

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

  const bool mounted = vendor.mounted();
  memset(g_status_buffer, 0, sizeof(g_status_buffer));
  memcpy(&g_status_buffer[0], "NST1", 4);
  NativeUsbProtocol::putU16LE(&g_status_buffer[4], kStatusSize);
  g_status_buffer[6] = 1;
  g_status_buffer[7] = mounted ? 3U : 1U;  // mirror configured/default semantics used by STM32
  g_status_buffer[8] = mounted ? 1U : 0U;
  g_status_buffer[9] = runtime.streaming ? 1U : 0U;
  g_status_buffer[10] = detail::bulkInTransferInProgress() ? 1U : 0U;
  g_status_buffer[11] = 0;
  NativeUsbProtocol::putU32LE(&g_status_buffer[12], runtime.frames_generated);
  NativeUsbProtocol::putU32LE(&g_status_buffer[16], runtime.frames_sent);
  NativeUsbProtocol::putU32LE(&g_status_buffer[20], runtime.frames_dropped);
  NativeUsbProtocol::putU64LE(&g_status_buffer[24], runtime.bytes_sent);
  NativeUsbProtocol::putU32LE(&g_status_buffer[32], detail::bulkInTransferCount());
  NativeUsbProtocol::putU32LE(&g_status_buffer[36], detail::bulkOutTransferCount());
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

}  // namespace

namespace detail {

bool handleNativeUsbRequest(uint8_t rhport, uint8_t stage,
                            arduino_usb_control_request_t const* request) {
  if (request == nullptr || request->bmRequestType != REQUEST_TYPE_VENDOR ||
      request->bmRequestRecipient != REQUEST_RECIPIENT_DEVICE || request->wIndex != 0) {
    return false;
  }

  if (request->bRequest == NativeUsbProtocol::kReqCommand &&
      request->bmRequestDirection == REQUEST_DIRECTION_OUT && request->wLength == 0) {
    if (stage == REQUEST_STAGE_SETUP) {
      return vendor.sendResponse(rhport, request);
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
      return vendor.sendResponse(rhport, request, g_status_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqDeviceInfo &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildDeviceInfo();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_device_info_buffer));
      return vendor.sendResponse(rhport, request, g_device_info_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqCapabilities &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildCapabilities();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_capabilities_buffer));
      return vendor.sendResponse(rhport, request, g_capabilities_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqConfig &&
      request->bmRequestDirection == REQUEST_DIRECTION_IN) {
    if (stage == REQUEST_STAGE_SETUP) {
      buildConfig();
      const size_t length = min(static_cast<size_t>(request->wLength),
                                sizeof(g_config_buffer));
      return vendor.sendResponse(rhport, request, g_config_buffer, length);
    }
    return true;
  }

  if (request->bRequest == NativeUsbProtocol::kReqSetConfig &&
      request->bmRequestDirection == REQUEST_DIRECTION_OUT && request->wLength == kConfigSize) {
    if (stage == REQUEST_STAGE_SETUP) {
      memset(g_config_rx_buffer, 0, sizeof(g_config_rx_buffer));
      // For control OUT with data, USBVendor writes the DATA stage into this
      // buffer before calling us again for REQUEST_STAGE_ACK.
      return vendor.sendResponse(rhport, request, g_config_rx_buffer,
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
  if (detail::handleMsOsRequest(rhport, stage, request)) {
    return true;
  }
  return handleNativeUsbRequest(rhport, stage, request);
}


}  // namespace detail

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

}  // namespace NativeUsbEsp32
