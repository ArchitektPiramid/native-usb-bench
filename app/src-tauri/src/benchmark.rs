use crate::protocol::{
    BenchmarkConfig, FrameParser, PayloadVisual, CMD_RESET, CMD_START, CMD_STOP,
};
use crate::usb::{
    find_device, open_and_claim, read_firmware_status, send_usb_command, set_device_config,
    usb_log, ConnectedDeviceInfo,
};
use serde::Serialize;
use std::{
    sync::{mpsc, Mutex},
    thread,
    time::{Duration, Instant},
};
use tauri::{AppHandle, Emitter};

// Runtime tuning knobs. These intentionally live together so the hot-path
// behavior can be changed without hunting through the worker implementation.
pub(crate) const STATS_EMIT_INTERVAL_MS: u64 = 250;
pub(crate) const VISUAL_EMIT_INTERVAL_MS: u64 = 100;
const BULK_READ_TIMEOUT_MS: u64 = 100;
const IDLE_SLEEP_MS: u64 = 5;
const USB_ERROR_BACKOFF_MS: u64 = 10;
const WORKER_REPLY_TIMEOUT_MS: u64 = 2500;
const STREAM_STATE_TIMEOUT_MS: u64 = 1000;
const TEN_SECOND_SUMMARY_MS: u64 = 10_000;

#[derive(Clone, Debug, Serialize)]
pub(crate) struct BenchStats {
    connected: bool,
    streaming: bool,
    bytes_total: u64,
    frames_total: u64,
    mb_per_sec: f64,
    mbit_per_sec: f64,
    avg_mb_per_sec: f64,
    frames_per_sec: f64,
    usb_reads_per_sec: f64,
    dropped_frames: u64,
    corrupt_frames: u64,
    discarded_bytes: u64,
    usb_errors: u64,
    last_sequence: Option<u32>,
    elapsed_ms: u64,
}

impl BenchStats {
    fn idle(connected: bool) -> Self {
        Self {
            connected,
            streaming: false,
            bytes_total: 0,
            frames_total: 0,
            mb_per_sec: 0.0,
            mbit_per_sec: 0.0,
            avg_mb_per_sec: 0.0,
            frames_per_sec: 0.0,
            usb_reads_per_sec: 0.0,
            dropped_frames: 0,
            corrupt_frames: 0,
            discarded_bytes: 0,
            usb_errors: 0,
            last_sequence: None,
            elapsed_ms: 0,
        }
    }

    fn update_parser_stats(&mut self, parser: &FrameParser) {
        self.frames_total = parser.frames_total;
        self.dropped_frames = parser.dropped_frames;
        self.corrupt_frames = parser.corrupt_frames;
        self.discarded_bytes = parser.discarded_bytes;
        self.last_sequence = parser.last_sequence;
    }
}

pub(crate) enum WorkerCommand {
    Start(mpsc::Sender<Result<(), String>>),
    Stop(mpsc::Sender<Result<(), String>>),
    SetConfig(BenchmarkConfig, mpsc::Sender<Result<BenchmarkConfig, String>>),
    Disconnect,
}

struct WorkerHandle {
    command_tx: mpsc::Sender<WorkerCommand>,
    join: thread::JoinHandle<()>,
}

#[derive(Default)]
pub(crate) struct AppState {
    worker: Mutex<Option<WorkerHandle>>,
}

