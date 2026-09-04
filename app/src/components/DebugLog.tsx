import { useEffect, useRef } from "react";
import type { LogLine } from "../types";

type Props = {
  logs: LogLine[];
  onClear: () => void;
};

export function DebugLog({ logs, onClear }: Props) {
  const logViewRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    const element = logViewRef.current;
    if (element) element.scrollTop = element.scrollHeight;
  }, [logs]);

  return (
    <section className="panel log-panel">
      <div className="section-heading compact-heading">
        <div>
          <h2>Debug log</h2>
          <p>
            USB discovery, WinUSB handshake, EP0 commands and Bulk transfer errors. Backend messages are also printed to the Tauri terminal.
          </p>
        </div>
        <button className="secondary" onClick={onClear}>Clear</button>
      </div>

      <div className="log-view" ref={logViewRef}>
        {logs.length === 0 ? (
          <div className="log-empty">No log entries yet.</div>
        ) : (
          logs.map((line) => (
            <div className={`log-line ${line.level}`} key={line.id}>
              <span className="log-time">{line.time}</span>
              <span className="log-level">{line.level.toUpperCase()}</span>
              <span className="log-message">{line.message}</span>
            </div>
          ))
        )}
      </div>
    </section>
  );
}
