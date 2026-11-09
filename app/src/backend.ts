import type {
  BenchStats,
  BenchmarkConfig,
  ConnectedDeviceInfo,
  PayloadVisual,
  ProcessStats,
  UsbDeviceInfo,
  UsbLogEvent,
} from "./types";

export type Unsubscribe = () => void;

export interface NativeUsbBackend {
  listDevices(): Promise<UsbDeviceInfo[]>;
  connect(deviceKey: string): Promise<ConnectedDeviceInfo>;
  disconnect(): Promise<void>;
  setConfig(config: BenchmarkConfig): Promise<BenchmarkConfig>;
  startBenchmark(): Promise<void>;
  stopBenchmark(): Promise<void>;
  getProcessStats(): Promise<ProcessStats>;

  onStats(handler: (stats: BenchStats) => void): Promise<Unsubscribe>;
  onPayloadVisual(handler: (visual: PayloadVisual) => void): Promise<Unsubscribe>;
  onError(handler: (message: string) => void): Promise<Unsubscribe>;
  onDisconnected(handler: () => void): Promise<Unsubscribe>;
  onLog(handler: (event: UsbLogEvent) => void): Promise<Unsubscribe>;
}
