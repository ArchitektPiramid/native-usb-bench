import { useEffect, useRef } from "react";
import { generatorName } from "../types";
import type { PayloadVisual } from "../types";

export function PayloadVisualizer({ visual }: { visual: PayloadVisual | null }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const draw = () => {
      const rect = canvas.getBoundingClientRect();
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      canvas.width = Math.max(1, Math.round(rect.width * dpr));
      canvas.height = Math.max(1, Math.round(rect.height * dpr));

      const ctx = canvas.getContext("2d");
      if (!ctx) return;

      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      const width = rect.width;
      const height = rect.height;

      ctx.clearRect(0, 0, width, height);
      ctx.fillStyle = "#07111f";
      ctx.fillRect(0, 0, width, height);

      ctx.strokeStyle = "#1c3555";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(width / 2, 0);
      ctx.lineTo(width / 2, height);
      ctx.moveTo(0, height / 2);
      ctx.lineTo(width, height / 2);
      ctx.stroke();

      if (!visual || visual.sample.length < 2) return;

      const padding = 10;
      const innerWidth = Math.max(1, width - padding * 2);
      const innerHeight = Math.max(1, height - padding * 2);
      ctx.fillStyle = "rgba(91, 174, 255, 0.9)";

      for (let i = 0; i + 1 < visual.sample.length; i += 2) {
        const x = padding + (visual.sample[i] / 255) * innerWidth;
        const y = padding + (1 - visual.sample[i + 1] / 255) * innerHeight;
        ctx.fillRect(x, y, 1.8, 1.8);
      }
    };

    draw();
    const observer = new ResizeObserver(draw);
    observer.observe(canvas);
    return () => observer.disconnect();
  }, [visual]);

  return (
    <section className="panel payload-main">
      <div className="payload-heading">
        <strong>Payload</strong>
        <span>{visual ? `${generatorName(visual.generator)} · seq ${visual.sequence.toLocaleString()}` : "Waiting"}</span>
      </div>
      <canvas ref={canvasRef} className="payload-canvas" />
    </section>
  );
}

export function BitBalance({ visual }: { visual: PayloadVisual | null }) {
  return (
    <section className="panel bit-panel">
      <div className="payload-heading bit-heading">
        <strong>Bit balance</strong>
        <span>ones</span>
      </div>
      <div className="simple-bits compact-bits">
        {(visual?.bit_ones_percent ?? Array.from({ length: 8 }, () => 0)).map((value, bit) => (
          <div className="simple-bit-row" key={bit} title={`b${bit}: ${value.toFixed(1)}% ones`}>
            <span>b{bit}</span>
            <div className="simple-bit-track">
              <div className="simple-bit-fill" style={{ width: `${Math.min(100, Math.max(0, value))}%` }} />
            </div>
          </div>
        ))}
      </div>
    </section>
  );
}
