import { StreamPanel } from "./components/BenchmarkPanel";
import { DebugLog } from "./components/DebugLog";
import { DevicePanel } from "./components/DevicePanel";
import { BitBalance, PayloadVisualizer } from "./components/PayloadVisualizer";
import { useNativeUsbBench } from "./hooks/useNativeUsbBench";

export default function App() {
  const bench = useNativeUsbBench();

  return (
    <main className="app-shell">
      <header className="app-header">
        <div>
          <h1>NativeUSB Bench</h1>
          <p>Native USB throughput benchmark</p>
        </div>
        <div className={`connection-state ${bench.connected ? "connected" : ""}`}>
          <span className="status-dot" />
          {bench.connected ? "Connected" : "Disconnected"}
        </div>
      </header>

      <DevicePanel
        devices={bench.devices}
        selected={bench.selected}
        selectedKey={bench.selectedKey}
        setSelectedKey={bench.setSelectedKey}
        connected={bench.connected}
        deviceDetails={bench.deviceDetails}
        busy={bench.busy}
        refreshing={bench.refreshing}
        onRefresh={() => void bench.refreshDevices()}
        onConnect={() => void bench.connect()}
        onDisconnect={() => void bench.disconnect()}
      />

      {bench.error && <div className="error-box">{bench.error}</div>}

      <section className="workspace-layout">
        <PayloadVisualizer visual={bench.payloadVisual} />

        <div className="workspace-side">
          <StreamPanel
            connected={bench.connected}
            streaming={bench.stats.streaming}
            busy={bench.busy}
            config={bench.config}
            capabilities={bench.deviceDetails?.capabilities ?? null}
            usbSpeed={bench.deviceDetails?.usb.speed ?? null}
            stats={bench.stats}
            processCpuUsage={bench.processCpuUsage}
            history={bench.history}
            onGeneratorChange={(generator) => void bench.setGenerator(generator)}
            onFrameSizeChange={(frameSize) => void bench.setFrameSize(frameSize)}
            onStart={() => void bench.startBenchmark()}
            onStop={() => void bench.stopBenchmark()}
          />
          <BitBalance visual={bench.payloadVisual} />
        </div>
      </section>

      <details className="debug-details">
        <summary>
          <span>Debug log</span>
          <span>{bench.logs.length} entries</span>
        </summary>
        <DebugLog logs={bench.logs} onClear={bench.clearLogs} />
      </details>
    </main>
  );
}
