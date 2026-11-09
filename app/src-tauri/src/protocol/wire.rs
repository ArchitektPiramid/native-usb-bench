use super::{BenchmarkConfig, DeviceCapabilities, DeviceInfo, FirmwareStatus};

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