pub(crate) fn connect_device(
    device_key: String,
    app: AppHandle,
    state: &AppState,
) -> Result<ConnectedDeviceInfo, String> {
    let mut guard = state
        .worker
        .lock()
        .map_err(|_| "State lock poisoned".to_string())?;

    if guard.as_ref().is_some_and(|worker| worker.join.is_finished()) {
        if let Some(worker) = guard.take() {
            let _ = worker.join.join();
        }
    }
    if guard.is_some() {
        return Err("A USB device is already connected.".into());
    }

    usb_log(&app, "info", format!("connect requested for {device_key}"));
    let device = find_device(&device_key)?;
    let opened = open_and_claim(&device, &app)?;
    let details = opened.details.clone();
    let initial_config = details.config.clone();
    let capabilities = details.capabilities.clone();
    let ep_in = details.usb.ep_in;
    let interface = details.usb.interface;
    let max_packet_size = details.usb.max_packet_size;
    let (command_tx, command_rx) = mpsc::channel();

    let join = thread::spawn(move || {
        let handle = opened.handle;
        let mut parser = FrameParser::new(initial_config);
        // Match one logical frame per libusb read buffer. A larger read can
        // wait for extra full-size USB packets and time out at low data rates.
        let mut read_buffer = vec![0u8; parser.config().frame_size as usize];
        let mut stats = BenchStats::idle(true);
        let mut started_at: Option<Instant> = None;
        let mut last_emit = Instant::now();
        let mut last_visual_emit = Instant::now();
        let mut last_visual_sequence: Option<u32> = None;
        let mut sample_bytes = 0u64;
        let mut sample_frames = 0u64;
        let mut sample_usb_reads = 0u64;
        let mut ten_second_summary_logged = false;

        'worker: loop {
            while let Ok(command) = command_rx.try_recv() {
                match command {
                    WorkerCommand::Start(reply_tx) => {
                        if stats.streaming {
                            let _ = reply_tx.send(Err("Benchmark is already running".into()));
                            continue;
                        }
                        parser.reset();
                        stats = BenchStats::idle(true);
                        started_at = None;
                        last_visual_emit = Instant::now();
                        last_visual_sequence = None;
                        sample_bytes = 0;
                        sample_frames = 0;
                        sample_usb_reads = 0;
                        ten_second_summary_logged = false;

                        usb_log(
                            &app,
                            "info",
                            format!(
                                "benchmark start: generator={} frame={}B seed=0x{:08X}",
                                parser.config().generator,
                                parser.config().frame_size,
                                parser.config().seed
                            ),
                        );
                        let result = send_usb_command(&handle, CMD_RESET)
                            .and_then(|_| send_usb_command(&handle, CMD_START))
                            .and_then(|_| wait_for_stream_state(&handle, true));
                        match &result {
                            Ok(()) => {
                                stats.streaming = true;
                                started_at = Some(Instant::now());
                                last_emit = Instant::now();
                                usb_log(&app, "debug", "firmware confirmed streaming=true");
                            }
                            Err(error) => {
                                usb_log(&app, "error", error.clone());
                                let _ = app.emit("bench-error", error.clone());
                                // In case the command was applied but its status was lost.
                                let _ = send_usb_command(&handle, CMD_STOP);
                            }
                        }
                        let _ = app.emit("bench-stats", stats.clone());
                        let _ = reply_tx.send(result);
                    }
                    WorkerCommand::Stop(reply_tx) => {
                        if !stats.streaming {
                            let _ = reply_tx.send(Err("Benchmark is not running".into()));
                            continue;
                        }
                        usb_log(&app, "info", "benchmark stop requested over EP0");
                        let result = send_usb_command(&handle, CMD_STOP)
                            .and_then(|_| wait_for_stream_state(&handle, false));
                        match &result {
                            Ok(()) => {
                                // USB may still have packets queued after firmware STOP.
                                // Drain briefly so the final counters include in-flight frames.
                                let drain_start = Instant::now();
                                while drain_start.elapsed() < Duration::from_millis(200) {
                                    match handle.read_bulk(
                                        ep_in,
                                        &mut read_buffer,
                                        Duration::from_millis(20),
                                    ) {
                                        Ok(count) if count > 0 => {
                                            stats.bytes_total += count as u64;
                                            sample_usb_reads += 1;
                                            sample_bytes += count as u64;
                                            sample_frames += parser.push(&read_buffer[..count]);
                                            stats.update_parser_stats(&parser);
                                        }
                                        Ok(_) | Err(rusb::Error::Timeout) => break,
                                        Err(_) => { stats.usb_errors += 1; break; }
                                    }
                                }
                                stats.streaming = false;
                                if let Some(start) = started_at.take() {
                                    stats.elapsed_ms = start.elapsed().as_millis() as u64;
                                    let elapsed = stats.elapsed_ms as f64 / 1000.0;
                                    if elapsed > 0.0 {
                                        stats.avg_mb_per_sec =
                                            (stats.bytes_total as f64 / 1_000_000.0) / elapsed;
                                    }
                                }
                                usb_log(&app, "info", format_benchmark_summary("final", &stats));
                                stats.mb_per_sec = 0.0;
                                stats.mbit_per_sec = 0.0;
                                stats.frames_per_sec = 0.0;
                                stats.usb_reads_per_sec = 0.0;
                                sample_bytes = 0;
                                sample_frames = 0;
                                sample_usb_reads = 0;
                                last_emit = Instant::now();
                                let _ = app.emit("bench-stats", stats.clone());
                            }
                            Err(error) => {
                                usb_log(&app, "error", error.clone());
                                let _ = app.emit("bench-error", error.clone());
                            }
                        }
                        let _ = reply_tx.send(result);
                    }
                    WorkerCommand::SetConfig(requested, reply_tx) => {
                        let result = if stats.streaming {
                            Err("Stop the benchmark before changing configuration".to_string())
                        } else {
                            set_device_config(&handle, &capabilities, max_packet_size, &requested).map(|applied| {
                                parser.set_config(applied.clone());
                                read_buffer.resize(applied.frame_size as usize, 0);
                                usb_log(
                                    &app,
                                    "info",
                                    format!(
                                        "config applied: generator={} frame={}B seed=0x{:08X}",
                                        applied.generator, applied.frame_size, applied.seed
                                    ),
                                );
                                applied
                            })
                        };
                        let _ = reply_tx.send(result);
                    }
                    WorkerCommand::Disconnect => {
                        usb_log(&app, "info", "disconnect requested");
                        let _ = send_usb_command(&handle, CMD_STOP);
                        break 'worker;
                    }
                }
            }

            if stats.streaming {
                match handle.read_bulk(
                    ep_in,
                    &mut read_buffer,
                    Duration::from_millis(BULK_READ_TIMEOUT_MS),
                ) {
                    Ok(count) if count > 0 => {
                        sample_usb_reads += 1;
                        stats.bytes_total += count as u64;
                        sample_bytes += count as u64;
                        sample_frames += parser.push(&read_buffer[..count]);
                        stats.update_parser_stats(&parser);
                    }
                    Ok(_) => {}
                    Err(rusb::Error::Timeout) => {}
                    Err(rusb::Error::NoDevice) => {
                        usb_log(&app, "error", "USB device disconnected during Bulk IN");
                        let _ = app.emit("bench-error", "USB device disconnected".to_string());
                        break 'worker;
                    }
                    Err(error) => {
                        stats.usb_errors += 1;
                        usb_log(&app, "error", format!("Bulk IN read failed: {error}"));
                        let _ = app.emit("bench-error", format!("Bulk IN read failed: {error}"));
                        thread::sleep(Duration::from_millis(USB_ERROR_BACKOFF_MS));
                    }
                }
            } else {
                thread::sleep(Duration::from_millis(IDLE_SLEEP_MS));
            }

            if stats.streaming
                && last_visual_emit.elapsed() >= Duration::from_millis(VISUAL_EMIT_INTERVAL_MS)
            {
                if let Some(visual) = parser.payload_visual() {
                    if Some(visual.sequence) != last_visual_sequence {
                        emit_visual(&app, visual.clone());
                        last_visual_sequence = Some(visual.sequence);
                    }
                }
                last_visual_emit = Instant::now();
            }

            if last_emit.elapsed() >= Duration::from_millis(STATS_EMIT_INTERVAL_MS) {
                let interval = last_emit.elapsed().as_secs_f64().max(0.001);
                stats.mb_per_sec = (sample_bytes as f64 / 1_000_000.0) / interval;
                stats.mbit_per_sec = stats.mb_per_sec * 8.0;
                stats.frames_per_sec = sample_frames as f64 / interval;
                stats.usb_reads_per_sec = sample_usb_reads as f64 / interval;

                if let Some(start) = started_at {
                    stats.elapsed_ms = start.elapsed().as_millis() as u64;
                    let elapsed = start.elapsed().as_secs_f64();
                    stats.avg_mb_per_sec = if elapsed > 0.0 {
                        (stats.bytes_total as f64 / 1_000_000.0) / elapsed
                    } else {
                        0.0
                    };
                }

                if stats.streaming && !ten_second_summary_logged && stats.elapsed_ms >= TEN_SECOND_SUMMARY_MS {
                    usb_log(&app, "info", format_benchmark_summary("10s", &stats));
                    ten_second_summary_logged = true;
                }
                let _ = app.emit("bench-stats", stats.clone());
                sample_bytes = 0;
                sample_frames = 0;
                sample_usb_reads = 0;
                last_emit = Instant::now();
            }
        }

        let _ = handle.release_interface(interface);
        stats.connected = false;
        stats.streaming = false;
        let _ = app.emit("bench-stats", stats);
        let _ = app.emit("bench-disconnected", ());
    });

    *guard = Some(WorkerHandle { command_tx, join });
    Ok(details)
}

