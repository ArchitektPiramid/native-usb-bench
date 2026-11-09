# MCU NativeUSB Bench

USB throughput benchmarking and device inspection tool for microcontrollers.

The project provides a Windows desktop application and matching MCU firmware for testing native USB implementations, streaming data, and measuring transfer performance.

The goal is to provide a common benchmarking environment for different microcontrollers and USB frameworks.

## Features

- USB device enumeration and descriptor inspection
- Interface and endpoint viewer
- Real-time USB Bulk streaming
- Throughput monitoring (MB/s, Mbit/s)
- Bit balance and data integrity statistics
- Configurable test data generation
- Firmware examples for supported MCU platforms


![NativeUSB Bench](docs/images/nativeusb-bench-overview.png)

## Supported Platforms

| MCU | Framework | USB | Status | Speed |
|---|---|---|---|---|
| ESP32-S3 | Arduino / TinyUSB | Full-Speed | Supported | 0.75 MB/s |
| ESP32-P4 | Arduino / TinyUSB | High-Speed  | WIP | x |
| STM32 | STM32Cube / Arduino | FS / HS | WIP | x |


## Getting Started

**Run from source**

```bash
cd app
pnpm install
pnpm tauri dev
```

Flash the matching firmware from the `firmware/` directory, connect the MCU via native USB, and select the device in the application.

## Tech Stack

- **Desktop:** Tauri 2, Rust
- **Frontend:** React, TypeScript, Vite
- **USB Backend:** Rust, WinUSB
- **Firmware:** Arduino / TinyUSB

## License

MIT License.