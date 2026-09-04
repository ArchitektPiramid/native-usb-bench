import type { ReactNode } from "react";
import { hex } from "../format";
import { GENERATORS, generatorName } from "../types";
import type { ConnectedDeviceInfo } from "../types";

export function DeviceDetails({ details }: { details: ConnectedDeviceInfo | null }) {
  if (!details) return null;

  const generators = GENERATORS.filter(
    (generator) => (details.capabilities.generator_mask & (1 << generator.id)) !== 0,
  )
    .map((generator) => generator.name)
    .join(", ");

  return (
    <section className="details-grid">
      <InfoCard title="USB descriptors">
        <Row label="VID / PID" value={`${hex(details.usb.vid)} / ${hex(details.usb.pid)}`} />
        <Row label="Speed" value={details.usb.speed} />
        <Row label="Interface" value={String(details.usb.interface)} />
        <Row label="Bulk IN / OUT" value={`${hex(details.usb.ep_in)} / ${hex(details.usb.ep_out)}`} />
        <Row label="Max packet" value={`${details.usb.max_packet_size} B`} />
      </InfoCard>

      <InfoCard title="DeviceInfo">
        <Row label="Target" value={details.device_info.target_name} />
        <Row label="MCU" value={details.device_info.mcu_name} />
        <Row label="CPU" value={`${(details.device_info.cpu_clock_hz / 1_000_000).toFixed(0)} MHz`} />
        <Row label="Firmware" value={details.device_info.firmware_version} />
        <Row label="Protocol" value={details.device_info.protocol_version} />
        <Row label="Framework" value={details.device_info.framework_name} />
      </InfoCard>

      <InfoCard title="Capabilities">
        <Row label="Benchmark" value={details.capabilities.benchmark_modes & 1 ? "MCU → PC" : "—"} />
        <Row label="Generators" value={generators || "—"} />
        <Row
          label="Frame range"
          value={`${details.capabilities.min_frame_size}–${details.capabilities.max_frame_size} B`}
        />
        <Row label="Default frame" value={`${details.capabilities.default_frame_size} B`} />
        <Row label="Validation" value={details.capabilities.deterministic_payload ? "Deterministic" : "Optional"} />
      </InfoCard>

      <InfoCard title="Config / status">
        <Row label="Generator" value={generatorName(details.config.generator)} />
        <Row label="Frame" value={`${details.config.frame_size} B`} />
        <Row label="Seed" value={`0x${details.config.seed.toString(16).toUpperCase().padStart(8, "0")}`} />
        <Row label="Configured" value={details.status.configured ? "Yes" : "No"} />
        <Row label="TX busy" value={details.status.tx_busy ? "Yes" : "No"} />
        <Row label="Control cmds" value={details.status.control_commands.toLocaleString()} />
      </InfoCard>
    </section>
  );
}

function InfoCard({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="info-card">
      <h3>{title}</h3>
      <div className="info-rows">{children}</div>
    </div>
  );
}

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="info-row">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}
