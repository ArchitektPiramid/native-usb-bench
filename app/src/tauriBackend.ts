import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import type { NativeUsbBackend } from "./backend";
import type {
  BenchStats,
  BenchmarkConfig,
  ConnectedDeviceInfo,
  PayloadVisual,
  ProcessStats,
  UsbDeviceInfo,
  UsbLogEvent,
} from "./types";

export const tauriBackend: NativeUsbBackend = {
  listDevices() {
    return invoke<UsbDeviceInfo[]>("list_usb_devices");
  },

  connect(deviceKey) {
    return invoke<ConnectedDeviceInfo>("connect_device", { deviceKey });
  },

  disconnect() {
    return invoke("disconnect_device");
  },

  setConfig(config: BenchmarkConfig) {
    return invoke<BenchmarkConfig>("set_benchmark_config", { config });
  },

  startBenchmark() {
    return invoke("start_benchmark");
  },

  stopBenchmark() {
    return invoke("stop_benchmark");
  },

  getProcessStats() {
    return invoke<ProcessStats>("get_process_stats");
  },

  onStats(handler) {
    return listen<BenchStats>("bench-stats", (event) => handler(event.payload));
  },

  onPayloadVisual(handler) {
    return listen<PayloadVisual>("payload-visual", (event) => handler(event.payload));
  },

  onError(handler) {
    return listen<string>("bench-error", (event) => handler(event.payload));
  },

  onDisconnected(handler) {
    return listen("bench-disconnected", () => handler());
  },

  onLog(handler) {
    return listen<UsbLogEvent>("usb-log", (event) => handler(event.payload));
  },
};
