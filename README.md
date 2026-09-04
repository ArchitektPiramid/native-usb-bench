# MCU NativeUSB Bench

Cross-platform USB throughput benchmarking and device inspection tool for microcontrollers.

The project provides a desktop application together with matching MCU firmware for testing native USB implementations, streaming data and inspecting USB devices.

The goal is to provide a common test environment for different microcontrollers and USB frameworks.

## Features

- USB device enumeration & descriptor inspection
- interface and endpoint viewer
- real-time USB data streaming
- throughput measurement & transfer rate statistics
- bit balance / data integrity statistics
- configurable test data generation
- support for multiple MCU platforms
- matching firmware examples for each supported MCU

## Supported platforms

Current and planned MCU support includes:

| MCU      | Framework           | USB                | Status    |
| -------- | ------------------- | ------------------ | --------- |
| ESP32-S3 | Arduino / ESP-IDF   | USB 2.0 Full-Speed | Supported |
| STM32    | Arduino / STM32Cube | USB FS / HS        | Supported |

Support for additional MCUs and frameworks is planned

## Application

The desktop application is built with:

- Tauri
- Rust
- React
- TypeScript
- Vite
