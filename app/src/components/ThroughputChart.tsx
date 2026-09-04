import { useMemo } from "react";
import { fmtBytes, fmtTime } from "../format";
import type { BenchStats } from "../types";

type Props = {
  values: number[];
  stats: BenchStats;
};

export function ThroughputChart({ values, stats }: Props) {
  const points = useMemo(() => {
    if (values.length === 0) return "";
    const max = Math.max(1.2, ...values, 0.01);
    return values
      .map((value, index) => {
        const x = values.length <= 1 ? 0 : (index / (values.length - 1)) * 100;
        const y = 100 - (value / max) * 90;
        return `${x},${Math.max(4, Math.min(96, y))}`;
      })
      .join(" ");
  }, [values]);

  return (
    <section className="panel chart-panel">
      <div className="chart-header">
        <strong>Throughput</strong>
        <span>{stats.streaming ? "Running" : "Idle"}</span>
      </div>

      <div className="chart-wrap">
        <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-label="Live throughput chart">
          <line x1="0" x2="100" y1="50" y2="50" className="gridline" />
          {points && <polyline points={points} className="throughput-line" />}
        </svg>
      </div>

      <div className="chart-footer">
        <span>{fmtBytes(stats.bytes_total)} received</span>
        <span>{stats.frames_total.toLocaleString()} frames</span>
        <span>{fmtTime(stats.elapsed_ms)}</span>
        <span>seq {stats.last_sequence ?? "—"}</span>
        <span>{stats.usb_errors} USB errors</span>
        <span>{fmtBytes(stats.discarded_bytes)} resync</span>
      </div>
    </section>
  );
}
