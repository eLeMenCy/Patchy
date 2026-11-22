// ValueMonitorNode.tsx — v0.0.992: the Value Monitor Pax's node (user,
// 2026-11-20: a way to see what a Value wire carries while testing the hub).
//
// The current value large, min / max seen and how many values arrived (all
// read-only from the Pax), and a scrolling graph of the last ~6 s drawn from
// the live readout (~30 samples / s — a monitor, not a recorder; the graph
// scales itself to what it shows). ⏸ freezes the display; ⟲ clears min / max /
// count (the Pax's Reset) and the graph.

import { useState, useEffect, useRef, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, NodeHeaderButton, NodeCollapseArrow,
         EditableTitle, commitModelName, _paxInfoMap, detectPaxTheme } from './NodeUtils';
import { HintContext } from './HintPanel';
import { X, Pause, Play, RotateCcw } from 'lucide-react';
import { perfCount } from './Perf';   // v0.0.994

const HISTORY = 180;   // samples (~6 s at the ~30 Hz activity feed)
const num = (v: number) => (Math.abs (v) >= 1000 ? v.toFixed (0) : (Math.round (v * 1000) / 1000).toString ());

export default function ValueMonitorNode ({ id, data, selected }: NodeProps) {
  perfCount ('render ValueMonitorNode');   // v0.0.994 — overlay: how often it re-renders
  const d = data as any;
  const { setHint } = useContext (HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, d._forceCollapsed);
  // v0.0.994 — the live readouts are written straight into the DOM (no
  // re-render per push: 3 monitors re-rendered ~160×/s in the user's test)
  const ro = useRef<{ last: number; mn: number; mx: number; count: number }> ({ last: 0, mn: 0, mx: 0, count: 0 });
  const txt = useRef<Record<'last' | 'mn' | 'mx' | 'count', HTMLSpanElement | null>> ({ last: null, mn: null, mx: null, count: null });
  const paint = () => {
    const v = ro.current, has = v.count > 0, t = txt.current;
    if (t.last)  t.last.textContent  = has ? num (v.last) : '—';
    if (t.mn)    t.mn.textContent    = has ? num (v.mn) : '—';
    if (t.mx)    t.mx.textContent    = has ? num (v.mx) : '—';
    if (t.count) t.count.textContent = String (Math.round (v.count));
  };
  const bind = (k: 'last' | 'mn' | 'mx' | 'count') => (el: HTMLSpanElement | null) => { txt.current[k] = el; if (el) paint (); };
  const [paused, setPaused] = useState (false);
  const pausedRef = useRef (false);
  pausedRef.current = paused;
  const hist = useRef<number[]> ([]);
  const canvas = useRef<HTMLCanvasElement> (null);
  const accent = detectPaxTheme (d.ports ?? [], d.paxName ? _paxInfoMap.get (d.paxName)?.colourCategory : undefined).accent;

  const draw = () => {
    const c = canvas.current;
    if (! c) return;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max (1, Math.round (c.clientWidth * dpr)), h = Math.round (60 * dpr);
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    const ctx = c.getContext ('2d');
    if (! ctx) return;
    ctx.clearRect (0, 0, w, h);
    const hs = hist.current;
    if (hs.length < 2) return;
    let lo = Math.min (...hs), hi = Math.max (...hs);
    if (hi - lo < 1e-6) { lo -= 0.5; hi += 0.5; }
    const pad = (hi - lo) * 0.1; lo -= pad; hi += pad;
    const y = (v: number) => h - (v - lo) / (hi - lo) * h;
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    ctx.lineWidth = 1;
    if (lo < 0 && hi > 0) { ctx.beginPath (); ctx.moveTo (0, y (0)); ctx.lineTo (w, y (0)); ctx.stroke (); }   // zero line
    ctx.strokeStyle = getComputedStyle (c).getPropertyValue (accent.replace (/^var\((--[a-z0-9-]+)\)$/, '$1')).trim () || '#e879f9';
    ctx.lineWidth = 1.5 * dpr;
    ctx.beginPath ();
    hs.forEach ((v, k) => { const x = (k + HISTORY - hs.length) / (HISTORY - 1) * w; if (k === 0) ctx.moveTo (x, y (v)); else ctx.lineTo (x, y (v)); });
    ctx.stroke ();
  };

  useEffect (() => Bridge.onPortActivity ((entries: any[]) => {
    if (pausedRef.current) return;
    const e = entries.find (x => x.id === id);
    if (! e?.paxReadOnly) return;
    const get = (i: number) => e.paxReadOnly.find ((r: any) => r.index === i)?.value ?? 0;
    const next = { last: get (0), mn: get (1), mx: get (2), count: get (3) };
    const p = ro.current;
    if (p.last !== next.last || p.count !== next.count || p.mn !== next.mn || p.mx !== next.mx) { ro.current = next; paint (); }
    if (next.count > 0) {
      hist.current.push (next.last);
      if (hist.current.length > HISTORY) hist.current.shift ();
      draw ();
    }
  }), [id]);   // eslint-disable-line react-hooks/exhaustive-deps

  const reset = () => {
    Bridge.setPaxParameter (id, 4, 1);
    Bridge.setPaxParameter (id, 4, 0);
    hist.current = [];
    draw ();
  };
  const port = ((d.ports ?? []) as { id: string; label: string }[]).find (p => p.id.endsWith ('_in'));
  const customName: string = d.customName ?? '';

  return (
    <div style={{
      display: 'inline-block', width: 220, background: 'var(--surface)',   // fixed: a canvas sized from its own width grew the node (user: "huge")
      border: `1px solid ${selected ? accent : 'var(--border)'}`, borderTop: `3px solid ${accent}`,
      borderRadius: 'var(--radius)', fontFamily: "'JetBrains Mono', monospace", position: 'relative',
    }}>
      {port && <NodeHandle nodeId={id} portId={port.id} label={port.label} direction="in" colour="var(--value)" index={0} total={1} anchor="value" />}

      <div style={{ display: 'flex', alignItems: 'center', padding: '4px 8px', gap: 4, cursor: 'pointer',
                    borderBottom: collapsed ? 'none' : `1px solid color-mix(in srgb, ${accent} 27%, transparent)` }}
           onDoubleClick={toggleCollapsed}>
        <NodeCollapseArrow collapsed={collapsed} accent={accent} />
        <div style={{ flex: 1, minWidth: 0 }}>
          <EditableTitle display={customName || 'Value Monitor'} value={customName} placeholder="Value Monitor"
            onCommit={commitModelName (id)}
            textStyle={{ fontSize: '11px', fontWeight: 700, color: accent, letterSpacing: '0.1em', fontFamily: "'Syne', sans-serif",
                         whiteSpace: 'nowrap', textTransform: 'uppercase', userSelect: 'none' }} />
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={() => setPaused (p => ! p)} active={paused} activeAccent={accent}
            onHint={{ onMouseEnter: () => setHint ({ title: paused ? 'Resume' : 'Pause', body: 'Freezes the display (the values keep flowing through the graph).' }), onMouseLeave: () => setHint (null) }}>
            {paused ? <Play size={11} /> : <Pause size={11} />}
          </NodeHeaderButton>
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={reset}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Clear', body: 'Clears min / max / count and the graph.' }), onMouseLeave: () => setHint (null) }}>
            <RotateCcw size={11} />
          </NodeHeaderButton>
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={() => Bridge.removeNode (id)}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Delete node', body: 'Remove this node.' }), onMouseLeave: () => setHint (null) }}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {! collapsed && (
        <div style={{ padding: '8px 12px', display: 'flex', flexDirection: 'column', gap: 4 }}>
          <div data-port-anchor="value" style={{ fontSize: 22, fontWeight: 700, color: accent, textAlign: 'center', fontVariantNumeric: 'tabular-nums' }}>
            <span ref={bind ('last')} />
          </div>
          <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 8, color: 'var(--text-muted)' }}>
            <span>min <span ref={bind ('mn')} style={{ color: 'var(--text-dim)' }} /></span>
            <span>max <span ref={bind ('mx')} style={{ color: 'var(--text-dim)' }} /></span>
            <span>count <span ref={bind ('count')} style={{ color: 'var(--text-dim)' }} /></span>
          </div>
          <canvas ref={canvas} style={{ width: '100%', height: 60, display: 'block', borderRadius: 3, background: 'var(--bg)' }} />
        </div>
      )}
    </div>
  );
}
