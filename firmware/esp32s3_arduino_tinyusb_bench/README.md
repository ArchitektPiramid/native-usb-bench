# ESP32-S3 / Arduino-ESP32 / TinyUSB target

NativeUSB Bench target for the ESP32-S3 native USB controller using Arduino-ESP32 + TinyUSB.

## USB layout

```text
Interface 0 - Vendor Specific (0xFF)
  EP0       - NativeUSB control plane
  Bulk OUT  - discovered from descriptor (normally 0x01), reserved for OUT/duplex benchmark
  Bulk IN   - discovered from descriptor (normally 0x81), MCU -> PC benchmark stream
```

The firmware uses NativeUSB Protocol 1.0, the same EP0 requests and the same `NUB1` Bulk frame format as the STM32 target.

## Tested framework target

The firmware is written against Arduino-ESP32 3.3.x and the previous stable ESP32-S3 transport was tested with 3.3.11. `GET_DEVICE_INFO` reports the Arduino-ESP32 version actually used to compile the sketch.

## Arduino IDE settings

Typical settings for `ESP32S3 Dev Module`:

```text
USB Mode            USB-OTG (TinyUSB)
USB CDC On Boot     Disabled
USB DFU On Boot     Disabled (if shown)
USB MSC On Boot     Disabled (if shown)
```

Only the native USB/OTG connector carries the NativeUSB Vendor interface. A separate UART/USB bridge can still be used for flashing/debug output when the board provides one.

## Windows / WinUSB

Development VID/PID:

```text
VID 0xCAFE
PID 0x4013
bcdDevice 5.00
```

The sketch provides Microsoft OS 1.0/WCID descriptors itself:

```text
MSFT100 -> Compatible ID WINUSB
```

so Windows can bind the inbox `winusb.sys` driver without Zadig or a custom INF.

## Benchmark defaults

```text
USB speed           Full-Speed 12 Mbit/s
Bulk MPS            64 B
Frame size          4096 B
Header              16 B NUB1
Payload             4080 B
Default generator   Xorshift32
Other generators    Ramp8, LFSR32
Direction           MCU -> PC (Bulk IN)
```

The maximum logical frame is intentionally kept at 4096 B on this first S3 target. Earlier ESP32-S3 testing showed that approximately 4 KiB direct DCD submissions are a stable high-throughput path, while larger low-level submissions can become less reliable.

## Data path

```text
loop()
  -> build one deterministic NUB1 frame
  -> submit one direct TinyUSB/DCD Bulk IN transfer
  -> wait until endpoint is idle
  -> next frame
```

There is no matrix/sensor model and no second producer task. Payload generation is deliberately cheap so the benchmark focuses on USB transport.

## Control plane

All low-rate control stays on EP0:

```text
0x30 COMMAND
0x31 GET_STATUS
0x32 GET_DEVICE_INFO
0x33 GET_CAPABILITIES
0x34 GET_CONFIG
0x35 SET_CONFIG
```

Bulk OUT is not used for commands.
