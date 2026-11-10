#pragma once
#include "config.h"

// All profiling code, data and serial output are excluded when disabled.
#if NUB_ROMUSB_PROFILE
#include <stdint.h>
namespace RomUsbProfile {
uint32_t cycles();
void recordFrame(uint32_t elapsed);
void recordWrite(uint32_t elapsed, uint32_t requested, uint32_t accepted, int result);
void callbackFromIsr();
void callbackServiced();
void report();
}  // namespace RomUsbProfile
#endif
