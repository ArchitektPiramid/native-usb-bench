use crate::protocol::{BenchmarkConfig, DeviceCapabilities, DeviceInfo, FirmwareStatus};
use rusb::{DeviceHandle, GlobalContext};
use serde::Serialize;
use tauri::{AppHandle, Emitter};

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
    pub(crate) key: String,
    pub(crate) bus: u8,
    pub(crate) address: u8,
    pub(crate) vid: u16,
    pub(crate) pid: u16,
    pub(crate) manufacturer: Option<String>,
    pub(crate) product: Option<String>,
    pub(crate) serial_number: Option<String>,
    pub(crate) speed: String,
    pub(crate) target: bool,
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

