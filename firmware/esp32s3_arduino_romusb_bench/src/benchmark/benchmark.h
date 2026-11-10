#pragma once
#include <stdint.h>
#include "../usb/rom_usb.h"

namespace Bench {
struct Options {
  RomUsb::BulkWriteSize bulk_write_size = RomUsb::BulkWriteSize::Bytes64;
  uint16_t loop_yield_every = 64;
};
void begin(Options options = {});
void tick();
}  // namespace Bench
