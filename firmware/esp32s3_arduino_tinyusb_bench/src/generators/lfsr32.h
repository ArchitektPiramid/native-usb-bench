#pragma once

#include "common.h"

namespace PayloadGenerators {

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

static inline void fillLfsr32(uint8_t* dst, size_t length, uint32_t seed,
                              uint32_t sequence) {
  uint32_t state = frameSeed(seed, sequence);
  for (size_t offset = 0; offset < length; offset += 4) {
    const uint32_t word = lfsr32(state);
    emitWordLE(dst + offset, length - offset, word);
  }
}

}  // namespace PayloadGenerators
