use crate::protocol::{
    encode_config, parse_capabilities, parse_config, parse_device_info, parse_status, BenchmarkConfig,
    DeviceCapabilities, DeviceInfo, FirmwareStatus, CMD_PING,
};
use rusb::{Device, DeviceHandle, Direction, GlobalContext, Speed, TransferType};
use serde::Serialize;
use std::time::Duration;
use tauri::{AppHandle, Emitter};

const CTRL_OUT_VENDOR_DEVICE: u8 = 0x40;
const CTRL_IN_VENDOR_DEVICE: u8 = 0xC0;
const REQ_COMMAND: u8 = 0x30;
const REQ_STATUS: u8 = 0x31;
const REQ_DEVICE_INFO: u8 = 0x32;
const REQ_CAPABILITIES: u8 = 0x33;
const REQ_CONFIG: u8 = 0x34;
const REQ_SET_CONFIG: u8 = 0x35;
const CONTROL_TIMEOUT_MS: u64 = 500;

#[derive(Clone, Copy)]
struct SupportedTarget {
    vid: u16,
    pid: u16,
    label: &'static str,
}

// Discovery intentionally uses a small explicit target table. Transport details
// (configuration/interface/endpoints/MPS) are discovered from USB descriptors,
// while MCU/framework identity comes from NativeUSB GET_DEVICE_INFO.
const SUPPORTED_TARGETS: &[SupportedTarget] = &[
    SupportedTarget {
        vid: 0xCAFE,
        pid: 0x4012,
        label: "NativeUSB STM32 target",
    },
    SupportedTarget {
        vid: 0xCAFE,
        pid: 0x4013,
        label: "NativeUSB ESP32-S3 target",
    },
];

#[derive(Clone, Debug, Serialize)]
struct LogEvent {
    level: String,
    message: String,
}

pub(crate) fn usb_log(app: &AppHandle, level: &str, message: impl Into<String>) {
    let message = message.into();
    eprintln!("[NativeUSB][{}] {}", level.to_ascii_uppercase(), message);
    let _ = app.emit(
        "usb-log",
        LogEvent {
            level: level.to_string(),
            message,
        },
    );
}

#[derive(Debug, Clone, Serialize)]
pub(crate) struct UsbDeviceInfo {
    key: String,
    bus: u8,
    address: u8,
    vid: u16,
    pid: u16,
    manufacturer: Option<String>,
    product: Option<String>,
    serial_number: Option<String>,
    speed: String,
    target: bool,
}

#[derive(Debug, Clone, Serialize)]
pub(crate) struct UsbTransportInfo {
    pub(crate) vid: u16,
    pub(crate) pid: u16,
    pub(crate) speed: String,
    pub(crate) usb_version: String,
    pub(crate) configuration: u8,
    pub(crate) interface: u8,
    pub(crate) alternate_setting: u8,
    pub(crate) ep_in: u8,
    pub(crate) ep_out: u8,
    pub(crate) max_packet_size: u16,
}

#[derive(Debug, Clone, Serialize)]
pub(crate) struct ConnectedDeviceInfo {
    pub(crate) usb: UsbTransportInfo,
    pub(crate) device_info: DeviceInfo,
    pub(crate) capabilities: DeviceCapabilities,
    pub(crate) config: BenchmarkConfig,
    pub(crate) status: FirmwareStatus,
}

pub(crate) struct OpenedDevice {
    pub(crate) handle: DeviceHandle<GlobalContext>,
    pub(crate) details: ConnectedDeviceInfo,
}

#[derive(Clone, Copy, Debug)]
struct BulkTransport {
    configuration: u8,
    interface: u8,
    alternate_setting: u8,
    ep_in: u8,
    ep_out: u8,
    max_packet_size: u16,
}

pub(crate) fn list_usb_devices(app: &AppHandle) -> Result<Vec<UsbDeviceInfo>, String> {
    // Refresh stays cheap and responsive: only enumerate + read the standard
    // device descriptor. No device open and no EP0 metadata handshake here.
    let devices = rusb::devices().map_err(|error| format!("libusb enumerate failed: {error}"))?;
    let mut out = Vec::new();

    for device in devices.iter() {
        let descriptor = match device.device_descriptor() {
            Ok(descriptor) => descriptor,
            Err(_) => continue,
        };

        let Some(target) = supported_target(descriptor.vendor_id(), descriptor.product_id()) else {
            continue;
        };

        let info = UsbDeviceInfo {
            key: device_key(&device),
            bus: device.bus_number(),
            address: device.address(),
            vid: descriptor.vendor_id(),
            pid: descriptor.product_id(),
            manufacturer: None,
            product: Some(target.label.to_string()),
            serial_number: None,
            speed: speed_name(device.speed()).to_string(),
            target: true,
        };

        usb_log(
            app,
            "info",
            format!(
                "target found: bus {} addr {} {:04X}:{:04X} {} ({})",
                info.bus, info.address, info.vid, info.pid, info.speed, target.label
            ),
        );
        out.push(info);
    }

    if out.is_empty() {
        usb_log(app, "warn", "no supported NativeUSB target found");
    }

    out.sort_by_key(|device| (device.bus, device.address));
    Ok(out)
}

