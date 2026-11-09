# ESP32-S3 — Arduino-ESP32 / TinyUSB

Firmware for the **ESP32-S3 native USB/OTG controller**. For the common USB layout, EP0 requests and `NUB1` frame format, see the [firmware overview](../README.md).

## Flash with Arduino IDE

1. Install **ESP32 by Espressif Systems 3.3.x** in Boards Manager (3.3.11 is the previously tested baseline). TinyUSB is provided by the ESP32 Arduino core; do not add a separate TinyUSB library.
2. Open `esp32s3_arduino_tinyusb_bench.ino` and select **ESP32S3 Dev Module** or your board's matching ESP32-S3 definition.
3. Configure **Tools**:

   | Option | Value |
   | --- | --- |
   | USB Mode | **USB-OTG (TinyUSB)** |
   | USB CDC On Boot | **Disabled** |
   | USB DFU On Boot | **Disabled** (if shown) |
   | USB MSC On Boot | **Disabled** (if shown) |

4. Select the upload port and click **Upload**. Use the board's USB-UART connector if available; otherwise enter ROM download mode on the native USB port (commonly **hold BOOT → tap RESET → release BOOT**). Exact flashing steps depend on the board.
5. Boot normally and connect the **native USB/OTG** port to the PC. The benchmark interface is a WinUSB vendor device, **not** an Arduino COM port. Open NativeUSB Bench and select **Refresh → Connect → Start**.

## Windows device

| Field | Value |
| --- | --- |
| Development VID:PID | `CAFE:4013` |
| USB interface | Vendor-specific (`0xFF`) |
| Bulk IN / OUT | Usually `0x81` / `0x01` (host discovers endpoints) |
| Driver | `winusb.sys` |

The firmware advertises Microsoft OS 1.0/WCID (`MSFT100` / `WINUSB`) for automatic WinUSB binding on supported Windows installations. If connection fails, check the driver on the **vendor interface**; do not replace the ROM bootloader or USB-UART driver. The USB `bcdDevice` value `5.00` is not the firmware release version.

## Target defaults

| Parameter | Value |
| --- | --- |
| USB | Full-Speed, 12 Mbit/s signaling |
| Bulk max packet size | 64 B |
| Test direction | MCU → PC (Bulk IN) |
| NUB1 frame | 4096 B total (16 B header + 4080 B payload) |
| Default payload generator | Xorshift32 (Ramp8 and LFSR32 also available) |

The **4096 B** frame cap is deliberate: earlier S3 tests found direct TinyUSB/DCD transfers around 4 KiB more reliable than larger submissions. The firmware builds one deterministic frame in `loop()`, submits a Bulk IN transfer and waits for the endpoint to become idle before sending the next frame. It does not use a separate producer task.

`GET_DEVICE_INFO` reports the Arduino-ESP32 version used to build the sketch. Record actual benchmark results in the main project README rather than treating the 12 Mbit/s USB speed as measured throughput.
