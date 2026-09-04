import { hex } from "../format";
import type { ConnectedDeviceInfo, UsbDeviceInfo } from "../types";

type Props = {
  devices: UsbDeviceInfo[];
  selected: UsbDeviceInfo | null;
  selectedKey: string;
  setSelectedKey: (key: string) => void;
  connected: boolean;
  deviceDetails: ConnectedDeviceInfo | null;
  busy: boolean;
  refreshing: boolean;
  onRefresh: () => void;
  onConnect: () => void;
  onDisconnect: () => void;
};

export function DevicePanel({
  devices,
  selected,
  selectedKey,
  setSelectedKey,
  connected,
  deviceDetails,
  busy,
  refreshing,
  onRefresh,
  onConnect,
  onDisconnect,
}: Props) {
  const usb = deviceDetails?.usb ?? null;
  const info = deviceDetails?.device_info ?? null;

  return (
    <section className="panel connection-panel">
      <div className="device-browser">
        <div className="section-heading">
          <strong>Devices</strong>
          <span>{devices.length}</span>
        </div>

        <div className="device-list" aria-label="Available NativeUSB devices">
          {devices.length === 0 ? (
            <div className="device-empty">{refreshing ? "Scanning…" : "No compatible device found"}</div>
          ) : (
            devices.map((device) => {
              const isSelected = device.key === selectedKey;
              return (
                <button
                  key={device.key}
                  type="button"
                  className={`device-list-item ${isSelected ? "selected" : ""}`}
                  onClick={() => setSelectedKey(device.key)}
                  disabled={connected || busy || refreshing}
                >
                  <span className="device-list-main">
                    <span className={`device-dot ${connected && isSelected ? "connected" : ""}`} />
                    <strong>{device.product ?? `${hex(device.vid)}:${hex(device.pid)}`}</strong>
                  </span>
                  <span className="device-list-meta">
                    {hex(device.vid)}:{hex(device.pid)} · {device.speed} · Bus {device.bus} · Addr {device.address}
                  </span>
                </button>
              );
            })
          )}
        </div>

        <div className="device-actions">
          <button className="button" onClick={onRefresh} disabled={connected || busy || refreshing}>
            {refreshing ? "Scanning…" : "Refresh"}
          </button>
          {connected ? (
            <button className="button danger" onClick={onDisconnect} disabled={busy}>
              {busy ? "Disconnecting…" : "Disconnect"}
            </button>
          ) : (
            <button className="button primary" onClick={onConnect} disabled={!selectedKey || busy || refreshing}>
              {busy ? "Connecting…" : "Connect"}
            </button>
          )}
        </div>
      </div>

      <div className="connection-details">
        <div className="detail-section usb-descriptors">
          <div className="section-heading">
            <strong>USB descriptors</strong>
          </div>
          <div className="detail-lines">
            <DetailLine label="VID / PID" value={selected ? `${hex(selected.vid)} / ${hex(selected.pid)}` : "—"} />
            <DetailLine label="Speed" value={usb?.speed ?? selected?.speed ?? "—"} />
            <DetailLine label="USB Version" value={usb?.usb_version ?? "—"} />
            <DetailLine label="Interface" value={usb ? String(usb.interface) : "—"} />
            <DetailLine
              label="Bulk IN / OUT"
              value={
                usb
                  ? `0x${usb.ep_in.toString(16).padStart(2, "0").toUpperCase()} / 0x${usb.ep_out
                      .toString(16)
                      .padStart(2, "0")
                      .toUpperCase()}`
                  : "—"
              }
            />
            <DetailLine label="Max packet" value={usb ? `${usb.max_packet_size} B` : "—"} />
          </div>
        </div>

        <div className="detail-divider" />

        <div className="detail-section device-data">
          <div className="section-heading">
            <strong>Device Info</strong>
          </div>
          <div className="detail-lines">
            <DetailLine label="Target" value={info?.target_name ?? "—"} />
            <DetailLine label="MCU" value={info?.mcu_name ?? "—"} />
            <DetailLine label="Board" value={info?.board_name || "—"} />
            <DetailLine label="CPU" value={info ? `${(info.cpu_clock_hz / 1_000_000).toFixed(0)} MHz` : "—"} />
            <DetailSeparator />
            <DetailLine label="Firmware" value={info?.firmware_version ?? "—"} />
            <DetailLine label="Protocol" value={info?.protocol_version ?? "—"} />
            <DetailSeparator />
            <DetailLine label="Framework" value={info?.framework_name ?? "—"} />
            <DetailLine label="USB Stack" value={info?.usb_stack_name || "—"} />
          </div>
        </div>
      </div>
    </section>
  );
}

function DetailLine({ label, value }: { label: string; value: string }) {
  return (
    <div className="detail-line">
      <span>{label}</span>
      <strong title={value}>{value}</strong>
    </div>
  );
}

function DetailSeparator() {
  return <div className="detail-separator" aria-hidden="true" />;
}
