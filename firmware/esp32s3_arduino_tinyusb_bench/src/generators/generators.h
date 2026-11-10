#pragma once

#include "../../nativeusb_protocol.h"
#include "ramp8.h"
#include "xorshift32.h"
#include "lfsr32.h"

namespace PayloadGenerators {

// The wire format uses the same three generators on MCU and host.
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
