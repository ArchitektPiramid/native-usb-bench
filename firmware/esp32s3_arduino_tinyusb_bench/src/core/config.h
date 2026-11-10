#pragma once

#include <Arduino.h>
#include <esp_arduino_version.h>

#include "../protocol/nativeusb_protocol.h"

#define NUB_STRINGIFY_INNER(x) #x
#define NUB_STRINGIFY(x) NUB_STRINGIFY_INNER(x)

namespace BenchConfig {

// Development identifiers. Use an assigned VID/PID before shipping hardware.
constexpr uint16_t kVendorId = 0xCAFE;
constexpr uint16_t kProductId = 0x4013;
constexpr uint16_t kUsbDeviceVersionBcd = 0x0500;  // bcdDevice 5.00

constexpr const char* kManufacturer = "NativeUSB Bench";
constexpr const char* kProduct = "NativeUSB Bench ESP32-S3";
constexpr const char* kInterfaceName = "NativeUSB Vendor";

// Generic NativeUSB metadata returned by GET_DEVICE_INFO.
constexpr const char* kTargetName = "esp32-s3-fs";
constexpr const char* kMcuName = "ESP32-S3";
constexpr const char* kFrameworkName =
    "Arduino-ESP32 " NUB_STRINGIFY(ESP_ARDUINO_VERSION_MAJOR) "."
    NUB_STRINGIFY(ESP_ARDUINO_VERSION_MINOR) "."
    NUB_STRINGIFY(ESP_ARDUINO_VERSION_PATCH);
constexpr const char* kUsbStackName = "TinyUSB";
#ifdef ARDUINO_BOARD
constexpr const char* kBoardName = ARDUINO_BOARD;
#else
constexpr const char* kBoardName = "";
#endif

constexpr uint16_t kTargetId = 0x3203;
constexpr uint16_t kMcuWireId = 0x0003;

constexpr uint8_t kFirmwareMajor = 0;
constexpr uint8_t kFirmwareMinor = 1;
constexpr uint8_t kFirmwarePatch = 0;

// ESP32-S3 native USB is USB Full-Speed, so Bulk MPS is 64 B.
constexpr uint16_t kUsbMaxPacket = 64;

// Keep the S3 first target conservative and use the already-proven 4 KiB
// direct DCD transfer size as the maximum logical benchmark frame.
constexpr uint32_t kMinFrameSize = 64;
constexpr uint32_t kMaxFrameSize = 4096;
constexpr uint32_t kDefaultFrameSize = 4096;
constexpr NativeUsbProtocol::PayloadGenerator kDefaultGenerator =
    NativeUsbProtocol::PayloadGenerator::Xorshift32;
constexpr uint32_t kDefaultSeed = 0x6D2B79F5u;

constexpr uint32_t kUsbWriteTimeoutMs = 500;
constexpr size_t kCommandQueueSize = 8;

static_assert(kMinFrameSize >= NativeUsbProtocol::kFrameHeaderSize,
              "frame too small");
static_assert(kMaxFrameSize <= 0xFFFFu,
              "TinyUSB DCD transfer length must fit uint16_t");
static_assert((kDefaultFrameSize % kUsbMaxPacket) == 0,
              "default frame must align to Full-Speed Bulk packet size");

}  // namespace BenchConfig

#undef NUB_STRINGIFY
#undef NUB_STRINGIFY_INNER
