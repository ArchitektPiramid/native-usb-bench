import { useMemo } from "react";
import type { ChangeEvent } from "react";
import { fmtBytes, fmtTime } from "../format";
import { GENERATORS } from "../types";
import type { BenchStats, BenchmarkConfig, DeviceCapabilities } from "../types";

type StreamPanelProps = {
  connected: boolean;
  streaming: boolean;
  busy: boolean;
  config: BenchmarkConfig | null;
  capabilities: DeviceCapabilities | null;
  usbSpeed: string | null;
  stats: BenchStats;
  processCpuUsage: number | null;
  history: number[];
  onGeneratorChange: (generator: number) => void;
  onFrameSizeChange: (frameSize: number) => void;
  onStart: () => void;
  onStop: () => void;
};


function nominalUsbRateMbit(speed: string | null): number | null {
  switch (speed) {
    case "Low-Speed":
      return 1.5;
    case "Full-Speed":
      return 12;
    case "High-Speed":
      return 480;
    case "SuperSpeed":
      return 5000;
    case "SuperSpeed+":
      // USB 3.2 SuperSpeedPlus can represent multiple lane/rate variants.
      // Avoid presenting a guessed utilization until the exact link rate is known.
      return null;
    default:
      return null;
  }
}

export function StreamPanel({
  connected,
  streaming,
  busy,
  config,
  capabilities,
  usbSpeed,
  stats,
  processCpuUsage,
  history,
  onGeneratorChange,
  onFrameSizeChange,
  onStart,
  onStop,
}: StreamPanelProps) {
  const availableGenerators = GENERATORS.filter(
    (generator) => !capabilities || (capabilities.generator_mask & (1 << generator.id)) !== 0,
  );

  const frameSizes = [4096, 8192, 16384].filter(
    (size) => capabilities && size >= capabilities.min_frame_size && size <= capabilities.max_frame_size,
  );
  // Always keep the firmware's current size selectable, even on older targets.
  if (config && !frameSizes.includes(config.frame_size)) {
    frameSizes.unshift(config.frame_size);
  }

  const nominalRateMbit = nominalUsbRateMbit(usbSpeed);
  const nominalLinkUsage =
    nominalRateMbit && nominalRateMbit > 0 ? Math.max(0, (stats.mbit_per_sec / nominalRateMbit) * 100) : null;

  const points = useMemo(() => {
    if (history.length === 0) return "";
    const max = Math.max(0.01, ...history);
    return history
      .map((value, index) => {
        const x = history.length <= 1 ? 0 : (index / (history.length - 1)) * 100;
        const y = 100 - (value / max) * 82;
        return `${x},${Math.max(10, Math.min(92, y))}`;
      })
      .join(" ");
  }, [history]);

  return (
    <section className="panel stream-panel">
      <div className="stream-heading">
        <strong>Stream</strong>
        <span className={`stream-state ${streaming ? "running" : ""}`}>{streaming ? "Running" : "Idle"}</span>
        <span className="stream-frame">Frame {config ? `${config.frame_size} B` : "—"}</span>
      </div>

      <div className="stream-controls">
        <button className="button primary compact" disabled={!connected || streaming || busy} onClick={onStart}>
          Start
        </button>
        <button className="button danger compact" disabled={!connected || !streaming || busy} onClick={onStop}>
          Stop
        </button>
        <label className="stream-generator-control">
          <span>Generator</span>
          <select
            value={config?.generator ?? 1}
            disabled={!connected || streaming || busy || !config}
            onChange={(event: ChangeEvent<HTMLSelectElement>) => onGeneratorChange(Number(event.target.value))}
          >
            {availableGenerators.map((generator) => (
              <option key={generator.id} value={generator.id}>
                {generator.name}
              </option>
            ))}
          </select>
        </label>
        <label className="stream-generator-control">
          <span>Frame size</span>
          <select
            value={config?.frame_size ?? 4096}
            disabled={!connected || streaming || busy || !config || !capabilities?.configurable_frame_size}
            onChange={(event: ChangeEvent<HTMLSelectElement>) => onFrameSizeChange(Number(event.target.value))}
          >
            {frameSizes.map((size) => (
              <option key={size} value={size}>
                {size / 1024} KiB
              </option>
            ))}
          </select>
        </label>
      </div>

      <div className="stream-throughput compact-chart">
        <div>
          <span>Throughput</span>
          <strong>{stats.mb_per_sec.toFixed(3)} MB/s</strong>
          <small>{stats.mbit_per_sec.toFixed(2)} Mbit/s</small>
        </div>
        <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Recent throughput">
          <line x1="0" x2="100" y1="84" y2="84" className="stream-gridline" />
          {points && <polyline points={points} className="stream-line" />}
        </svg>
      </div>

      <div className="stream-stat-list">
        <StreamStat label="Frames / s" value={`${stats.frames_per_sec.toFixed(1)} /s`} />
        <StreamStat label="USB reads" value={`${stats.usb_reads_per_sec.toFixed(1)} /s`} />
        <StreamStat
          label="Process CPU"
          value={processCpuUsage == null ? "—" : `${processCpuUsage.toFixed(1)}%`}
          title="CPU usage of the NativeUSB Bench host process, normalized across all logical processors."
        />
        <StreamStat
          label="Payload / link rate"
          value={nominalLinkUsage == null ? "—" : `${nominalLinkUsage.toFixed(1)}%`}
          title="Application payload throughput divided by the nominal USB signaling rate. This is not physical bus utilization."
        />
        <StreamStat label="Received" value={fmtBytes(stats.bytes_total)} />
        <StreamStat label="Frames" value={stats.frames_total.toLocaleString()} />
        <StreamStat label="Dropped" value={stats.dropped_frames.toLocaleString()} bad={stats.dropped_frames > 0} />
        <StreamStat label="Corrupt" value={stats.corrupt_frames.toLocaleString()} bad={stats.corrupt_frames > 0} />
        <StreamStat label="Elapsed" value={fmtTime(stats.elapsed_ms)} />
        <StreamStat label="Last sequence" value={stats.last_sequence?.toLocaleString() ?? "—"} />
        <StreamStat label="USB errors" value={stats.usb_errors.toLocaleString()} bad={stats.usb_errors > 0} />
        <StreamStat label="Resync" value={fmtBytes(stats.discarded_bytes)} bad={stats.discarded_bytes > 0} />
      </div>
    </section>
  );
}

function StreamStat({
  label,
  value,
  bad = false,
  title,
}: {
  label: string;
  value: string;
  bad?: boolean;
  title?: string;
}) {
  return (
    <div className={`stream-stat-row ${bad ? "bad" : ""}`} title={title}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}
