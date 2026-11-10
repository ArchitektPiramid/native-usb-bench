#include "rom_usb.h"
#include "rom_usb_internal.h"
#include "descriptors.h"
#include <Arduino.h>
#include <cstring>
#include <cerrno>
#include "../core/config.h"

namespace RomUsb {
namespace Internal {
using NativeUsbProtocol::BenchmarkConfig;
using NativeUsbProtocol::RuntimeStatus;
using NativeUsbProtocol::PayloadGenerator;
namespace {
constexpr uint16_t kInfoSize = 144;
constexpr uint16_t kCapsSize = 32;
constexpr uint16_t kStatusSize = 48;
constexpr uint16_t kCfgSize = 24;
static uint8_t response[160] __attribute__((aligned(4)));
static BenchmarkConfig config {PayloadGenerator::Xorshift32,
                               static_cast<uint8_t>(NativeUsbProtocol::kModeIn),
                               BenchConfig::kDefaultFrameSize, BenchConfig::kSeed};
static RuntimeStatus runtime {false,0,0,0,0};
static uint32_t command_count = 0;
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
  response[10] = bulkBusy();
  NativeUsbProtocol::putU32LE(response + 12, runtime.frames_generated);
  NativeUsbProtocol::putU32LE(response + 16, runtime.frames_sent);
  NativeUsbProtocol::putU32LE(response + 20, runtime.frames_dropped);
  NativeUsbProtocol::putU64LE(response + 24, runtime.bytes_sent);
  NativeUsbProtocol::putU32LE(response + 32, bulkInCount());
  NativeUsbProtocol::putU32LE(response + 36, bulkOutCount());
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

// The ROM control engine calls the vendor handler *after* receiving the OUT
// data stage. Its data pointer refers to the completed request buffer.
// Apply before the status stage so a subsequent GET_CONFIG sees the new values.
int applyConfig(const uint8_t* data, int32_t length) {
  if (!data || length != kCfgSize ||
      memcmp(data, "NCF1", 4) != 0 ||
      NativeUsbProtocol::getU16LE(data + 4) != kCfgSize || data[6] != 1) {
    return -EINVAL;
  }
  const uint32_t frame = NativeUsbProtocol::getU32LE(data + 12);
  if (data[7] > static_cast<uint8_t>(PayloadGenerator::Lfsr32) ||
      data[8] != NativeUsbProtocol::kModeIn ||
      frame < BenchConfig::kMps || frame > BenchConfig::kMaxFrameSize ||
      frame % BenchConfig::kMps != 0) {
    return -EINVAL;
  }
  if (runtime.streaming || bulkBusy()) return -EBUSY;

  config = {static_cast<PayloadGenerator>(data[7]), data[8], frame,
            NativeUsbProtocol::getU32LE(data + 16)};
  return 0;
}

bool enqueueCommand(uint8_t command, uint8_t arg) {
  const uint8_t next = (command_head + 1) % BenchConfig::kCommandQueueLength;
  if (next == command_tail) return false;
  commands[command_head] = {command,arg};
  command_head = next;
  ++command_count;
  return true;
}

} // namespace

// ROM handles standard USB requests; these are NativeUSB vendor commands.
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
    // The ROM handler is invoked after the control OUT data stage. Never
    // replace *data: it already contains the received NCF1 configuration.
    const int rc = applyConfig(*data, *length);
    if (rc != 0) return rc;
    *length = 0;  // Zero-length status response to the host.
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

uint32_t runtimeFramesSent() { return runtime.frames_sent; }
} // namespace Internal

NativeUsbProtocol::BenchmarkConfig getConfig() { return Internal::config; }
void setRuntime(const NativeUsbProtocol::RuntimeStatus& status) { Internal::runtime = status; }
bool takeCommand(uint8_t* code, uint8_t* arg) {
  using namespace Internal;
  if (!code || !arg || command_tail == command_head) return false;
  const auto cmd = commands[command_tail];
  command_tail = (command_tail + 1) % BenchConfig::kCommandQueueLength;
  *code = cmd.code;
  *arg = cmd.arg;
  return true;
}
}  // namespace RomUsb