/// Emits copyable decimal-rate summaries for the benchmark results table.
fn format_benchmark_summary(label: &str, stats: &BenchStats) -> String {
    format!(
        "benchmark {label}: elapsed={:.2}s avg={:.3} MB/s ({:.3} Mbit/s) bytes={} frames={} dropped={} corrupt={} usb_errors={}",
        stats.elapsed_ms as f64 / 1000.0,
        stats.avg_mb_per_sec,
        stats.avg_mb_per_sec * 8.0,
        stats.bytes_total,
        stats.frames_total,
        stats.dropped_frames,
        stats.corrupt_frames,
        stats.usb_errors,
    )
}

fn emit_visual(app: &AppHandle, visual: PayloadVisual) {
    let _ = app.emit("payload-visual", visual);
}

pub(crate) fn start_benchmark(state: &AppState) -> Result<(), String> {
    send_ack_worker_command(state, WorkerCommand::Start)
}

pub(crate) fn stop_benchmark(state: &AppState) -> Result<(), String> {
    send_ack_worker_command(state, WorkerCommand::Stop)
}

pub(crate) fn set_benchmark_config(
    state: &AppState,
    config: BenchmarkConfig,
) -> Result<BenchmarkConfig, String> {
    let guard = state
        .worker
        .lock()
        .map_err(|_| "State lock poisoned".to_string())?;
    let worker = guard
        .as_ref()
        .ok_or_else(|| "No USB device connected".to_string())?;

    let (reply_tx, reply_rx) = mpsc::channel();
    worker
        .command_tx
        .send(WorkerCommand::SetConfig(config, reply_tx))
        .map_err(|_| "USB worker has stopped".to_string())?;

    reply_rx
        .recv_timeout(Duration::from_millis(WORKER_REPLY_TIMEOUT_MS))
        .map_err(|_| "Timed out waiting for USB worker to apply configuration".to_string())?
}

