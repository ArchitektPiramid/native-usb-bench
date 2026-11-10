#pragma once

#include <Arduino.h>

namespace PayloadGenerators {

// Derive each frame's state independently so dropped frames remain verifiable.
static inline uint32_t frameSeed(uint32_t seed, uint32_t sequence) {
  uint32_t state = seed ^ (0x9E3779B9u * (sequence + 1u));
  return state == 0u ? 0xA341316Cu : state;
}

static inline void emitWordLE(uint8_t* dst, size_t remaining, uint32_t word) {
  const size_t count = remaining < 4 ? remaining : 4;
  for (size_t i = 0; i < count; ++i) {
    dst[i] = static_cast<uint8_t>(word >> (i * 8));
  }
}

}  // namespace PayloadGenerators
