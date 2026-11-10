#pragma once
#include <stdint.h>
#include "../core/config.h"

namespace RomUsbDescriptors {
// ROM USB core expects a descriptor stream terminated by a zero-length entry.
// Device (18), configuration (9), interface (9), Bulk OUT (7), Bulk IN (7).
constexpr uint8_t kDescriptors[] = {
    18, 1, 0x00, 0x02, 0, 0, 0, 64,
    uint8_t(BenchConfig::kVendorId), uint8_t(BenchConfig::kVendorId >> 8),
    uint8_t(BenchConfig::kProductId), uint8_t(BenchConfig::kProductId >> 8),
    0x01, 0x05, 1, 2, 3, 1,
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xFF, 0, 0, 4,
    7, 5, 0x01, 2, 64, 0, 0,
    7, 5, 0x81, 2, 64, 0, 0,
    0, 0,
};
constexpr size_t kInterfaceOffset = 18 + 9;
static_assert(sizeof(kDescriptors) == 52, "Descriptor length changed");
// Microsoft OS 1.0 string descriptor for WCID, index 0xEE.
constexpr uint8_t kMsString[] = {
    18, 3, 'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0,0x20,0
};
// Compatible ID: WINUSB on interface zero.
constexpr uint8_t kMsCompatId[] = {
    0x28,0,0,0, 0,1, 4,0, 1,0,0,0,0,0,0,0,
    0,1, 'W','I','N','U','S','B',0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};
static_assert(sizeof(kMsCompatId) == 40, "Invalid WINUSB compatible ID");
// Same interface GUID as the TinyUSB target so WinUSB can bind the application.
constexpr uint8_t kMsProperties[] = {
    0x8E,0,0,0, 0,1,5,0, 1,0,
    0x84,0,0,0, 1,0,0,0, 0x28,0,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,
    'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,'a',0,'c',0,'e',0,
    'G',0,'U',0,'I',0,'D',0,0,0,
    0x4E,0,0,0,
    '{',0,'D',0,'3',0,'4',0,'C',0,'9',0,'B',0,'2',0,'6',0,'-',0,
    '6',0,'F',0,'7',0,'B',0,'-',0,'4',0,'C',0,'2',0,'B',0,'-',0,
    '9',0,'E',0,'2',0,'3',0,'-',0,'8',0,'D',0,'4',0,'6',0,'A',0,
    '5',0,'B',0,'5',0,'1',0,'C',0,'7',0,'2',0,'}',0,0,0,
};
static_assert(sizeof(kMsProperties) == 142, "Invalid WINUSB GUID descriptor");
}  // namespace RomUsbDescriptors
