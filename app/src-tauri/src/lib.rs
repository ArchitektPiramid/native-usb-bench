mod benchmark;
mod protocol;
mod usb;

use benchmark::AppState;
use protocol::BenchmarkConfig;
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

pub fn run() {
    tauri::Builder::default()
        .manage(AppState::default())
        .invoke_handler(tauri::generate_handler![
            list_usb_devices,
            connect_device,
            set_benchmark_config,
            start_benchmark,
            stop_benchmark,
            disconnect_device
        ])
        .run(tauri::generate_context!())
        .expect("error while running NativeUSB Bench");
}
