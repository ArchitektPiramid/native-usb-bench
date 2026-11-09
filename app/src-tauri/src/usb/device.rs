use super::control::{
    read_capabilities, read_config, read_device_info, read_firmware_status, send_usb_command,
    validate_config,
};
use super::types::{
    usb_log, ConnectedDeviceInfo, OpenedDevice, UsbDeviceInfo, UsbTransportInfo,
};
use crate::protocol::{CMD_PING, PROTOCOL_MAJOR};
use rusb::{Device, Direction, GlobalContext, Speed, TransferType};
use tauri::AppHandle;

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

    if !device_info.protocol_version.starts_with(&format!("{PROTOCOL_MAJOR}.")) {
        return Err(format!("Unsupported NativeUSB protocol {}", device_info.protocol_version));
    }
    validate_config(&capabilities, bulk.max_packet_size, &config)?;

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
