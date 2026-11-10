#pragma once

#include <Arduino.h>

namespace NativeUsbProtocol {

constexpr uint8_t kProtocolMajor = 1;
constexpr uint8_t kProtocolMinor = 0;

constexpr uint8_t kFrameMagic[4] = {'N', 'U', 'B', '1'};
constexpr uint8_t kDataType = 0x90;
constexpr uint16_t kFrameHeaderSize = 16;

// EP0 vendor-device requests. These values intentionally match the STM32
// target so the host protocol is MCU/framework independent.
constexpr uint8_t kReqCommand = 0x30;
constexpr uint8_t kReqStatus = 0x31;
constexpr uint8_t kReqDeviceInfo = 0x32;
constexpr uint8_t kReqCapabilities = 0x33;
constexpr uint8_t kReqConfig = 0x34;
constexpr uint8_t kReqSetConfig = 0x35;

constexpr uint8_t kCmdPing = 0x01;
constexpr uint8_t kCmdStart = 0x02;
constexpr uint8_t kCmdStop = 0x03;
constexpr uint8_t kCmdResetCounters = 0x04;

constexpr uint32_t kModeIn = 1u << 0;
constexpr uint32_t kModeOut = 1u << 1;
constexpr uint32_t kModeDuplex = 1u << 2;
constexpr uint32_t kModeEcho = 1u << 3;

enum class PayloadGenerator : uint8_t {
  Ramp8 = 0,
  Xorshift32 = 1,
  Lfsr32 = 2,
};

constexpr uint32_t generatorBit(PayloadGenerator generator) {
  return 1u << static_cast<uint8_t>(generator);
}

struct BenchmarkConfig {
  PayloadGenerator generator;
  uint8_t mode;
  uint32_t frame_size;
  uint32_t seed;
};

struct RuntimeStatus {
  bool streaming;
  uint32_t frames_generated;
  uint32_t frames_sent;
  uint32_t frames_dropped;
  uint64_t bytes_sent;
};

static inline void putU16LE(uint8_t* dst, uint16_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
}

static inline void putU32LE(uint8_t* dst, uint32_t value) {
  dst[0] = static_cast<uint8_t>(value);
  dst[1] = static_cast<uint8_t>(value >> 8);
  dst[2] = static_cast<uint8_t>(value >> 16);
  dst[3] = static_cast<uint8_t>(value >> 24);
}

static inline void putU64LE(uint8_t* dst, uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i) {
    dst[i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

static inline uint16_t getU16LE(const uint8_t* src) {
  return static_cast<uint16_t>(src[0]) |
         (static_cast<uint16_t>(src[1]) << 8);
}

static inline uint32_t getU32LE(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) |
         (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) |
         (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace NativeUsbProtocol
