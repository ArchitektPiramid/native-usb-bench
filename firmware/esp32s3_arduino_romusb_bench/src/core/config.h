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
constexpr uint8_t kFirmwareMinor = 2;
constexpr uint8_t kFirmwarePatch = 1;
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
// 0: remove all profiler code from the firmware; 1: periodic UART summary.
#ifndef NUB_ROMUSB_PROFILE
#define NUB_ROMUSB_PROFILE 0
#endif
#ifndef NUB_ROMUSB_PROFILE_INTERVAL_MS
#define NUB_ROMUSB_PROFILE_INTERVAL_MS 5000
#endif
static_assert(NUB_ROMUSB_PROFILE_INTERVAL_MS >= 1000, "profiler reporting interval must be >= 1 second");
// Compile-time options shared by separately compiled Arduino .cpp files.
// Transfer size and yield interval are instead selected in the sketch .ino.
#ifndef NUB_ROMUSB_IRAM_IN_CALLBACK
#define NUB_ROMUSB_IRAM_IN_CALLBACK 1
#endif
static_assert(NUB_ROMUSB_IRAM_IN_CALLBACK == 0 || NUB_ROMUSB_IRAM_IN_CALLBACK == 1,
              "IRAM callback option must be 0 or 1");
