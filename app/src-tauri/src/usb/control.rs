use crate::protocol::{
    encode_config, parse_capabilities, parse_config, parse_device_info, parse_status,
    BenchmarkConfig, DeviceCapabilities, DeviceInfo, FirmwareStatus, HEADER_SIZE, MAX_FRAME_SIZE,
};
use rusb::{DeviceHandle, GlobalContext};
use std::time::Duration;

const CTRL_OUT_VENDOR_DEVICE: u8 = 0x40;
const CTRL_IN_VENDOR_DEVICE: u8 = 0xC0;
const REQ_COMMAND: u8 = 0x30;
const REQ_STATUS: u8 = 0x31;
const REQ_DEVICE_INFO: u8 = 0x32;
const REQ_CAPABILITIES: u8 = 0x33;
const REQ_CONFIG: u8 = 0x34;
const REQ_SET_CONFIG: u8 = 0x35;
const CONTROL_TIMEOUT_MS: u64 = 500;

pub(crate) fn send_usb_command(
    handle: &DeviceHandle<GlobalContext>,
    command: u8,
) -> Result<(), String> {
    let count = handle
        .write_control(
            CTRL_OUT_VENDOR_DEVICE,
            REQ_COMMAND,
            command as u16,
            0,
            &[],
            Duration::from_millis(CONTROL_TIMEOUT_MS),
        )
        .map_err(|error| {
            format!(
                "EP0 command 0x{command:02X} failed (bmRequestType=0x40, bRequest=0x30): {error}"
            )
        })?;

    if count != 0 {
        return Err(format!("Unexpected EP0 command data count: {count}"));
    }
    Ok(())
}

pub(crate) fn set_device_config(
    handle: &DeviceHandle<GlobalContext>,
    capabilities: &DeviceCapabilities,
    max_packet_size: u16,
    requested: &BenchmarkConfig,
) -> Result<BenchmarkConfig, String> {
    validate_config(capabilities, max_packet_size, requested)?;
    let data = encode_config(requested);
    let count = handle
        .write_control(
            CTRL_OUT_VENDOR_DEVICE,
            REQ_SET_CONFIG,
            0,
            0,
            &data,
            Duration::from_millis(CONTROL_TIMEOUT_MS),
        )
        .map_err(|error| format!("EP0 SET_CONFIG failed: {error}"))?;

    if count != data.len() {
        return Err(format!(
            "EP0 SET_CONFIG wrote {count} bytes, expected {}",
            data.len()
        ));
    }

    let applied = read_config(handle)?;
    if applied.generator != requested.generator
        || applied.frame_size != requested.frame_size
        || applied.seed != requested.seed
        || applied.mode != requested.mode
        || applied.deterministic_payload != requested.deterministic_payload
    {
        return Err(format!(
            "Firmware did not apply requested config. Requested generator={} frame={} seed=0x{:08X}; applied generator={} frame={} seed=0x{:08X}",
            requested.generator,
            requested.frame_size,
            requested.seed,
            applied.generator,
            applied.frame_size,
            applied.seed
        ));
    }
    Ok(applied)
}

pub(super) fn validate_config(
    capabilities: &DeviceCapabilities,
    max_packet_size: u16,
    config: &BenchmarkConfig,
) -> Result<(), String> {
    if !capabilities.supports_generator(config.generator) {
        return Err(format!(
            "Generator {} is not supported by this target",
            config.generator
        ));
    }
    if config.mode != 1 {
        return Err("This firmware currently supports MCU -> PC (IN) benchmark mode only".into());
    }
    if config.frame_size < capabilities.min_frame_size
        || config.frame_size > capabilities.max_frame_size
        || config.frame_size < HEADER_SIZE as u32
        || config.frame_size > MAX_FRAME_SIZE as u32
    {
        return Err(format!(
            "Frame size {} is outside device range {}..{}",
            config.frame_size, capabilities.min_frame_size, capabilities.max_frame_size
        ));
    }
    if max_packet_size == 0 || config.frame_size % max_packet_size as u32 != 0 {
        return Err(format!(
            "Frame size must be a multiple of the target Bulk MPS ({} bytes)",
            max_packet_size
        ));
    }
    Ok(())
}

pub(super) fn read_device_info(handle: &DeviceHandle<GlobalContext>) -> Result<DeviceInfo, String> {
    // Ask for the current extended DeviceInfo size, but accept the original
    // 96-byte NDI1 response from older firmware.
    const DEVICE_INFO_CAPACITY: usize = 144;
    const DEVICE_INFO_MIN_SIZE: usize = 96;
    let mut data = vec![0u8; DEVICE_INFO_CAPACITY];
    let count = handle
        .read_control(
            CTRL_IN_VENDOR_DEVICE,
            REQ_DEVICE_INFO,
            0,
            0,
            &mut data,
            Duration::from_millis(CONTROL_TIMEOUT_MS),
        )
        .map_err(|error| format!("EP0 GET_DEVICE_INFO failed (bRequest=0x{REQ_DEVICE_INFO:02X}): {error}"))?;
    data.truncate(count);
    if count < DEVICE_INFO_MIN_SIZE {
        return Err(format!(
            "EP0 GET_DEVICE_INFO returned {count} bytes, expected at least {DEVICE_INFO_MIN_SIZE}"
        ));
    }
    parse_device_info(&data)
}

pub(super) fn read_capabilities(handle: &DeviceHandle<GlobalContext>) -> Result<DeviceCapabilities, String> {
    let data = read_control_blob(handle, REQ_CAPABILITIES, 32, "GET_CAPABILITIES")?;
    parse_capabilities(&data)
}

pub(super) fn read_config(handle: &DeviceHandle<GlobalContext>) -> Result<BenchmarkConfig, String> {
    let data = read_control_blob(handle, REQ_CONFIG, 24, "GET_CONFIG")?;
    parse_config(&data)
}

pub(crate) fn read_firmware_status(handle: &DeviceHandle<GlobalContext>) -> Result<FirmwareStatus, String> {
    let data = read_control_blob(handle, REQ_STATUS, 48, "GET_STATUS")?;
    parse_status(&data)
}

fn read_control_blob(
    handle: &DeviceHandle<GlobalContext>,
    request: u8,
    size: usize,
    label: &str,
) -> Result<Vec<u8>, String> {
    let mut data = vec![0u8; size];
    let count = handle
        .read_control(
            CTRL_IN_VENDOR_DEVICE,
            request,
            0,
            0,
            &mut data,
            Duration::from_millis(CONTROL_TIMEOUT_MS),
        )
        .map_err(|error| format!("EP0 {label} failed (bRequest=0x{request:02X}): {error}"))?;
    data.truncate(count);
    if count < size {
        return Err(format!(
            "EP0 {label} returned {count} bytes, expected at least {size}"
        ));
    }
    Ok(data)
}

