use serde::{Deserialize, Serialize};

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

#[derive(Clone, Debug)]
struct PayloadPreview {
    sequence: u32,
    generator: u8,
    sample: Vec<u8>,
}

#[derive(Debug)]
pub(crate) struct FrameParser {
    buffer: Vec<u8>,
    expected_sequence: Option<u32>,
    config: BenchmarkConfig,
    pub(crate) frames_total: u64,
    pub(crate) dropped_frames: u64,
    pub(crate) corrupt_frames: u64,
    pub(crate) discarded_bytes: u64,
    pub(crate) last_sequence: Option<u32>,
    latest_preview: Option<PayloadPreview>,
}

impl FrameParser {
    pub(crate) fn new(config: BenchmarkConfig) -> Self {
        Self {
            buffer: Vec::with_capacity(config.frame_size as usize * 3),
            expected_sequence: None,
            config,
            frames_total: 0,
            dropped_frames: 0,
            corrupt_frames: 0,
            discarded_bytes: 0,
            last_sequence: None,
            latest_preview: None,
        }
    }

    pub(crate) fn set_config(&mut self, config: BenchmarkConfig) {
        self.config = config;
        self.reset();
    }

    pub(crate) fn config(&self) -> &BenchmarkConfig {
        &self.config
    }

    pub(crate) fn reset(&mut self) {
        self.buffer.clear();
        self.expected_sequence = None;
        self.frames_total = 0;
        self.dropped_frames = 0;
        self.corrupt_frames = 0;
        self.discarded_bytes = 0;
        self.last_sequence = None;
        self.latest_preview = None;
    }

    /// Adds raw Bulk IN bytes and returns the number of complete application
    /// frames parsed during this call.
    pub(crate) fn push(&mut self, data: &[u8]) -> u64 {
        self.buffer.extend_from_slice(data);
        let mut parsed_now = 0u64;

        loop {
            if self.buffer.len() < HEADER_SIZE {
                return parsed_now;
            }

            if self.buffer[..4] != MAGIC {
                if let Some(pos) = self.buffer.windows(4).position(|window| window == MAGIC) {
                    self.discarded_bytes += pos as u64;
                    self.buffer.drain(..pos);
                } else {
                    let keep = self.buffer.len().min(3);
                    let drop_count = self.buffer.len() - keep;
                    self.discarded_bytes += drop_count as u64;
                    self.buffer.drain(..drop_count);
                    return parsed_now;
                }

                if self.buffer.len() < HEADER_SIZE {
                    return parsed_now;
                }
            }

            let protocol_version = self.buffer[4];
            let frame_type = self.buffer[5];
            let flags = u16::from_le_bytes([self.buffer[6], self.buffer[7]]);
            let generator = (flags & 0x00FF) as u8;
            let sequence = u32::from_le_bytes([
                self.buffer[8],
                self.buffer[9],
                self.buffer[10],
                self.buffer[11],
            ]);
            let payload_len = u32::from_le_bytes([
                self.buffer[12],
                self.buffer[13],
                self.buffer[14],
                self.buffer[15],
            ]) as usize;
            let total = HEADER_SIZE.saturating_add(payload_len);

            if protocol_version != PROTOCOL_MAJOR
                || frame_type != TYPE_DATA
                || payload_len == 0
                || total > MAX_FRAME_SIZE
                || total != self.config.frame_size as usize
                || generator != self.config.generator
            {
                self.corrupt_frames += 1;
                self.discarded_bytes += 1;
                self.buffer.drain(..1);
                continue;
            }

            if self.buffer.len() < total {
                return parsed_now;
            }

            let payload = &self.buffer[HEADER_SIZE..total];
            if self.config.deterministic_payload
                && !validate_payload(generator, self.config.seed, sequence, payload)
            {
                self.corrupt_frames += 1;
            }

            if let Some(expected) = self.expected_sequence {
                if sequence != expected {
                    if sequence.wrapping_sub(expected) < 0x8000_0000 {
                        self.dropped_frames += sequence.wrapping_sub(expected) as u64;
                    } else {
                        self.corrupt_frames += 1;
                    }
                }
            }

            self.latest_preview = Some(PayloadPreview {
                sequence,
                generator,
                sample: sample_payload(payload, PREVIEW_BYTES),
            });
            self.expected_sequence = Some(sequence.wrapping_add(1));
            self.last_sequence = Some(sequence);
            self.frames_total += 1;
            parsed_now += 1;
            self.buffer.drain(..total);
        }
    }

