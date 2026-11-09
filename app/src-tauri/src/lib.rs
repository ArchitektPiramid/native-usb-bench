mod benchmark;
mod protocol;
mod system_stats;
mod usb;

use benchmark::AppState;
use protocol::BenchmarkConfig;
use system_stats::{ProcessStats, ProcessStatsState};
use tauri::{AppHandle, State};

#[tauri::command]
fn list_usb_devices(app: AppHandle) -> Result<Vec<usb::UsbDeviceInfo>, String> {
    usb::list_usb_devices(&app)
}

#[tauri::command]
fn connect_device(
    device_key: String,
    app: AppHandle,
    state: State<'_, AppState>,
) -> Result<usb::ConnectedDeviceInfo, String> {
    benchmark::connect_device(device_key, app, state.inner())
}

#[tauri::command]
fn set_benchmark_config(
    config: BenchmarkConfig,
    state: State<'_, AppState>,
) -> Result<BenchmarkConfig, String> {
    benchmark::set_benchmark_config(state.inner(), config)
}

#[tauri::command]
fn start_benchmark(state: State<'_, AppState>) -> Result<(), String> {
    benchmark::start_benchmark(state.inner())
}

#[tauri::command]
fn stop_benchmark(state: State<'_, AppState>) -> Result<(), String> {
    benchmark::stop_benchmark(state.inner())
}

#[tauri::command]
fn disconnect_device(state: State<'_, AppState>) -> Result<(), String> {
    benchmark::disconnect_device(state.inner())
}

#[tauri::command]
fn get_process_stats(state: State<'_, ProcessStatsState>) -> Result<ProcessStats, String> {
    state.sample()
}

pub fn run() {
    tauri::Builder::default()
        .manage(AppState::default())
        .manage(ProcessStatsState::new())
        .invoke_handler(tauri::generate_handler![
            list_usb_devices,
            connect_device,
            set_benchmark_config,
            start_benchmark,
            stop_benchmark,
            disconnect_device,
            get_process_stats
        ])
        .run(tauri::generate_context!())
        .expect("error while running NativeUSB Bench");
}
