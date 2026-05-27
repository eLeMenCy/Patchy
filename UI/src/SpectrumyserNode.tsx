// SPDX-License-Identifier: GPL-3.0-or-later
// Patchy — Spectrumyser node UI

import { useEffect, useRef, useState, useCallback, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, SpectrumSnapshot } from './Bridge';
import { NodeHandle, useNodeCollapsed, NodeHeaderButton } from './NodeUtils';
import { HintContext } from './HintPanel';
import { Settings, X } from 'lucide-react';

const ACCENT = 'var(--audio)';
const UI_BINS = 64;
const CANVAS_W = 250;
const CANVAS_H = 60;
const BAND_COLORS = ['#a78bfa', '#60a5fa', '#34d399', '#fb923c', '#f87171'];

const DEFAULT_LOW  = [20,   200,  2000,  8000, 16000];
const DEFAULT_HIGH = [200, 2000,  8000, 16000, 20000];

// Logarithmic frequency scale helpers (20Hz - 20000Hz)
const FREQ_MIN = 20, FREQ_MAX = 20000;
const toSlider  = (hz: number) =>
  Math.round((Math.log(hz / FREQ_MIN) / Math.log(FREQ_MAX / FREQ_MIN)) * 1000);
const fromSlider = (v: number) =>
  Math.round(FREQ_MIN * Math.pow(FREQ_MAX / FREQ_MIN, v / 1000));

function freqLabel(hz: number) {
  return hz >= 1000 ? `${(hz/1000).toFixed(1)}k` : `${Math.round(hz)}`;
}

// ── Spectrum canvas ───────────────────────────────────────────────────────────
function SpectrumDisplay({ mags, bands }: { mags: number[]; bands: { lo: number; hi: number }[] }) {
  const ref = useRef<HTMLCanvasElement>(null);
  const COLORS = BAND_COLORS.map(col => col + '44');

  useEffect(() => {
    const c = ref.current; if (!c) return;
    const ctx = c.getContext('2d'); if (!ctx) return;
    ctx.clearRect(0, 0, CANVAS_W, CANVAS_H);
    ctx.fillStyle = '#0a0f18';
    ctx.fillRect(0, 0, CANVAS_W, CANVAS_H);
    const minL = Math.log10(20), maxL = Math.log10(20000);
    const xOf = (hz: number) => ((Math.log10(Math.max(20,hz)) - minL) / (maxL - minL)) * CANVAS_W;
    bands.forEach((b, i) => {
      ctx.fillStyle = COLORS[i % COLORS.length];
      ctx.fillRect(xOf(b.lo), 0, xOf(b.hi) - xOf(b.lo), CANVAS_H);
    });
    const bw = CANVAS_W / UI_BINS;
    for (let i = 0; i < UI_BINS; i++) {
      const v = (mags[i] ?? 0) / 1000, bh = v * CANVAS_H;
      ctx.fillStyle = `rgb(${Math.round(52+v*200)},${Math.round(211-v*100)},80)`;
      ctx.fillRect(i*bw, CANVAS_H-bh, bw-1, bh);
    }
    ctx.strokeStyle = '#ffffff18'; ctx.lineWidth = 1;
    [100,1000,10000].forEach(f => {
      const x = xOf(f);
      ctx.beginPath(); ctx.moveTo(x,0); ctx.lineTo(x,CANVAS_H); ctx.stroke();
      ctx.fillStyle = '#ffffff44'; ctx.font = '8px monospace';
      ctx.fillText(freqLabel(f), x+2, CANVAS_H-2);
    });
  }, [mags, bands]);

  return <canvas ref={ref} width={CANVAS_W} height={CANVAS_H}
    style={{ display: 'block', borderRadius: 2 }} />;
}

