# NativeUSB Bench — Firmware

Each target implements **NativeUSB Protocol v1.0** so the desktop app can control the benchmark and verify incoming data independently of the MCU or USB framework. Target-specific flashing instructions and limits live in each MCU folder.

## Targets

| MCU | Firmware | Status |
| --- | --- | --- |
| ESP32-S3 | [Arduino-ESP32 / TinyUSB](esp32s3_arduino_tinyusb_bench/) | Supported (Bulk IN) |
| ESP32-S3 | [Arduino-ESP32 / ROM USB](esp32s3_arduino_romusb_bench/) | Experimental (Bulk IN) |
| ESP32-P4 | — | WIP |
| STM32 | — | WIP |

Both ESP32-S3 implementations use native USB Full-Speed and the same wire protocol. The WIP targets are not ready for benchmarking.

## How it works

1. The host discovers the vendor-specific USB interface and its endpoints from the device descriptors.
2. **EP0** carries low-rate vendor control requests (device info, configuration, status and start/stop commands).
3. On **Bulk IN**, the MCU streams `NUB1` frames: a **16-byte header** followed by deterministic test data. The host checks frame integrity and measures throughput.
4. **Bulk OUT** is reserved for future OUT/duplex testing; it is not used for commands or data streaming by the current ESP32-S3 target.

Protocol v1.0 EP0 requests:

| Request | Purpose |
| --- | --- |
| `0x30` COMMAND | PING, START, STOP, RESET_COUNTERS |
| `0x31` GET_STATUS | Streaming state and counters |
| `0x32` GET_DEVICE_INFO | MCU, framework and firmware information |
| `0x33` GET_CAPABILITIES | Supported modes and limits |
| `0x34` GET_CONFIG | Read benchmark configuration |
| `0x35` SET_CONFIG | Change benchmark configuration |

Available deterministic generators are **Xorshift32**, **Ramp8** and **LFSR32** (subject to target capabilities). Frame size, endpoint addresses and USB speed are target-specific; the application reports measured transfer throughput, not the USB signaling rate.

