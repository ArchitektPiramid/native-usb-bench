import { useCallback, useEffect, useMemo, useState } from "react";
import { currentTime } from "../format";
import { tauriBackend } from "../tauriBackend";
import { EMPTY_STATS } from "../types";
import type {
  BenchStats,
  BenchmarkConfig,
  ConnectedDeviceInfo,
  LogLine,
  PayloadVisual,
  UsbDeviceInfo,
} from "../types";

export function useNativeUsbBench() {
  const [devices, setDevices] = useState<UsbDeviceInfo[]>([]);
  const [selectedKey, setSelectedKey] = useState("");
  const [connectedKey, setConnectedKey] = useState("");
  const [deviceDetails, setDeviceDetails] = useState<ConnectedDeviceInfo | null>(null);
  const [config, setConfig] = useState<BenchmarkConfig | null>(null);
  const [stats, setStats] = useState<BenchStats>(EMPTY_STATS);
  const [history, setHistory] = useState<number[]>([]);
  const [payloadVisual, setPayloadVisual] = useState<PayloadVisual | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const [refreshing, setRefreshing] = useState(false);
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [processCpuUsage, setProcessCpuUsage] = useState<number | null>(null);

  const connected = Boolean(connectedKey);
  const selected = useMemo(
    () => devices.find((device) => device.key === selectedKey) ?? null,
    [devices, selectedKey],
  );

  const addLocalLog = useCallback((level: string, message: string) => {
    setLogs((old) => [
      ...old.slice(-249),
      { id: Date.now() + Math.random(), time: currentTime(), level, message },
    ]);
  }, []);

  const refreshDevices = useCallback(async () => {
    if (refreshing) return;
    setRefreshing(true);
    setError("");
    addLocalLog("debug", "refresh supported NativeUSB targets");

    // Yield a paint so the button can immediately enter the Refreshing state.
    await new Promise<void>((resolve) => requestAnimationFrame(() => resolve()));

    try {
      const found = await tauriBackend.listDevices();
      setDevices(found);
      setSelectedKey((current) =>
        found.some((device) => device.key === current) ? current : (found[0]?.key ?? ""),
      );
      addLocalLog("info", found.length ? `target detected (${found.length})` : "target not detected");
    } catch (e) {
      const message = String(e);
      setError(message);
      addLocalLog("error", message);
    } finally {
      setRefreshing(false);
    }
  }, [addLocalLog, refreshing]);

  const connect = useCallback(async () => {
    if (!selectedKey) return;
    setBusy(true);
    setError("");
    addLocalLog("info", `connect ${selectedKey}`);

    try {
      const details = await tauriBackend.connect(selectedKey);
      setConnectedKey(selectedKey);
      setDeviceDetails(details);
      setConfig(details.config);
      setStats({ ...EMPTY_STATS, connected: true });
      setHistory([]);
      setPayloadVisual(null);
      addLocalLog(
        "info",
        `connected: ${details.device_info.mcu_name} · firmware ${details.device_info.firmware_version}`,
      );
    } catch (e) {
      const message = String(e);
      setError(message);
      addLocalLog("error", message);
    } finally {
      setBusy(false);
    }
  }, [addLocalLog, selectedKey]);

  const disconnect = useCallback(async () => {
    setBusy(true);
    addLocalLog("info", "disconnect");

    try {
      await tauriBackend.disconnect();
    } catch (e) {
      const message = String(e);
      setError(message);
      addLocalLog("error", message);
    } finally {
      setConnectedKey("");
      setDeviceDetails(null);
      setConfig(null);
      setStats(EMPTY_STATS);
      setHistory([]);
      setPayloadVisual(null);
      setBusy(false);
    }
  }, [addLocalLog]);

  const updateConfig = useCallback(
    async (next: BenchmarkConfig) => {
      if (!connected || stats.streaming) return;
      setBusy(true);
      setError("");
      try {
        const applied = await tauriBackend.setConfig(next);
        setConfig(applied);
        setDeviceDetails((current) => (current ? { ...current, config: applied } : current));
        setPayloadVisual(null);
      } catch (e) {
        const message = String(e);
        setError(message);
        addLocalLog("error", message);
      } finally {
        setBusy(false);
      }
    },
    [addLocalLog, connected, stats.streaming],
  );

  const setGenerator = useCallback(
    async (generator: number) => {
      if (!config || config.generator === generator) return;
      addLocalLog("info", `set payload generator ${generator}`);
      await updateConfig({ ...config, generator });
    },
    [addLocalLog, config, updateConfig],
  );

  const startBenchmark = useCallback(async () => {
    if (!connected || busy || stats.streaming) return;

    setBusy(true);
    setError("");
    setHistory([]);
    setPayloadVisual(null);
    addLocalLog("info", "start benchmark");

    try {
      // The Rust command resolves only after the MCU accepts RESET + START.
      await tauriBackend.startBenchmark();
    } catch (e) {
      const message = String(e);
      setError(message);
      addLocalLog("error", message);
    } finally {
      setBusy(false);
    }
  }, [addLocalLog, busy, connected, stats.streaming]);

  const stopBenchmark = useCallback(async () => {
    if (!connected || busy || !stats.streaming) return;

    setBusy(true);
    setError("");
    addLocalLog("info", "stop benchmark");

    try {
      // Stop resolves after the MCU accepts STOP and the final frozen stats
      // snapshot has been emitted by the worker.
      await tauriBackend.stopBenchmark();
    } catch (e) {
      const message = String(e);
      setError(message);
      addLocalLog("error", message);
    } finally {
      setBusy(false);
    }
  }, [addLocalLog, busy, connected, stats.streaming]);

  const clearLogs = useCallback(() => setLogs([]), []);

  useEffect(() => {
    let disposed = false;
    const cleanup: Array<() => void> = [];

    void (async () => {
      cleanup.push(
        await tauriBackend.onStats((nextStats) => {
          if (disposed) return;
          setStats(nextStats);
          if (nextStats.streaming) {
            setHistory((old) => [...old.slice(-79), nextStats.mb_per_sec]);
          }
        }),
      );

      cleanup.push(
        await tauriBackend.onPayloadVisual((visual) => {
          if (disposed) return;
          setPayloadVisual(visual);
        }),
      );

      cleanup.push(
        await tauriBackend.onError((message) => {
          if (disposed) return;
          setError(message);
          addLocalLog("error", message);
        }),
      );

      cleanup.push(
        await tauriBackend.onDisconnected(() => {
          if (disposed) return;
          setConnectedKey("");
          setDeviceDetails(null);
          setConfig(null);
          setStats(EMPTY_STATS);
          setPayloadVisual(null);
          addLocalLog("warn", "USB worker disconnected");
        }),
      );

      cleanup.push(
        await tauriBackend.onLog((event) => {
          if (disposed) return;
          addLocalLog(event.level, event.message);
        }),
      );

      if (!disposed) {
        await refreshDevices();
      }
    })();

    return () => {
      disposed = true;
      cleanup.forEach((unsubscribe) => unsubscribe());
    };
    // Event subscriptions are intentionally created once for the app lifetime.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    let disposed = false;
    let inFlight = false;

    const updateProcessCpu = async () => {
      if (inFlight) return;
      inFlight = true;

      try {
        const processStats = await tauriBackend.getProcessStats();
        if (!disposed) {
          setProcessCpuUsage(processStats.cpu_usage_percent);
        }
      } catch {
        if (!disposed) {
          setProcessCpuUsage(null);
        }
      } finally {
        inFlight = false;
      }
    };

    void updateProcessCpu();
    const intervalId = window.setInterval(() => void updateProcessCpu(), 500);

    return () => {
      disposed = true;
      window.clearInterval(intervalId);
    };
  }, []);

  return {
    devices,
    selected,
    selectedKey,
    setSelectedKey,
    connected,
    deviceDetails,
    config,
    stats,
    processCpuUsage,
    history,
    payloadVisual,
    error,
    busy,
    refreshing,
    logs,
    refreshDevices,
    connect,
    disconnect,
    setGenerator,
    startBenchmark,
    stopBenchmark,
    clearLogs,
  };
}
