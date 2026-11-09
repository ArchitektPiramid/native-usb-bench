use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize)]
pub(crate) struct DeviceInfo {
    pub protocol_version: String,
    pub firmware_version: String,
    pub cpu_clock_hz: u32,
    pub target_id: u16,
    pub mcu_id: u16,
    pub target_name: String,
    pub mcu_name: String,
    pub board_name: String,
    pub framework_name: String,
    pub usb_stack_name: String,
}

#[derive(Clone, Debug, Serialize)]
pub(crate) struct DeviceCapabilities {
    pub benchmark_modes: u32,
    pub generator_mask: u32,
    pub min_frame_size: u32,
    pub max_frame_size: u32,
    pub default_frame_size: u32,
    pub deterministic_payload: bool,
    pub configurable_generator: bool,
    pub configurable_frame_size: bool,
}

impl DeviceCapabilities {
    pub(crate) fn supports_generator(&self, generator: u8) -> bool {
        generator < 32 && (self.generator_mask & (1u32 << generator)) != 0
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub(crate) struct BenchmarkConfig {
    pub generator: u8,
    pub mode: u8,
    pub frame_size: u32,
    pub seed: u32,
    pub deterministic_payload: bool,
}

#[derive(Clone, Debug, Serialize)]
pub(crate) struct FirmwareStatus {
    pub usb_state: u8,
    pub configured: bool,
    pub streaming: bool,
    pub tx_busy: bool,
    pub alt_setting: u8,
    pub frames_generated: u32,
    pub frames_sent: u32,
    pub frames_dropped: u32,
    pub bytes_sent: u64,
    pub bulk_in_transfers: u32,
    pub bulk_out_transfers: u32,
    pub control_commands: u32,
    pub uptime_ms: u32,
}

#[derive(Clone, Debug, Serialize)]
pub(crate) struct PayloadVisual {
    pub sequence: u32,
    pub generator: u8,
    pub sample: Vec<u8>,
    pub histogram: Vec<u32>,
    pub bit_ones_percent: Vec<f64>,
}