// ── Main node ─────────────────────────────────────────────────────────────────
export default function SpectrumyserNode({ id, data, selected }: NodeProps) {
  const { setHint } = useContext(HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const [showSettings, setShowSettings] = useState(false);
  const [mags,      setMags]      = useState<number[]>(new Array(UI_BINS).fill(0));
  const [bands,     setBands]     = useState(() =>
    DEFAULT_LOW.slice(0,3).map((lo,i) => ({ lo, hi: DEFAULT_HIGH[i] })));
  const [bandCount, setBandCount] = useState(3);
  const [bandLow,   setBandLow]   = useState([...DEFAULT_LOW]);
  const [bandHigh,  setBandHigh]  = useState([...DEFAULT_HIGH]);

  const portBodyRef  = useRef<HTMLDivElement>(null);
  const cmdDown      = useRef(false);
  const [linked, setLinked] = useState(false);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const down = e.metaKey || e.ctrlKey;
      cmdDown.current = down;
      setLinked(down);
    };
    window.addEventListener('keydown', onKey);
    window.addEventListener('keyup',   onKey);
    return () => { window.removeEventListener('keydown', onKey); window.removeEventListener('keyup', onKey); };
  }, []);
  const ports    = (data as any)?.ports ?? [];
  const inPorts  = ports.filter((p: any) => p.type === 'audio' && p.direction === 'input');
  const outPorts = ports.filter((p: any) => p.type === 'audio' && p.direction === 'output');

  useEffect(() => Bridge.onSpectrumSnapshots((snaps: SpectrumSnapshot[]) => {
    const s = snaps.find(s => s.id === id); if (!s) return;
    setMags(s.mags);
    setBands(s.bands);
    setBandCount(s.bands.length);
    setBandLow(s.bands.map(b => b.lo));
    setBandHigh(s.bands.map(b => b.hi));
  }), [id]);

  const handleParam = useCallback((idx: number, val: number) => {
    if (idx === 0) {
      const n = Math.round(val);
      Bridge.setAddonParameter(id, idx, val);
      setBandCount(n);
      setBands(Array.from({length:n}, (_,b) => ({
        lo: bandLow[b] ?? DEFAULT_LOW[b],
        hi: bandHigh[b] ?? DEFAULT_HIGH[b],
      })));
      return;
    }

    const isLow  = (idx - 1) % 2 === 0;
    const b      = isLow ? (idx-1)/2 : (idx-2)/2;
    const linked = cmdDown.current;

    if (isLow) {
      Bridge.setAddonParameter(id, idx, val);
      setBandLow(p => { const a=[...p]; a[b]=val; return a; });
      setBands(p => p.map((bd,i) => i===b ? {...bd, lo:val} : bd));
      if (linked) {
        // Delta in slider space (log scale) so both thumbs move at equal speed
        const prevSlider = toSlider(bandLow[b] ?? DEFAULT_LOW[b]);
        const newSlider  = toSlider(val);
        const delta      = newSlider - prevSlider;
        const newHiSlider = Math.min(1000, Math.max(0, toSlider(bandHigh[b] ?? DEFAULT_HIGH[b]) + delta));
        const newHi = fromSlider(newHiSlider);
        Bridge.setAddonParameter(id, idx+1, newHi);
        setBandHigh(p => { const a=[...p]; a[b]=newHi; return a; });
        setBands(p => p.map((bd,i) => i===b ? {...bd, lo:val, hi:newHi} : bd));
      }
    } else {
      Bridge.setAddonParameter(id, idx, val);
      setBandHigh(p => { const a=[...p]; a[b]=val; return a; });
      setBands(p => p.map((bd,i) => i===b ? {...bd, hi:val} : bd));
      if (linked) {
        // Delta in slider space (log scale) so both thumbs move at equal speed
        const prevSlider = toSlider(bandHigh[b] ?? DEFAULT_HIGH[b]);
        const newSlider  = toSlider(val);
        const delta      = newSlider - prevSlider;
        const newLoSlider = Math.min(1000, Math.max(0, toSlider(bandLow[b] ?? DEFAULT_LOW[b]) + delta));
        const newLo = fromSlider(newLoSlider);
        Bridge.setAddonParameter(id, idx-1, newLo);
        setBandLow(p => { const a=[...p]; a[b]=newLo; return a; });
        setBands(p => p.map((bd,i) => i===b ? {...bd, lo:newLo, hi:val} : bd));
      }
    }
  }, [id, bandLow, bandHigh]);

  const handleReset = useCallback(() => {
    // Reset only frequency boundaries for current band count — not the count itself
    for (let b = 0; b < bandCount; b++) {
      Bridge.setAddonParameter(id, 1+b*2,   DEFAULT_LOW[b]);
      Bridge.setAddonParameter(id, 1+b*2+1, DEFAULT_HIGH[b]);
    }
    setBandLow([...DEFAULT_LOW]);
    setBandHigh([...DEFAULT_HIGH]);
    setBands(Array.from({length: bandCount}, (_,b) => ({ lo: DEFAULT_LOW[b], hi: DEFAULT_HIGH[b] })));
  }, [id, bandCount]);

  const label = (data as any)?.label ?? 'Spectrumyser';

  return (
    <div
      onMouseEnter={() => setHint({ title:'Spectrumyser', body:'FFT spectrum analyser.\nEach output port carries audio filtered to that band.' })}
      onMouseLeave={() => setHint(null)}
      style={{
        background:   'var(--surface2)',
        border:       `1px solid ${selected ? ACCENT : 'var(--border)'}`,
        borderTop:    `3px solid ${ACCENT}`,
        borderRadius: 'var(--radius)',
        minWidth:     248,
        fontFamily:   "'JetBrains Mono', monospace",
        boxShadow:    selected
          ? `0 0 0 1px ${ACCENT}, 0 8px 32px var(--audio-glow, #22c55e33)`
          : '0 4px 16px rgba(0,0,0,.5)',
        transition:   'box-shadow .15s, border-color .15s',
        position:     'relative',
      }}
    >
      {/* IN port — centred on canvas via portBodyRef */}
      {inPorts.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour={ACCENT} portBodyRef={portBodyRef} offset={CANVAS_H / 2 - 12} />
      ))}

      {/* OUT ports — use portBodyRef so they merge to centre when collapsed */}
      {outPorts.map((p: any, i: number) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour={BAND_COLORS[i] ?? ACCENT} index={i} total={outPorts.length}
          portBodyRef={portBodyRef} offset={4} />
      ))}

      {/* Header — stopPropagation on buttons prevents double-click fold */}
      <div style={{
        display: 'flex', alignItems: 'center',
        padding: '4px 8px', gap: 4,
        background: `${ACCENT}18`,
        borderBottom: collapsed ? 'none' : `1px solid ${ACCENT}44`,
        cursor: 'pointer',
      }}
        onDoubleClick={toggleCollapsed}
      >
        <NodeHeaderButton onClick={toggleCollapsed}
          onHint={{ onMouseEnter: ()=>{}, onMouseLeave: ()=>{} }}>
          <span style={{ color:ACCENT, opacity:0.7, display:'inline-block',
            transform: collapsed ? 'rotate(0deg)' : 'rotate(90deg)',
            transition:'transform 0.2s' }}>
            <svg width="8" height="10" viewBox="0 0 8 10" style={{ display:'block' }}>
              <polygon points="0,0 8,5 0,10" fill="currentColor" />
            </svg>
          </span>
        </NodeHeaderButton>

        <div style={{ flex:1, fontSize:'11px', fontWeight:700, color:ACCENT,
          letterSpacing:'0.1em', fontFamily:"'Syne', sans-serif",
          whiteSpace:'nowrap', textTransform:'uppercase', userSelect:'none' }}>
          {label}
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton
            onClick={() => setShowSettings(v => !v)}
            onHint={{ onMouseEnter: () => setHint({title:'Band Settings',body:'Configure frequency bands.'}), onMouseLeave: () => setHint(null) }}>
            <span style={{
              display:'flex', alignItems:'center', justifyContent:'center',
              border: showSettings ? `1px solid ${ACCENT}` : '1px solid transparent',
              borderRadius:3, padding:'1px',
            }}>
              <Settings size={11} color={showSettings ? ACCENT : 'var(--text-muted)'} />
            </span>
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton
            onClick={() => Bridge.removeNode(id)}
            onHint={{ onMouseEnter: () => setHint({title:'Delete node',body:'Remove this node.'}), onMouseLeave: () => setHint(null) }}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {!collapsed && (
        <div ref={portBodyRef} style={{ padding: '6px 8px 8px' }}>
          <SpectrumDisplay mags={mags} bands={bands} />
        </div>
      )}

      {/* Settings panel */}
      {showSettings && !collapsed && (
        <div style={{ padding:'8px 10px', fontSize:9, color:'var(--text)',
          borderTop:`1px solid var(--border)` }}>
          {/* Band count + reset */}
          <div className="nodrag" style={{ display:'flex', alignItems:'center', gap:6, marginBottom:6 }}>
            <span style={{ color:'var(--text-muted)', minWidth:40 }}>Bands</span>
            {[1,2,3,4,5].map(n => (
              <div key={n} className="nodrag" onClick={() => handleParam(0,n)} style={{
                width:18, height:18, display:'flex', alignItems:'center', justifyContent:'center',
                cursor:'pointer', borderRadius:2,
                background: n===bandCount ? ACCENT : 'var(--surface)',
                color: n===bandCount ? '#000' : 'var(--text-muted)',
                fontWeight: n===bandCount ? 700 : 400, fontSize:10,
              }}>{n}</div>
            ))}
            <div onDoubleClick={e => e.stopPropagation()}>
              <NodeHeaderButton onClick={handleReset}
                onHint={{ onMouseEnter: () => setHint({title:'Reset',body:'Reset all bands to defaults.'}), onMouseLeave: () => setHint(null) }}>
                <span style={{ fontSize:11, fontWeight:700 }}>R</span>
              </NodeHeaderButton>
            </div>
          </div>

          {/* Sliders — nodrag stops node from moving */}
          {Array.from({length: bandCount}, (_,b) => (
            <div key={b} className="nodrag"
              onMouseDown={e => e.stopPropagation()}
              style={{ display:'flex', alignItems:'center', gap:4, marginBottom:4 }}>
              <span style={{ color: BAND_COLORS[b], minWidth:24, fontWeight:700 }}>B{b+1}</span>
              <input type="range" min={0} max={1000} step={1}
                value={toSlider(bandLow[b] ?? DEFAULT_LOW[b])}
                className="nodrag"
                onMouseDown={e => e.stopPropagation()}
                onDoubleClick={e => { e.stopPropagation(); handleParam(1+b*2, DEFAULT_LOW[b]); }}
                onChange={e => handleParam(1+b*2, fromSlider(parseInt(e.target.value)))}
                style={{ width:70, ['--thumb-color' as any]: BAND_COLORS[b] }} />
              <span style={{ minWidth:28, color:ACCENT }}>{freqLabel(bandLow[b] ?? DEFAULT_LOW[b])}</span>
              <span style={{ color: linked ? '#fbbf24' : 'var(--text-muted)', fontSize: linked ? 11 : 9, transition: 'all 0.1s' }}>{linked ? '⇔' : '→'}</span>
              <input type="range" min={0} max={1000} step={1}
                value={toSlider(bandHigh[b] ?? DEFAULT_HIGH[b])}
                className="nodrag"
                onMouseDown={e => e.stopPropagation()}
                onDoubleClick={e => { e.stopPropagation(); handleParam(1+b*2+1, DEFAULT_HIGH[b]); }}
                onChange={e => handleParam(1+b*2+1, fromSlider(parseInt(e.target.value)))}
                style={{ width:70, ['--thumb-color' as any]: BAND_COLORS[b] }} />
              <span style={{ minWidth:32, color:ACCENT }}>{freqLabel(bandHigh[b] ?? DEFAULT_HIGH[b])}</span>
            </div>
          ))}
          {linked && (
            <div style={{ padding:'2px 0 4px', fontSize:8, color:'#fbbf24', textAlign:'center' }}>
              ⌘/Ctrl held — sliders linked
            </div>
          )}
        </div>
      )}
    </div>
  );
}