pub(crate) fn disconnect_device(state: &AppState) -> Result<(), String> {
    let worker = {
        let mut guard = state
            .worker
            .lock()
            .map_err(|_| "State lock poisoned".to_string())?;
        guard.take()
    };

    if let Some(worker) = worker {
        let _ = worker.command_tx.send(WorkerCommand::Disconnect);
        let _ = worker.join.join();
    }
    Ok(())
}

// A command completes only when its EP0 transfer and firmware status check finish.
fn send_ack_worker_command(
    state: &AppState,
    command: impl FnOnce(mpsc::Sender<Result<(), String>>) -> WorkerCommand,
) -> Result<(), String> {
    let guard = state
        .worker
        .lock()
        .map_err(|_| "State lock poisoned".to_string())?;
    let worker = guard
        .as_ref()
        .ok_or_else(|| "No USB device connected".to_string())?;
    let (reply_tx, reply_rx) = mpsc::channel();
    worker
        .command_tx
        .send(command(reply_tx))
        .map_err(|_| "USB worker has stopped".to_string())?;
    reply_rx
        .recv_timeout(Duration::from_millis(WORKER_REPLY_TIMEOUT_MS))
        .map_err(|_| "Timed out waiting for USB benchmark command".to_string())?
}

fn wait_for_stream_state(
    handle: &rusb::DeviceHandle<rusb::GlobalContext>,
    expected: bool,
) -> Result<(), String> {
    let start = Instant::now();
    let mut last_error = String::new();
    while start.elapsed() < Duration::from_millis(STREAM_STATE_TIMEOUT_MS) {
        match read_firmware_status(handle) {
            Ok(status) if status.streaming == expected => return Ok(()),
            Ok(_) => {}
            Err(error) => last_error = error,
        }
        thread::sleep(Duration::from_millis(10));
    }
    Err(format!(
        "Firmware did not confirm streaming={expected} within {STREAM_STATE_TIMEOUT_MS} ms{}",
        if last_error.is_empty() { String::new() } else { format!(": {last_error}") }
    ))
}

#[cfg(test)]
mod benchmark_summary_tests {
    use super::*;

    #[test]
    fn summary_reports_decimal_throughput_and_error_counters() {
        let mut stats = BenchStats::idle(true);
        stats.elapsed_ms = 10_000;
        stats.bytes_total = 8_000_000;
        stats.avg_mb_per_sec = 0.8;
        stats.frames_total = 1953;
        stats.dropped_frames = 2;
        stats.corrupt_frames = 1;
        stats.usb_errors = 3;

        assert_eq!(
            format_benchmark_summary("10s", &stats),
            "benchmark 10s: elapsed=10.00s avg=0.800 MB/s (6.400 Mbit/s) bytes=8000000 frames=1953 dropped=2 corrupt=1 usb_errors=3"
        );
    }

    #[test]
    fn final_summary_supports_short_or_empty_runs() {
        let stats = BenchStats::idle(true);
        assert_eq!(
            format_benchmark_summary("final", &stats),
            "benchmark final: elapsed=0.00s avg=0.000 MB/s (0.000 Mbit/s) bytes=0 frames=0 dropped=0 corrupt=0 usb_errors=0"
        );
    }
}
