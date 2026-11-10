#pragma once

#include <Arduino.h>
#include <cstring>

namespace PayloadGenerators {

// Derive each frame's state independently so dropped frames remain verifiable.
static inline uint32_t frameSeed(uint32_t seed, uint32_t sequence) {
  uint32_t state = seed ^ (0x9E3779B9u * (sequence + 1u));
  return state == 0u ? 0xA341316Cu : state;
}

static inline void emitWordLE(uint8_t* dst, size_t remaining, uint32_t word) {
  if (remaining >= 4) {
    // ESP32-S3 is little-endian; full words need only one copy.
    std::memcpy(dst, &word, sizeof(word));
    return;
  }
  for (size_t i = 0; i < remaining; ++i) {
    dst[i] = static_cast<uint8_t>(word >> (i * 8));
  }
}

}  // namespace PayloadGenerators
