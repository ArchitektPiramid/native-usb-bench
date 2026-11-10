#pragma once

#include "common.h"

namespace PayloadGenerators {

static inline void fillRamp8(uint8_t* dst, size_t length, uint32_t sequence) {
  for (size_t i = 0; i < length; ++i) {
    dst[i] = static_cast<uint8_t>((sequence + i) & 0xFFu);
  }
}

}  // namespace PayloadGenerators