pub(crate) fn find_device(key: &str) -> Result<Device<GlobalContext>, String> {
    let devices = rusb::devices().map_err(|error| format!("libusb enumerate failed: {error}"))?;
    for device in devices.iter() {
        if device_key(&device) == key {
            return Ok(device);
        }
    }
    Err("USB device disappeared. Press Refresh and reconnect.".into())
}

pub(crate) fn open_and_claim(
    device: &Device<GlobalContext>,
    app: &AppHandle,
) -> Result<OpenedDevice, String> {
    let descriptor = device
        .device_descriptor()
        .map_err(|error| format!("Cannot read device descriptor: {error}"))?;

    let target = supported_target(descriptor.vendor_id(), descriptor.product_id()).ok_or_else(|| {
        format!(
            "Selected device {:04X}:{:04X} is not a registered NativeUSB target.",
            descriptor.vendor_id(),
            descriptor.product_id()
        )
    })?;

    let bulk = find_vendor_bulk(device, descriptor.num_configurations())?.ok_or_else(|| {
        format!(
            "{} has no Vendor Specific interface with one Bulk IN and one Bulk OUT endpoint.",
            target.label
        )
    })?;

    usb_log(
        app,
        "info",
        format!(
            "opening {:04X}:{:04X} ({}) through libusb/WinUSB",
            descriptor.vendor_id(),
            descriptor.product_id(),
            target.label
        ),
    );

    let handle = match device.open() {
        Ok(handle) => {
            usb_log(app, "info", "device open succeeded");
            handle
        }
        Err(error) => {
            usb_log(app, "error", format!("device open failed: {error}"));
            return Err(format!(
                "Cannot open {:04X}:{:04X}: {error}. On Windows this normally means WinUSB is not bound to this device instance.",
                descriptor.vendor_id(), descriptor.product_id()
            ));
        }
    };

    if let Ok(active) = handle.active_configuration() {
        if active != bulk.configuration {
            handle.set_active_configuration(bulk.configuration).map_err(|error| {
                format!(
                    "Cannot select USB configuration {}: {error}",
                    bulk.configuration
                )
            })?;
        }
    }

    usb_log(
        app,
        "info",
        format!(
            "claiming interface {} alt {}",
            bulk.interface, bulk.alternate_setting
        ),
    );
    handle.claim_interface(bulk.interface).map_err(|error| {
        usb_log(
            app,
            "error",
            format!("claim interface {} failed: {error}", bulk.interface),
        );
        format!("Cannot claim interface {}: {error}", bulk.interface)
    })?;

    if bulk.alternate_setting != 0 {
        handle
            .set_alternate_setting(bulk.interface, bulk.alternate_setting)
            .map_err(|error| {
                format!(
                    "Cannot select alternate setting {} on interface {}: {error}",
                    bulk.alternate_setting, bulk.interface
                )
            })?;
    }

    usb_log(
        app,
        "info",
        format!(
            "interface {} claimed; EP0 control, Bulk IN=0x{:02X}, Bulk OUT=0x{:02X}, MPS={} B",
            bulk.interface, bulk.ep_in, bulk.ep_out, bulk.max_packet_size
        ),
    );

    // The same EP0 handshake is required from every MCU/framework target.
    send_usb_command(&handle, CMD_PING)?;
    let device_info = read_device_info(&handle)?;
    let capabilities = read_capabilities(&handle)?;
    let config = read_config(&handle)?;
    let status = read_firmware_status(&handle)?;

    if !status.configured {
        return Err(format!(
            "Firmware reports USB state {} and configured=false after interface claim.",
            status.usb_state
        ));
    }

    usb_log(
        app,
        "info",
        format!(
            "device info: target={} mcu={} board={} cpu={}MHz firmware={} protocol={} framework={} usb_stack={}",
            device_info.target_name,
            device_info.mcu_name,
            if device_info.board_name.is_empty() { "unknown" } else { device_info.board_name.as_str() },
            device_info.cpu_clock_hz / 1_000_000,
            device_info.firmware_version,
            device_info.protocol_version,
            device_info.framework_name,
            if device_info.usb_stack_name.is_empty() { "unknown" } else { device_info.usb_stack_name.as_str() }
        ),
    );
    usb_log(
        app,
        "info",
        format!(
            "config: generator={} frame={}B seed=0x{:08X}; capabilities generators=0x{:08X}",
            config.generator, config.frame_size, config.seed, capabilities.generator_mask
        ),
    );

    Ok(OpenedDevice {
        handle,
        details: ConnectedDeviceInfo {
            usb: UsbTransportInfo {
                vid: descriptor.vendor_id(),
                pid: descriptor.product_id(),
                speed: speed_name(device.speed()).to_string(),
                usb_version: usb_version_name(descriptor.usb_version()),
                configuration: bulk.configuration,
                interface: bulk.interface,
                alternate_setting: bulk.alternate_setting,
                ep_in: bulk.ep_in,
                ep_out: bulk.ep_out,
                max_packet_size: bulk.max_packet_size,
            },
            device_info,
            capabilities,
            config,
            status,
        },
    })
}

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

