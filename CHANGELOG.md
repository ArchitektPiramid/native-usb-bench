# Changelog

## [Unreleased]

### ESP32-S3 / Arduino TinyUSB

- Split USB descriptors, EP0 control and Bulk transfers; organize firmware modules.
- Extract and optimize Ramp8, Xorshift32 and LFSR32 generators.
- Pipeline Bulk IN with double buffering and non-blocking transfers.
- Add experimental 8/16 KiB NUB1 frames using 4 KiB DCD chunks.

### ESP32-S3 / Arduino ROM USB

- Add ROM USB firmware with WinUSB, quiet UART and configurable Bulk IN chunks.
- Apply EP0 SET_CONFIG updates for generator and 4/8/16 KiB frame sizes.

### Host / CI

- Split USB discovery, EP0 control, wire protocol and frame parsing.
- Add host frame-size selection and stop on fatal Bulk IN errors.
- Run Windows host checks for PRs to `features` or manually.

## [0.1.0] - 2026-11-09

- Initial Windows benchmark application and ESP32-S3 firmware.
