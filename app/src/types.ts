export type UsbDeviceInfo = {
  key: string;
  bus: number;
  address: number;
  vid: number;
  pid: number;
  manufacturer: string | null;
  product: string | null;
  serial_number: string | null;
  speed: string;
  target: boolean;
};

export type UsbTransportInfo = {
  vid: number;
  pid: number;
  speed: string;
  usb_version: string;
  configuration: number;
  interface: number;
  alternate_setting: number;
  ep_in: number;
  ep_out: number;
  max_packet_size: number;
};

export type DeviceInfo = {
  protocol_version: string;
  firmware_version: string;
  cpu_clock_hz: number;
  target_id: number;
  mcu_id: number;
  target_name: string;
  mcu_name: string;
  board_name: string;
  framework_name: string;
  usb_stack_name: string;
};

export type DeviceCapabilities = {
  benchmark_modes: number;
  generator_mask: number;
  min_frame_size: number;
  max_frame_size: number;
  default_frame_size: number;
  deterministic_payload: boolean;
  configurable_generator: boolean;
  configurable_frame_size: boolean;
};

export type BenchmarkConfig = {
  generator: number;
  mode: number;
  frame_size: number;
  seed: number;
  deterministic_payload: boolean;
};

export type FirmwareStatus = {
  usb_state: number;
  configured: boolean;
  streaming: boolean;
  tx_busy: boolean;
  alt_setting: number;
  frames_generated: number;
  frames_sent: number;
  frames_dropped: number;
  bytes_sent: number;
  bulk_in_transfers: number;
  bulk_out_transfers: number;
  control_commands: number;
  uptime_ms: number;
};

export type ConnectedDeviceInfo = {
  usb: UsbTransportInfo;
  device_info: DeviceInfo;
  capabilities: DeviceCapabilities;
  config: BenchmarkConfig;
  status: FirmwareStatus;
};

export type BenchStats = {
  connected: boolean;
  streaming: boolean;
  bytes_total: number;
  frames_total: number;
  mb_per_sec: number;
  mbit_per_sec: number;
  avg_mb_per_sec: number;
  frames_per_sec: number;
  usb_reads_per_sec: number;
  dropped_frames: number;
  corrupt_frames: number;
  discarded_bytes: number;
  usb_errors: number;
  last_sequence: number | null;
  elapsed_ms: number;
};

export type PayloadVisual = {
  sequence: number;
  generator: number;
  sample: number[];
  histogram: number[];
  bit_ones_percent: number[];
};

export type UsbLogEvent = {
  level: string;
  message: string;
};

export type LogLine = UsbLogEvent & {
  id: number;
  time: string;
};

export const GENERATORS = [
  { id: 0, name: "Ramp8", description: "Sequential byte ramp; baseline pattern" },
  { id: 1, name: "Xorshift32", description: "Fast 32-bit pseudo-random generator" },
  { id: 2, name: "LFSR32", description: "PRBS-style 32-bit Galois LFSR" },
] as const;

export function generatorName(id: number) {
  return GENERATORS.find((generator) => generator.id === id)?.name ?? `Generator ${id}`;
}

export const EMPTY_STATS: BenchStats = {
  connected: false,
  streaming: false,
  bytes_total: 0,
  frames_total: 0,
  mb_per_sec: 0,
  mbit_per_sec: 0,
  avg_mb_per_sec: 0,
  frames_per_sec: 0,
  usb_reads_per_sec: 0,
  dropped_frames: 0,
  corrupt_frames: 0,
  discarded_bytes: 0,
  usb_errors: 0,
  last_sequence: null,
  elapsed_ms: 0,
};
