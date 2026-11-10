#pragma once

#include "common.h"

namespace PayloadGenerators {

static inline uint32_t xorshift32(uint32_t& state) {
  uint32_t x = state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

static inline void fillXorshift32(uint8_t* dst, size_t length, uint32_t seed,
                                  uint32_t sequence) {
  uint32_t state = frameSeed(seed, sequence);
  for (size_t offset = 0; offset < length; offset += 4) {
    const uint32_t word = xorshift32(state);
    emitWordLE(dst + offset, length - offset, word);
  }
}

}  // namespace PayloadGenerators
