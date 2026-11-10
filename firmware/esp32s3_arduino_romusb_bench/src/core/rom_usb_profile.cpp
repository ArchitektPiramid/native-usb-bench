#include "rom_usb_profile.h"
#if NUB_ROMUSB_PROFILE
#include <Arduino.h>
#include "esp_cpu.h"
#include <cerrno>

namespace RomUsbProfile {
namespace {
struct Metric {
  uint64_t total = 0;
  uint32_t maximum = 0;
  uint32_t count = 0;
  void add(uint32_t value) {
    total += value;
    if (value > maximum) maximum = value;
    ++count;
  }
  uint32_t average() const { return count ? static_cast<uint32_t>(total / count) : 0; }
};
Metric frames, writes, callback_to_task;
uint32_t partial_writes = 0;
uint32_t busy_writes = 0;
uint32_t failed_writes = 0;
volatile uint32_t callback_cycle = 0;
uint32_t last_report_ms = 0;
uint16_t report_polls = 0;
}

uint32_t cycles() { return esp_cpu_get_cycle_count(); }
void recordFrame(uint32_t elapsed) { frames.add(elapsed); }
void recordWrite(uint32_t elapsed, uint32_t requested, uint32_t accepted, int rc) {
  writes.add(elapsed);
  if (rc == 0 && accepted > 0 && accepted < requested) ++partial_writes;
  if ((rc == 0 && accepted == 0) || rc == -EAGAIN || rc == -EBUSY) ++busy_writes;
  else if (rc != 0) ++failed_writes;
}
void callbackFromIsr() { callback_cycle = cycles(); }
void callbackServiced() {
  // The completion timestamp is captured in the ISR on the same Arduino core.
  callback_to_task.add(cycles() - callback_cycle);
}
void report() {
  // Do not query the system clock on each high-frequency benchmark loop.
  if ((++report_polls & 1023u) != 0u) return;
  const uint32_t now = millis();
  if (now - last_report_ms < NUB_ROMUSB_PROFILE_INTERVAL_MS) return;
  const uint32_t interval = now - last_report_ms;
  last_report_ms = now;
  Serial.printf(
      "[ROMUSB][PERF] ms=%lu frames=%lu gen_cyc(avg/max)=%lu/%lu "
      "writes=%lu dcd_cyc(avg/max)=%lu/%lu cb2task_cyc(avg/max)=%lu/%lu "
      "partial=%lu busy=%lu errors=%lu MHz=%lu\n",
      static_cast<unsigned long>(interval),
      static_cast<unsigned long>(frames.count),
      static_cast<unsigned long>(frames.average()),
      static_cast<unsigned long>(frames.maximum),
      static_cast<unsigned long>(writes.count),
      static_cast<unsigned long>(writes.average()),
      static_cast<unsigned long>(writes.maximum),
      static_cast<unsigned long>(callback_to_task.average()),
      static_cast<unsigned long>(callback_to_task.maximum),
      static_cast<unsigned long>(partial_writes),
      static_cast<unsigned long>(busy_writes),
      static_cast<unsigned long>(failed_writes),
      static_cast<unsigned long>(getCpuFrequencyMhz()));
  frames = {};
  writes = {};
  callback_to_task = {};
  partial_writes = busy_writes = failed_writes = 0;
}
}  // namespace RomUsbProfile
#endif  // NUB_ROMUSB_PROFILE
