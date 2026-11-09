// Public boundary for the NativeUSB wire protocol and Bulk frame parser.
mod frames;
mod payload;
mod types;
mod wire;

pub(crate) use frames::FrameParser;
pub(crate) use types::{BenchmarkConfig, DeviceCapabilities, DeviceInfo, FirmwareStatus, PayloadVisual};
pub(crate) use wire::{encode_config, parse_capabilities, parse_config, parse_device_info, parse_status};

pub(crate) const CMD_PING: u8 = 0x01;
pub(crate) const CMD_START: u8 = 0x02;
pub(crate) const CMD_STOP: u8 = 0x03;
pub(crate) const CMD_RESET: u8 = 0x04;

pub(crate) const HEADER_SIZE: usize = 16;
pub(crate) const MAX_FRAME_SIZE: usize = 16 * 1024;
pub(crate) const PREVIEW_BYTES: usize = 512;
pub(crate) const MAGIC: [u8; 4] = *b"NUB1";
pub(crate) const PROTOCOL_MAJOR: u8 = 1;
pub(crate) const TYPE_DATA: u8 = 0x90;

pub(crate) const GENERATOR_RAMP8: u8 = 0;
pub(crate) const GENERATOR_XORSHIFT32: u8 = 1;
pub(crate) const GENERATOR_LFSR32: u8 = 2;