    pub(crate) fn payload_visual(&self) -> Option<PayloadVisual> {
        let preview = self.latest_preview.as_ref()?;
        let mut histogram = vec![0u32; 32];
        let mut bit_ones = [0u32; 8];

        for byte in &preview.sample {
            histogram[(*byte as usize) >> 3] += 1;
            for (bit, count) in bit_ones.iter_mut().enumerate() {
                *count += ((*byte >> bit) & 1) as u32;
            }
        }

        let denominator = preview.sample.len().max(1) as f64;
        let bit_ones_percent = bit_ones
            .iter()
            .map(|count| (*count as f64 / denominator) * 100.0)
            .collect();

        Some(PayloadVisual {
            sequence: preview.sequence,
            generator: preview.generator,
            sample: preview.sample.clone(),
            histogram,
            bit_ones_percent,
        })
    }
}

pub(crate) fn parse_device_info(data: &[u8]) -> Result<DeviceInfo, String> {
    // DeviceInfo v1 originally used 96 bytes. Newer firmware may append
    // optional fields while keeping the same NDI1 prefix and struct-size
    // contract, so older targets remain compatible.
    require_header(data, b"NDI1", 96, "DeviceInfo")?;
    let wire_size = read_u16(data, 4)? as usize;
    let usb_stack_name = if wire_size >= 116 && data.len() >= 116 {
        read_fixed_string(&data[92..116])
    } else {
        String::new()
    };
    let board_name = if wire_size >= 140 && data.len() >= 140 {
        read_fixed_string(&data[116..140])
    } else {
        String::new()
    };

    Ok(DeviceInfo {
        protocol_version: format!("{}.{}", data[6], data[7]),
        firmware_version: format!("{}.{}.{}", data[8], data[9], data[10]),
        cpu_clock_hz: read_u32(data, 12)?,
        target_id: read_u16(data, 16)?,
        mcu_id: read_u16(data, 18)?,
        target_name: read_fixed_string(&data[20..44]),
        mcu_name: read_fixed_string(&data[44..68]),
        board_name,
        framework_name: read_fixed_string(&data[68..92]),
        usb_stack_name,
    })
}

pub(crate) fn parse_capabilities(data: &[u8]) -> Result<DeviceCapabilities, String> {
    require_header(data, b"NCP1", 32, "Capabilities")?;
    let flags = read_u32(data, 28)?;
    Ok(DeviceCapabilities {
        benchmark_modes: read_u32(data, 8)?,
        generator_mask: read_u32(data, 12)?,
        min_frame_size: read_u32(data, 16)?,
        max_frame_size: read_u32(data, 20)?,
        default_frame_size: read_u32(data, 24)?,
        deterministic_payload: flags & 0x01 != 0,
        configurable_generator: flags & 0x02 != 0,
        configurable_frame_size: flags & 0x04 != 0,
    })
}

pub(crate) fn parse_config(data: &[u8]) -> Result<BenchmarkConfig, String> {
    require_header(data, b"NCF1", 24, "Config")?;
    let flags = read_u32(data, 20)?;
    Ok(BenchmarkConfig {
        generator: data[7],
        mode: data[8],
        frame_size: read_u32(data, 12)?,
        seed: read_u32(data, 16)?,
        deterministic_payload: flags & 0x01 != 0,
    })
}

pub(crate) fn encode_config(config: &BenchmarkConfig) -> [u8; 24] {
    let mut data = [0u8; 24];
    data[..4].copy_from_slice(b"NCF1");
    data[4..6].copy_from_slice(&(24u16).to_le_bytes());
    data[6] = 1;
    data[7] = config.generator;
    data[8] = config.mode;
    data[12..16].copy_from_slice(&config.frame_size.to_le_bytes());
    data[16..20].copy_from_slice(&config.seed.to_le_bytes());
    data[20..24].copy_from_slice(&(if config.deterministic_payload { 1u32 } else { 0 }).to_le_bytes());
    data
}

