/**
 * PerfOverlay.tsx — v0.0.994: frame-time overlay (Preferences → Display →
 * Performance overlay, or ⇧⌘P / Ctrl+Shift+P)
 *
 * The user's request (2026-11-22): a built-in tool to see the UI's load, on
 * macOS and on Garuda alike (no Safari Web Inspector needed).
 *
 * Top: frames per second, average and worst frame time over the last second,
 * how many frames took longer than 33 ms (a visible stutter at 30 fps), and a
 * strip of the last 120 frames (green ≤ 17 ms, amber ≤ 33 ms, red beyond).
 * Below: the probes of Perf.ts — what reached the UI in the last second (how
 * many times) and how long our own JS spent on each (ms per second; 1000 =
 * the whole second). Updated 4× a second straight into the DOM (no React
 * re-render), so the overlay itself barely shows in its own numbers.
 */

import { useEffect, useRef } from 'react';
import { perfSnapshot, setPerfOn } from './Perf';

const N = 120;

export default function PerfOverlay ({ onClose }: { onClose: () => void }) {
  const textRef   = useRef<HTMLDivElement> (null);
  const probesRef = useRef<HTMLDivElement> (null);
  const canvasRef = useRef<HTMLCanvasElement> (null);

  useEffect (() => {
    setPerfOn (true);
    const frames = new Float32Array (N);   // ring of frame times (ms)
    let head = 0, prev = performance.now (), raf = 0, lastPaint = 0;
    const secTimes: number[] = [];          // [timestamp, dt] pairs of the last second

    const tick = (now: number) => {
      const dt = now - prev; prev = now;
      frames[head] = dt; head = (head + 1) % N;
      secTimes.push (now, dt);
      while (secTimes.length > 0 && secTimes[0] < now - 1000) secTimes.splice (0, 2);

      if (now - lastPaint > 250) {
        lastPaint = now;
        let sum = 0, worst = 0, long = 0, n = 0;
        for (let i = 1; i < secTimes.length; i += 2) { const d = secTimes[i]; sum += d; n++; if (d > worst) worst = d; if (d > 33.4) long++; }
        if (textRef.current)
          textRef.current.textContent = `${n} fps · avg ${(n ? sum / n : 0).toFixed (1)} ms · worst ${worst.toFixed (0)} ms · >33 ms: ${long}`;

        const c = canvasRef.current, g = c?.getContext ('2d');
        if (c && g) {
          g.clearRect (0, 0, c.width, c.height);
          const w = c.width / N;
          for (let k = 0; k < N; k++) {
            const d = frames[(head + k) % N];
            const h = Math.min (c.height, (d / 50) * c.height);
            g.fillStyle = d <= 17.5 ? '#3fb950' : d <= 33.4 ? '#d29922' : '#f85149';
            g.fillRect (k * w, c.height - h, Math.max (1, w - 0.5), h);
          }
          g.fillStyle = 'rgba(255,255,255,0.25)';
          g.fillRect (0, c.height - (16.7 / 50) * c.height, c.width, 1);   // 60 fps line
        }

        if (probesRef.current) {
          const rows = perfSnapshot ();
          probesRef.current.textContent = rows.length === 0 ? '—'
            : rows.map (([name, a]) => `${name.padEnd (16)} ${String (a.n).padStart (5)}/s${a.ms > 0 ? `  ${a.ms.toFixed (1).padStart (6)} ms/s` : ''}`).join ('\n');
        }
      }
      raf = requestAnimationFrame (tick);
    };
    raf = requestAnimationFrame (tick);
    return () => { cancelAnimationFrame (raf); setPerfOn (false); };
  }, []);

  return (
    <div className="nodrag nowheel"
      style={{ position: 'fixed', right: 12, bottom: 12, zIndex: 10000, width: 300, padding: '6px 8px',
               background: 'rgba(13,17,23,0.88)', border: '1px solid var(--border)', borderRadius: 6,
               font: '10px/1.35 ui-monospace, Menlo, monospace', color: '#c9d1d9', pointerEvents: 'auto' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: 4 }}>
        <span style={{ color: 'var(--accent)', fontWeight: 600 }}>PERFORMANCE</span>
        <span onClick={onClose} style={{ cursor: 'pointer', color: 'var(--text-muted)' }} title="Close (⇧⌘P / Ctrl+Shift+P)">×</span>
      </div>
      <div ref={textRef}>…</div>
      <canvas ref={canvasRef} width={284} height={36} style={{ width: 284, height: 36, display: 'block', margin: '4px 0' }} />
      <div style={{ color: 'var(--text-muted)', marginBottom: 2 }}>what reached the UI · JS time spent on it</div>
      <div ref={probesRef} style={{ whiteSpace: 'pre' }}>—</div>
    </div>
  );
}
