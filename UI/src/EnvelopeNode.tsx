// SPDX-License-Identifier: GPL-3.0-or-later
// Patchy — Envelope node UI

import { useEffect, useRef, useState, useCallback, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, NodeHeaderButton, NodeCollapseArrow } from './NodeUtils';
import { HintContext } from './HintPanel';
import { Settings, X } from 'lucide-react';

// Envelope is an AV node — use the AV colour
const ACCENT   = 'var(--av)';
const CANVAS_W = 250;
const CANVAS_H = 48;

// Log scale helpers (same as Spectrumyser)
const FREQ_MIN = 20, FREQ_MAX = 20000;
const toSlider   = (hz: number) =>
  Math.round((Math.log(hz / FREQ_MIN) / Math.log(FREQ_MAX / FREQ_MIN)) * 1000);
const fromSlider = (v: number) =>
  Math.round(FREQ_MIN * Math.pow(FREQ_MAX / FREQ_MIN, v / 1000));
const freqLabel  = (hz: number) =>
  hz >= 1000 ? `${(hz / 1000).toFixed(1)}k` : `${Math.round(hz)}`;

// ── Envelope display canvas ───────────────────────────────────────────────────
function EnvelopeDisplay({ ccValue, attack, release }: { ccValue: number; attack: number; release: number }) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const history   = useRef<number[]>(new Array(CANVAS_W).fill(0));

  useEffect(() => {
    history.current.push(ccValue / 127);
    if (history.current.length > CANVAS_W) history.current.shift();

    const c = canvasRef.current; if (!c) return;
    const ctx = c.getContext('2d'); if (!ctx) return;
    ctx.clearRect(0, 0, CANVAS_W, CANVAS_H);
    ctx.fillStyle = '#0a0f18';
    ctx.fillRect(0, 0, CANVAS_W, CANVAS_H);

    // Grid line at 50%
    ctx.strokeStyle = '#ffffff12'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(0, CANVAS_H/2); ctx.lineTo(CANVAS_W, CANVAS_H/2); ctx.stroke();

    // Ghost envelope shape (attack + sustain + release)
    const totalMs  = attack + 200 + release; // attack + sustain plateau + release
    const atkX     = (attack / totalMs) * CANVAS_W;
    const relX     = CANVAS_W - (release / totalMs) * CANVAS_W;
    ctx.strokeStyle = '#fb923c33';
    ctx.fillStyle   = '#fb923c0a';
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    ctx.moveTo(0,    CANVAS_H);          // start bottom-left
    ctx.lineTo(atkX, 4);                 // attack up
    ctx.lineTo(relX, 4);                 // sustain plateau
    ctx.lineTo(CANVAS_W, CANVAS_H);      // release down
    ctx.closePath();
    ctx.fill();
    ctx.stroke();

    // Envelope curve
    const grad = ctx.createLinearGradient(0, 0, 0, CANVAS_H);
    grad.addColorStop(0, '#fb923c');
    grad.addColorStop(1, '#fb923c22');
    ctx.fillStyle   = grad;
    ctx.strokeStyle = '#fb923c';
    ctx.lineWidth   = 1.5;
    ctx.beginPath();
    history.current.forEach((v, i) => {
      const x = i, y = CANVAS_H - v * CANVAS_H;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.lineTo(CANVAS_W, CANVAS_H);
    ctx.lineTo(0, CANVAS_H);
    ctx.closePath();
    ctx.fill();

    ctx.beginPath();
    history.current.forEach((v, i) => {
      const x = i, y = CANVAS_H - v * CANVAS_H;
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();

    // Current CC value label
    ctx.fillStyle = '#ffffffcc';
    ctx.font = '9px monospace';
    ctx.fillText(`CC ${Math.round(ccValue)}`, 4, 12);

  }, [ccValue]);

  return <canvas ref={canvasRef} width={CANVAS_W} height={CANVAS_H}
    style={{ display: 'block', borderRadius: 2 }} />;
}

// ── Slider row ────────────────────────────────────────────────────────────────
function SliderRow({ label, value, min, max, step = 0, format, onChange, onDoubleClick, color }: {
  label:         string;
  value:         number;
  min:           number;
  max:           number;
  step?:         number;
  format:        (v: number) => string;
  onChange:      (v: number) => void;
  onDoubleClick: () => void;
  color?:        string;
}) {
  return (
    <div className="nodrag" onMouseDown={e => e.stopPropagation()}
      style={{ display:'flex', alignItems:'center', gap:4, marginBottom:3 }}>
      <span style={{ color:'var(--text-muted)', minWidth:60, fontSize:8 }}>{label}</span>
      <input type="range" min={min} max={max} step={step === 0 ? 'any' : step}
        value={value} className="nodrag"
        onMouseDown={e => e.stopPropagation()}
        onDoubleClick={e => { e.stopPropagation(); onDoubleClick(); }}
        onChange={e => onChange(parseFloat(e.target.value))}
        style={{ flex:1, ['--thumb-color' as any]: color ?? ACCENT }} />
      <span style={{ minWidth:38, color: color ?? ACCENT, fontSize:8, textAlign:'right' }}>
        {format(value)}
      </span>
    </div>
  );
}

// ── Number stepper ────────────────────────────────────────────────────────────
function Stepper({ label, value, min, max, onChange }: {
  label:    string;
  value:    number;
  min:      number;
  max:      number;
  onChange: (v: number) => void;
}) {
  const [editing, setEditing] = useState(false);
  const [draft,   setDraft]   = useState('');
  const dragStart = useRef<{ y: number; v: number } | null>(null);

  const commitDraft = () => {
    const n = parseInt(draft, 10);
    if (!isNaN(n)) onChange(Math.max(min, Math.min(max, n)));
    setEditing(false);
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (!(e.metaKey || e.ctrlKey)) return;
    e.preventDefault();
    dragStart.current = { y: e.clientY, v: value };
    const onMove = (ev: MouseEvent) => {
      if (!dragStart.current) return;
      const delta = Math.round((dragStart.current.y - ev.clientY) / 3);
      onChange(Math.max(min, Math.min(max, dragStart.current.v + delta)));
    };
    const onUp = () => {
      dragStart.current = null;
      window.removeEventListener('mousemove', onMove);
      window.removeEventListener('mouseup',   onUp);
    };
    window.addEventListener('mousemove', onMove);
    window.addEventListener('mouseup',   onUp);
  };

  return (
    <div className="nodrag" onDoubleClick={e => e.stopPropagation()}
      style={{ display:'flex', alignItems:'center', gap:4, marginBottom:3 }}>
      <span style={{ color:'var(--text-muted)', minWidth:60, fontSize:8 }}>{label}</span>
      <div style={{ display:'flex', alignItems:'center', gap:3 }}>
        <div onClick={() => onChange(Math.max(min, value-1))}
          onDoubleClick={e => e.stopPropagation()} style={{
          width:16, height:16, display:'flex', alignItems:'center', justifyContent:'center',
          background:'var(--surface)', borderRadius:2, cursor:'pointer', fontSize:10,
          color:'var(--text-muted)', userSelect:'none',
        }}>−</div>

        {editing ? (
          <input autoFocus type="number" value={draft}
            onChange={e => setDraft(e.target.value)}
            onBlur={commitDraft}
            onKeyDown={e => {
              if (e.key === 'Enter') commitDraft();
              if (e.key === 'Escape') setEditing(false);
              e.stopPropagation();
            }}
            style={{
              width:36, textAlign:'center', fontSize:9,
              background:'var(--surface)', color:ACCENT,
              border:`1px solid ${ACCENT}`, borderRadius:2,
              outline:'none', padding:'1px 2px',
              fontFamily:"'JetBrains Mono', monospace",
            }} />
        ) : (
          <span
            onDoubleClick={e => { e.stopPropagation(); setDraft(String(value)); setEditing(true); }}
            onMouseDown={handleMouseDown}
            title="Double-click to type • ⌘/Ctrl+drag to scrub"
            style={{
              minWidth:36, textAlign:'center', fontSize:9, color:ACCENT,
              cursor:'ns-resize', userSelect:'none', padding:'1px 2px',
              borderRadius:2, border:'1px solid transparent',
            }}>
            {value}
          </span>
        )}

        <div onClick={() => onChange(Math.min(max, value+1))}
          onDoubleClick={e => e.stopPropagation()} style={{
          width:16, height:16, display:'flex', alignItems:'center', justifyContent:'center',
          background:'var(--surface)', borderRadius:2, cursor:'pointer', fontSize:10,
          color:'var(--text-muted)', userSelect:'none',
        }}>+</div>
      </div>
    </div>
  );
}

// ── Main node ─────────────────────────────────────────────────────────────────
export default function EnvelopeNode({ id, data, selected }: NodeProps) {
  const { setHint }  = useContext(HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const [showSettings, setShowSettings] = useState(false);

  // Parameters
  const [mode,       setMode]       = useState(0);    // 0=Amplitude, 1=Spectral
  const [ccNumber,   setCcNumber]   = useState(11);
  const [midiCh,     setMidiCh]     = useState(1);
  const [attack,     setAttack]     = useState(10);
  const [release,    setRelease]    = useState(200);
  const [sensitivity,setSensitivity]= useState(1.0);
  const [bandLow,    setBandLow]    = useState(200);
  const [bandHigh,   setBandHigh]   = useState(2000);

  // Live CC value from port activity
  const [ccValue, setCcValue] = useState(0);
  const portBodyRef = useRef<HTMLDivElement>(null);

  // Restore params from addonParams in node data
  useEffect(() => {
    // addonParams contains parameter definitions (name/min/max/default)
    // Use defaultValue to initialise — actual values come via Bridge.setAddonParameter
    const params: any[] = (data as any)?.addonParams ?? [];
    params.forEach((p: any, i: number) => {
      const v = p.defaultValue ?? 0;
      try {
        switch (i) {
          case 0: setMode(Math.round(v));        break;
          case 1: setCcNumber(Math.round(v));    break;
          case 2: setMidiCh(Math.round(v));      break;
          case 3: setAttack(v);                  break;
          case 4: setRelease(v);                 break;
          case 5: setSensitivity(v);             break;
          case 6: setBandLow(v);                 break;
          case 7: setBandHigh(v);                break;
        }
      } catch {}
    });
  }, []);

  // Subscribe to port activity for live CC value (via MIDI out RMS)
  useEffect(() => {
    return Bridge.onPortActivity((entries: any[]) => {
      const e = entries.find((e: any) => e.id === id);
      if (e) setCcValue(Math.round((Math.max(e.l ?? 0, e.r ?? 0) / 1000) * 127));
    });
  }, [id]);

  const handleReset = useCallback(() => {
    setParam(0, 0);     // Mode: Amplitude
    setParam(1, 11);    // CC: 11
    setParam(2, 1);     // MIDI Ch: 1
    setParam(3, 10);    // Attack: 10ms
    setParam(4, 200);   // Release: 200ms
    setParam(5, 1.0);   // Sensitivity: ×1.0
    setParam(6, 200);   // Band Low: 200Hz
    setParam(7, 2000);  // Band High: 2000Hz
  }, []);

  const setParam = useCallback((idx: number, val: number) => {
    Bridge.setAddonParameter(id, idx, val);
    switch (idx) {
      case 0: setMode(Math.round(val));       break;
      case 1: setCcNumber(Math.round(val));   break;
      case 2: setMidiCh(Math.round(val));     break;
      case 3: setAttack(val);                 break;
      case 4: setRelease(val);                break;
      case 5: setSensitivity(val);            break;
      case 6: setBandLow(val);                break;
      case 7: setBandHigh(val);               break;
    }
  }, [id]);

  const ports    = (data as any)?.ports ?? [];
  const inAudio  = ports.filter((p: any) => p.type === 'audio' && p.direction === 'input');
  const outAudio = ports.filter((p: any) => p.type === 'audio' && p.direction === 'output');
  const inMidi   = ports.filter((p: any) => p.type === 'midi'  && p.direction === 'input');
  const outMidi  = ports.filter((p: any) => p.type === 'midi'  && p.direction === 'output');
  const label    = (data as any)?.label ?? 'Envelope';

  return (
    <div
      onMouseEnter={() => setHint({ title:'Envelope', body:'Converts audio amplitude or a frequency band into a MIDI CC stream.\nIdeal for driving automation, LED controllers or modulation.' })}
      onMouseLeave={() => setHint(null)}
      style={{
        background:   'var(--surface2)',
        border:       `1px solid ${selected ? ACCENT : 'var(--border)'}`,
        borderTop:    `3px solid ${ACCENT}`,
        borderRadius: 'var(--radius)',
        minWidth:     280,
        fontFamily:   "'JetBrains Mono', monospace",
        boxShadow:    selected
          ? `0 0 0 1px ${ACCENT}, 0 8px 32px #fb923c33`
          : '0 4px 16px rgba(0,0,0,.5)',
        transition:   'box-shadow .15s, border-color .15s',
        position:     'relative',
      }}
    >
      {/* IN ports — all anchored to canvas via portBodyRef so they stay together */}
      {inAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={CANVAS_H / 2 - 18} />
      ))}
      {inMidi.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour="var(--midi)" portBodyRef={portBodyRef} offset={CANVAS_H / 2 - 4} />
      ))}

      {/* OUT ports — all anchored to canvas via portBodyRef so they stay together */}
      {outAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={CANVAS_H / 2 - 18} />
      ))}
      {outMidi.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--midi)" portBodyRef={portBodyRef} offset={CANVAS_H / 2 - 4} />
      ))}

      {/* Header */}
      <div style={{
        display:'flex', alignItems:'center', padding:'4px 8px', gap:4,
        background:`${ACCENT}18`,
        borderBottom: collapsed ? 'none' : `1px solid ${ACCENT}44`,
        cursor:'pointer',
      }} onDoubleClick={toggleCollapsed}>

        <NodeHeaderButton onClick={toggleCollapsed}
          onHint={{ onMouseEnter:()=>{}, onMouseLeave:()=>{} }}>
          <NodeCollapseArrow collapsed={collapsed} accent={ACCENT} />
        </NodeHeaderButton>

        <div style={{ flex:1, fontSize:'11px', fontWeight:700, color:ACCENT,
          letterSpacing:'0.1em', fontFamily:"'Syne', sans-serif",
          textTransform:'uppercase', userSelect:'none' }}>
          {label}
        </div>

        {/* Mode badge */}
        <div className="nodrag" onClick={e => { e.stopPropagation(); setParam(0, mode === 0 ? 1 : 0); }}
          style={{
            fontSize:8, padding:'2px 5px', borderRadius:2, cursor:'pointer',
            background: mode === 1 ? ACCENT : 'var(--surface)',
            color:      mode === 1 ? '#000' : 'var(--text-muted)',
            fontWeight:700, userSelect:'none',
          }}>
          {mode === 0 ? 'AMP' : 'BAND'}
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={handleReset}
            onHint={{ onMouseEnter: () => setHint({title:'Reset',body:'Reset all parameters to defaults.'}), onMouseLeave: () => setHint(null) }}>
            <span style={{ fontSize:11, fontWeight:700 }}>R</span>
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={() => setShowSettings(v => !v)}
            onHint={{ onMouseEnter: () => setHint({title:'Settings',body:'Configure envelope parameters.'}), onMouseLeave: () => setHint(null) }}>
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
          <NodeHeaderButton onClick={() => Bridge.removeNode(id)}
            onHint={{ onMouseEnter: () => setHint({title:'Delete node',body:'Remove this node.'}), onMouseLeave: () => setHint(null) }}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {/* Body */}
      {!collapsed && (
        <div ref={portBodyRef} style={{ padding:'6px 8px 4px' }}>
          <EnvelopeDisplay ccValue={ccValue} attack={attack} release={release} />
        </div>
      )}

      {/* Settings */}
      {showSettings && !collapsed && (
        <div style={{ padding:'8px 10px', fontSize:9, color:'var(--text)',
          borderTop:`1px solid var(--border)` }}>

          <SliderRow label="Attack"      value={attack}      min={1}    max={500}  step={1}
            format={v => `${Math.round(v)}ms`} onChange={v => setParam(3,v)} color={ACCENT}
            onDoubleClick={() => setParam(3, 10)} />
          <SliderRow label="Release"     value={release}     min={1}    max={2000} step={1}
            format={v => `${Math.round(v)}ms`} onChange={v => setParam(4,v)} color={ACCENT}
            onDoubleClick={() => setParam(4, 200)} />
          <SliderRow label="Sensitivity" value={sensitivity} min={0.1}  max={4}    step={0}
            format={v => `×${v.toFixed(2)}`}  onChange={v => setParam(5,v)} color={ACCENT}
            onDoubleClick={() => setParam(5, 1.0)} />

          <div style={{ borderTop:'1px solid var(--border)', marginTop:4, paddingTop:4 }}>
            <Stepper label="CC Number" value={ccNumber} min={0} max={127}
              onChange={v => setParam(1, v)} />
            <Stepper label="MIDI Ch"   value={midiCh}   min={1} max={16}
              onChange={v => setParam(2, v)} />
          </div>

          {/* Band filters — only in Spectral mode */}
          {mode === 1 && (
            <div style={{ borderTop:'1px solid var(--border)', marginTop:4, paddingTop:4 }}>
              <div style={{ color:'var(--text-muted)', fontSize:8, marginBottom:4 }}>Band Filter</div>
              <SliderRow label="Low"  value={toSlider(bandLow)}  min={0} max={1000} step={1}
                format={() => freqLabel(bandLow)}
                onChange={v => setParam(6, fromSlider(v))} color="var(--audio)"
                onDoubleClick={() => setParam(6, 200)} />
              <SliderRow label="High" value={toSlider(bandHigh)} min={0} max={1000} step={1}
                format={() => freqLabel(bandHigh)}
                onChange={v => setParam(7, fromSlider(v))} color="var(--audio)"
                onDoubleClick={() => setParam(7, 2000)} />
            </div>
          )}
        </div>
      )}
    </div>
  );
}
