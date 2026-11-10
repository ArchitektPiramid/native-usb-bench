# Changelog

## [Unreleased]

- Organize ESP32-S3 firmware into benchmark, USB, protocol and core modules.

- Speed up full-word ESP32-S3 generator writes.

- Move ESP32-S3 payload generators into separate firmware modules.

- Pipeline ESP32-S3 Bulk IN using two buffers and non-blocking transfers.

- Split ESP32-S3 descriptors, EP0 handling and Bulk transfers.
- Split host USB discovery, EP0 control, wire protocol and frame parsing.
- Run Windows host checks only for PRs to `features` or manual requests.

## [0.1.0] - 2026-11-09

- Initial Windows benchmark application and ESP32-S3 firmware.