fn validate_config(
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

fn find_vendor_bulk(
    device: &Device<GlobalContext>,
    num_configurations: u8,
) -> Result<Option<BulkTransport>, String> {
    for config_index in 0..num_configurations {
        let config = match device.config_descriptor(config_index) {
            Ok(config) => config,
            Err(_) => continue,
        };

        for interface in config.interfaces() {
            for descriptor in interface.descriptors() {
                if descriptor.class_code() != 0xFF {
                    continue;
                }

                let mut ep_in = None;
                let mut ep_out = None;
                let mut max_packet_size = 0u16;

                for endpoint in descriptor.endpoint_descriptors() {
                    if endpoint.transfer_type() != TransferType::Bulk {
                        continue;
                    }
                    match endpoint.direction() {
                        Direction::In if ep_in.is_none() => {
                            ep_in = Some(endpoint.address());
                            max_packet_size = max_packet_size.max(endpoint.max_packet_size());
                        }
                        Direction::Out if ep_out.is_none() => {
                            ep_out = Some(endpoint.address());
                            max_packet_size = max_packet_size.max(endpoint.max_packet_size());
                        }
                        _ => {}
                    }
                }

                if let (Some(ep_in), Some(ep_out)) = (ep_in, ep_out) {
                    return Ok(Some(BulkTransport {
                        configuration: config.number(),
                        interface: descriptor.interface_number(),
                        alternate_setting: descriptor.setting_number(),
                        ep_in,
                        ep_out,
                        max_packet_size,
                    }));
                }
            }
        }
    }

    Ok(None)
}

fn read_device_info(handle: &DeviceHandle<GlobalContext>) -> Result<DeviceInfo, String> {
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

fn read_capabilities(handle: &DeviceHandle<GlobalContext>) -> Result<DeviceCapabilities, String> {
    let data = read_control_blob(handle, REQ_CAPABILITIES, 32, "GET_CAPABILITIES")?;
    parse_capabilities(&data)
}

fn read_config(handle: &DeviceHandle<GlobalContext>) -> Result<BenchmarkConfig, String> {
    let data = read_control_blob(handle, REQ_CONFIG, 24, "GET_CONFIG")?;
    parse_config(&data)
}

fn read_firmware_status(handle: &DeviceHandle<GlobalContext>) -> Result<FirmwareStatus, String> {
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

fn supported_target(vid: u16, pid: u16) -> Option<&'static SupportedTarget> {
    SUPPORTED_TARGETS
        .iter()
        .find(|target| target.vid == vid && target.pid == pid)
}

fn usb_version_name(version: rusb::Version) -> String {
    format!("{}.{}{}", version.major(), version.minor(), version.sub_minor())
}

fn speed_name(speed: Speed) -> &'static str {
    match speed {
        Speed::Low => "Low-Speed",
        Speed::Full => "Full-Speed",
        Speed::High => "High-Speed",
        Speed::Super => "SuperSpeed",
        Speed::SuperPlus => "SuperSpeed+",
        _ => "Unknown speed",
    }
}

fn device_key(device: &Device<GlobalContext>) -> String {
    format!("{}:{}", device.bus_number(), device.address())
}
