#pragma once

#include <Arduino.h>

#include "nativeusb_protocol.h"

namespace PayloadGenerators {

// Identical generator semantics to the STM32 target. Any frame can be
// reproduced independently from (seed, sequence), which makes host validation
// robust to dropped frames and parser resynchronisation.
static inline uint32_t frameSeed(uint32_t seed, uint32_t sequence) {
  uint32_t state = seed ^ (0x9E3779B9u * (sequence + 1u));
  return state == 0u ? 0xA341316Cu : state;
}

static inline uint32_t xorshift32(uint32_t& state) {
  uint32_t x = state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

static inline uint32_t lfsr32(uint32_t& state) {
  const uint32_t lsb = state & 1u;
  state >>= 1;
  if (lsb != 0u) {
    state ^= 0x80200003u;
  }
  if (state == 0u) {
    state = 0x1u;
  }
  return state;
}

static inline void emitWordLE(uint8_t* dst, size_t remaining, uint32_t word) {
  const size_t count = remaining < 4 ? remaining : 4;
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<uint8_t>(word >> (i * 8));
  }
}

static inline void fillRamp8(uint8_t* dst, size_t length, uint32_t sequence) {
  for (size_t i = 0; i < length; ++i) {
    dst[i] = static_cast<uint8_t>((sequence + i) & 0xFFu);
  }
}

static inline void fillXorshift32(uint8_t* dst, size_t length, uint32_t seed,
                                  uint32_t sequence) {
  uint32_t state = frameSeed(seed, sequence);
  for (size_t offset = 0; offset < length; offset += 4) {
    const uint32_t word = xorshift32(state);
    emitWordLE(dst + offset, length - offset, word);
  }
}

static inline void fillLfsr32(uint8_t* dst, size_t length, uint32_t seed,
                              uint32_t sequence) {
  uint32_t state = frameSeed(seed, sequence);
  for (size_t offset = 0; offset < length; offset += 4) {
    const uint32_t word = lfsr32(state);
    emitWordLE(dst + offset, length - offset, word);
  }
}

static inline void fill(uint8_t* dst, size_t length,
                        NativeUsbProtocol::PayloadGenerator generator,
                        uint32_t seed, uint32_t sequence) {
  switch (generator) {
    case NativeUsbProtocol::PayloadGenerator::Xorshift32:
      fillXorshift32(dst, length, seed, sequence);
      break;
    case NativeUsbProtocol::PayloadGenerator::Lfsr32:
      fillLfsr32(dst, length, seed, sequence);
      break;
    case NativeUsbProtocol::PayloadGenerator::Ramp8:
    default:
      fillRamp8(dst, length, sequence);
      break;
  }
}

}  // namespace PayloadGenerators