pub(crate) fn parse_status(data: &[u8]) -> Result<FirmwareStatus, String> {
    require_header(data, b"NST1", 48, "Status")?;
    Ok(FirmwareStatus {
        usb_state: data[7],
        configured: data[8] != 0,
        streaming: data[9] != 0,
        tx_busy: data[10] != 0,
        alt_setting: data[11],
        frames_generated: read_u32(data, 12)?,
        frames_sent: read_u32(data, 16)?,
        frames_dropped: read_u32(data, 20)?,
        bytes_sent: read_u64(data, 24)?,
        bulk_in_transfers: read_u32(data, 32)?,
        bulk_out_transfers: read_u32(data, 36)?,
        control_commands: read_u32(data, 40)?,
        uptime_ms: read_u32(data, 44)?,
    })
}

fn require_header(data: &[u8], magic: &[u8; 4], expected_size: usize, name: &str) -> Result<(), String> {
    if data.len() < expected_size {
        return Err(format!("{name} response too short: {} < {expected_size}", data.len()));
    }
    if &data[..4] != magic {
        return Err(format!("{name} magic mismatch: {:02X?}", &data[..4]));
    }
    let wire_size = read_u16(data, 4)? as usize;
    if wire_size < expected_size || wire_size > data.len() {
        return Err(format!("{name} struct size mismatch: wire={wire_size}, received={}", data.len()));
    }
    Ok(())
}

fn read_u16(data: &[u8], offset: usize) -> Result<u16, String> {
    let bytes: [u8; 2] = data
        .get(offset..offset + 2)
        .ok_or_else(|| format!("u16 out of range at {offset}"))?
        .try_into()
        .map_err(|_| "u16 conversion failed".to_string())?;
    Ok(u16::from_le_bytes(bytes))
}

fn read_u32(data: &[u8], offset: usize) -> Result<u32, String> {
    let bytes: [u8; 4] = data
        .get(offset..offset + 4)
        .ok_or_else(|| format!("u32 out of range at {offset}"))?
        .try_into()
        .map_err(|_| "u32 conversion failed".to_string())?;
    Ok(u32::from_le_bytes(bytes))
}

fn read_u64(data: &[u8], offset: usize) -> Result<u64, String> {
    let bytes: [u8; 8] = data
        .get(offset..offset + 8)
        .ok_or_else(|| format!("u64 out of range at {offset}"))?
        .try_into()
        .map_err(|_| "u64 conversion failed".to_string())?;
    Ok(u64::from_le_bytes(bytes))
}

fn read_fixed_string(data: &[u8]) -> String {
    let end = data.iter().position(|byte| *byte == 0).unwrap_or(data.len());
    String::from_utf8_lossy(&data[..end]).trim().to_string()
}

fn sample_payload(payload: &[u8], limit: usize) -> Vec<u8> {
    if payload.len() <= limit {
        return payload.to_vec();
    }
    (0..limit)
        .map(|index| {
            let source = index * payload.len() / limit;
            payload[source]
        })
        .collect()
}

fn frame_seed(seed: u32, sequence: u32) -> u32 {
    let state = seed ^ 0x9E37_79B9u32.wrapping_mul(sequence.wrapping_add(1));
    if state == 0 { 0xA341_316C } else { state }
}

fn xorshift32(state: &mut u32) -> u32 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}

fn lfsr32(state: &mut u32) -> u32 {
    let lsb = *state & 1;
    *state >>= 1;
    if lsb != 0 {
        *state ^= 0x8020_0003;
    }
    if *state == 0 {
        *state = 1;
    }
    *state
}

fn validate_payload(generator: u8, seed: u32, sequence: u32, payload: &[u8]) -> bool {
    match generator {
        GENERATOR_RAMP8 => payload
            .iter()
            .enumerate()
            .all(|(index, byte)| *byte == ((sequence as usize + index) & 0xFF) as u8),
        GENERATOR_XORSHIFT32 => validate_word_generator(payload, frame_seed(seed, sequence), xorshift32),
        GENERATOR_LFSR32 => validate_word_generator(payload, frame_seed(seed, sequence), lfsr32),
        _ => false,
    }
}

fn validate_word_generator(
    payload: &[u8],
    mut state: u32,
    next: fn(&mut u32) -> u32,
) -> bool {
    let mut offset = 0usize;
    while offset < payload.len() {
        let expected = next(&mut state).to_le_bytes();
        let count = (payload.len() - offset).min(4);
        if payload[offset..offset + count] != expected[..count] {
            return false;
        }
        offset += count;
    }
    true
}
