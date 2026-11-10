#pragma once
#include <Arduino.h>
#include <esp_arduino_version.h>
#include "../protocol/nativeusb_protocol.h"

#define NUB_STR_I(x) #x
#define NUB_STR(x) NUB_STR_I(x)
namespace BenchConfig {
// Same development VID/PID and NUB1 wire protocol as the TinyUSB target.
constexpr uint16_t kVendorId = 0xCAFE;
constexpr uint16_t kProductId = 0x4013;
constexpr uint16_t kTargetId = 0x3203;
constexpr uint16_t kMcuWireId = 0x0003;
constexpr uint8_t kFirmwareMajor = 0;
constexpr uint8_t kFirmwareMinor = 1;
constexpr uint8_t kFirmwarePatch = 0;
constexpr uint16_t kMps = 64;
constexpr uint32_t kMaxFrameSize = 16384;
constexpr uint32_t kDefaultFrameSize = 4096;
constexpr uint32_t kSeed = 0x6D2B79F5;
constexpr uint8_t kCommandQueueLength = 8;
constexpr const char* kFramework = "Arduino-ESP32 " NUB_STR(ESP_ARDUINO_VERSION_MAJOR) "."
    NUB_STR(ESP_ARDUINO_VERSION_MINOR) "." NUB_STR(ESP_ARDUINO_VERSION_PATCH);
constexpr const char* kUsbStack = "ESP-ROM USB";
constexpr const char* kTargetName = "esp32-s3-fs";
constexpr const char* kMcuName = "ESP32-S3";
#ifdef ARDUINO_BOARD
constexpr const char* kBoard = ARDUINO_BOARD;
#else
constexpr const char* kBoard = "";
#endif
}  // namespace BenchConfig
#undef NUB_STR
#undef NUB_STR_I

// 0: silent; 1: initialization/errors; 2: periodic diagnostics.
#ifndef NUB_ROMUSB_LOG_LEVEL
#define NUB_ROMUSB_LOG_LEVEL 0
#endif
// Bytes scheduled per Bulk IN write; 64 is the on-device tested baseline.
#ifndef NUB_ROMUSB_BULK_CHUNK_BYTES
#define NUB_ROMUSB_BULK_CHUNK_BYTES 64
#endif
static_assert(NUB_ROMUSB_BULK_CHUNK_BYTES >= BenchConfig::kMps &&
              NUB_ROMUSB_BULK_CHUNK_BYTES <= 4096 &&
              NUB_ROMUSB_BULK_CHUNK_BYTES % BenchConfig::kMps == 0,
              "ROM USB chunk size must be 64..4096 bytes, in multiples of 64");
