// Patchy — Audio to DMX node UI

import { useEffect, useRef, useState, useCallback, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, NodeHeaderButton, NodeCollapseArrow, nodeContainerStyle, settingsPanelStyle, sectionDividerStyle, _paxInfoMap, detectPaxTheme } from './NodeUtils';
import { HintContext } from './HintPanel';
import { Settings, X } from 'lucide-react';

// Persist settings panel open state across graph updates (survives undo/redo)
const _settingsOpen = new Map<string, boolean>();

const FREQ_MIN = 20, FREQ_MAX = 20000;
const freqLabel = (hz: number) =>
  hz >= 1000 ? `${(hz / 1000).toFixed(2)}k` : `${Math.round(hz)}`;

// Param indices — must match AudioToDmxPax.cpp's PAX_getParameterInfo,
// plus two UI-only entries (5, 6) that never go to the backend at all —
// see the settingsJson-restore effect below for why they're still part of
// the same persisted array.
const IDX_MODE = 0, IDX_SENS = 1, IDX_BANDLOW = 2, IDX_BANDHIGH = 3, IDX_CHANNEL = 4, IDX_DAMPING = 5;

// Resolve a CSS custom property reference (e.g. "var(--value)", which is
// exactly what theme.accent returns for this Pax's auto-detected Converter
// category) to its actual literal colour. Needed because Canvas 2D's
// fillStyle/strokeStyle cannot interpret var() at all — canvas operates
// outside the DOM's computed-style cascade. Passing "var(--value)" (or a
// concatenated "var(--value)55" for alpha) doesn't throw, it just silently
// fails and keeps whatever fillStyle was already set — defaulting to
// black — which is exactly why the waveform drew nothing: every *ordinary*
// DOM use of `color` (port dots, plain 1px borders) resolves fine through
// normal CSS, which is what made this so easy to miss. EnvelopeDisplay
// sidesteps this entirely by hardcoding a literal hex for its own canvas
// work instead of using its CSS var(); this resolves dynamically instead
// of hardcoding a second copy of the colour that could drift from
// index.css over time.
function resolveCssColor(value: string): string {
  if (!value.startsWith('var(')) return value;
  const varName = value.slice(4, value.indexOf(')')).split(',')[0].trim();
  const resolved = getComputedStyle(document.documentElement).getPropertyValue(varName).trim();
  return resolved || '#e879f9'; // fallback: Converter's own fixed hex (--value in index.css)
}

// ── Slider row — label on the left, matching EnvelopeNode's layout ──────────
function SliderRow({ label, value, min, max, step = 0, format, onChange, onDoubleClick, onCommit, color }: {
  label:         string;
  value:         number;
  min:           number;
  max:           number;
  step?:         number;
  format:        (v: number) => string;
  onChange:      (v: number) => void;
  onDoubleClick: () => void;
  onCommit?:     () => void;
  color:         string;
}) {
  return (
    <div className="nodrag" onMouseDown={e => e.stopPropagation()}
      style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 3 }}>
      <span style={{ color: 'var(--text-muted)', minWidth: 60, fontSize: 8 }}>{label}</span>
      <input type="range" min={min} max={max} step={step === 0 ? 'any' : step}
        value={value} className="nodrag"
        onMouseDown={e => e.stopPropagation()}
        onMouseUp={onCommit} onKeyUp={onCommit}
        onDoubleClick={e => { e.stopPropagation(); onDoubleClick(); onCommit?.(); }}
        onChange={e => onChange(parseFloat(e.target.value))}
        style={{ flex: 1, ['--thumb-color' as any]: color }} />
      <span style={{ minWidth: 42, color, fontSize: 8, textAlign: 'right' }}>
        {format(value)}
      </span>
    </div>
  );
}

// ── Number stepper — identical control to EnvelopeNode's CC Number/MIDI Ch ──
function Stepper({ label, value, min, max, width = 28, color, buttons = false, onChange }: {
  label:    string;
  value:    number;
  min:      number;
  max:      number;
  width?:   number;
  color:    string;
  buttons?: boolean; // +/- buttons — off by default (compact label-only, for the Zoom Steppers flanking the narrow track), on for DMX Channel where there's room and they were preferred
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

  // Drag-to-scrub — plain click+drag, no modifier key needed. A stationary
  // click (mousedown+mouseup with no movement) naturally produces delta=0,
  // so this can't be triggered accidentally by an ordinary click; it only
  // does something once the mouse actually moves vertically.
  const handleMouseDown = (e: React.MouseEvent) => {
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
      style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 3 }}>
      {label && <span style={{ color: 'var(--text-muted)', minWidth: 60, fontSize: 8 }}>{label}</span>}

      <div style={{ display: 'flex', alignItems: 'center', gap: buttons ? 3 : 0 }}>
        {buttons && (
          <div onClick={() => onChange(Math.max(min, value - 1))}
            onDoubleClick={e => e.stopPropagation()} style={{
            width: 16, height: 16, display: 'flex', alignItems: 'center', justifyContent: 'center',
            background: 'var(--surface)', borderRadius: 2, cursor: 'pointer', fontSize: 10,
            color: 'var(--text-muted)', userSelect: 'none',
          }}>−</div>
        )}

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
              width, textAlign: 'center', fontSize: 9,
              background: 'var(--surface)', color,
              border: `1px solid ${color}`, borderRadius: 2,
              outline: 'none', padding: '1px 2px',
              fontFamily: "'JetBrains Mono', monospace",
            }} />
        ) : (
          <span
            onDoubleClick={e => { e.stopPropagation(); setDraft(String(value)); setEditing(true); }}
            onMouseDown={handleMouseDown}
            title="Double-click to type • drag up/down to scrub"
            style={{
              minWidth: width, textAlign: 'center', fontSize: 9, color,
              cursor: 'ns-resize', userSelect: 'none', padding: '1px 2px',
              borderRadius: 2, border: `1px solid ${color}44`, background: 'var(--surface)',
            }}>
            {value}
          </span>
        )}

        {buttons && (
          <div onClick={() => onChange(Math.min(max, value + 1))}
            onDoubleClick={e => e.stopPropagation()} style={{
            width: 16, height: 16, display: 'flex', alignItems: 'center', justifyContent: 'center',
            background: 'var(--surface)', borderRadius: 2, cursor: 'pointer', fontSize: 10,
            color: 'var(--text-muted)', userSelect: 'none',
          }}>+</div>
        )}
      </div>
    </div>
  );
}

// ── Freq Band dual-handle slider ─────────────────────────────────────────────
// One slider, two heads (Band Low / Band High — the actual values sent to
// the backend), plus a Stepper at each extremity that narrows the slider's
// own min/max span (a "zoom" window) rather than always spanning the full
// 20Hz-20kHz range — makes fine adjustment of a narrow band actually usable.
// Two native <input type="range"> stacked via the .dual-range CSS trick
// (index.css) rather than a from-scratch pointer-tracking widget, matching
// how every other slider in this codebase is a styled native range input.
// Each head double-click-resets to its own default (200Hz / 2000Hz),
// matching the same convention Sensitivity's slider already uses — those
// two numbers are hardcoded here rather than imported, same values as the
// top-level `defaults` array in the settingsJson-restore effect (index 2/3
// there); keep both in sync if the defaults ever change.
function FreqBandDual({ bandLow, bandHigh, zoomMin, zoomMax, color, onBandChange, onZoomChange, onCommit }: {
  bandLow:      number;
  bandHigh:     number;
  zoomMin:      number;
  zoomMax:      number;
  color:        string;
  onBandChange: (which: 'low' | 'high', v: number) => void;
  onZoomChange: (which: 'min' | 'max', v: number) => void;
  onCommit:     () => void;
}) {
  // Wrapped as one bordered "card" (item 6: was two loose-looking rows with
  // no visual grouping) so the track and its Zoom controls read as a
  // single unit rather than two unrelated sliders stacked by coincidence.
  return (
    <div className="nodrag" onMouseDown={e => e.stopPropagation()} style={{
      marginBottom: 3, background: 'var(--surface)', border: '1px solid var(--border)',
      borderRadius: 3, padding: '4px 6px 5px',
    }}>
      {/* Track + Steppers, one row — Steppers vertically aligned with the
          track rather than in a separate row below (this does narrow the
          track itself, same trade-off that led to splitting them apart a
          few sessions ago — reintroduced deliberately, per request).
          Labels float above each handle, positioned OUTSIDE it (left
          label ending just left, right label starting just right) rather
          than centred on it — same treatment as the Band Low/High labels
          on the BandDisplay graphic above. */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 4, marginTop: 10 }}>
        <Stepper label="" value={zoomMin} min={FREQ_MIN} max={zoomMax - 10} width={26} color={color}
          onChange={v => onZoomChange('min', v)} />
        <div className="dual-range" style={{ position: 'relative', flex: 1, height: 16 }}>
          <span style={{
            position: 'absolute', top: -10,
            left: `${((bandLow - zoomMin) / (zoomMax - zoomMin)) * 100}%`,
            transform: 'translateX(calc(-100% - 3px))', fontSize: 8, color, whiteSpace: 'nowrap',
          }}>{freqLabel(bandLow)}</span>
          <span style={{
            position: 'absolute', top: -10,
            left: `${((bandHigh - zoomMin) / (zoomMax - zoomMin)) * 100}%`,
            transform: 'translateX(5px)', fontSize: 8, color, whiteSpace: 'nowrap',
          }}>{freqLabel(bandHigh)}</span>
          <input type="range" min={zoomMin} max={zoomMax} step={1}
            value={bandLow}
            onMouseUp={onCommit} onKeyUp={onCommit}
            onChange={e => onBandChange('low', Number(e.target.value))}
            onDoubleClick={e => { e.stopPropagation(); onBandChange('low', 200); onCommit(); }}
            style={{ width: '100%', top: 5, left: 0, ['--thumb-color' as any]: color }} />
          <input type="range" min={zoomMin} max={zoomMax} step={1}
            value={bandHigh}
            onMouseUp={onCommit} onKeyUp={onCommit}
            onChange={e => onBandChange('high', Number(e.target.value))}
            onDoubleClick={e => { e.stopPropagation(); onBandChange('high', 2000); onCommit(); }}
            style={{ width: '100%', top: 5, left: 0, ['--thumb-color' as any]: color }} />
        </div>
        <Stepper label="" value={zoomMax} min={zoomMin + 10} max={FREQ_MAX} width={34} color={color}
          onChange={v => onZoomChange('max', v)} />
      </div>
    </div>
  );
}

// ── Band display — small always-visible graphic, not inside the foldable
// settings panel. Redesigned per eLeMenCy's reference image + brief
// ("Bislider_canvas_-_brief.odt"/"...Pict.jpeg", 2026-07-31): a unified
// frequency-axis graphic. Back to front:
//   1. Wave — RMS mode only. A glow/blur version (matching the reference
//      image) was tried across both modes and then removed entirely —
//      "confusing, didn't earn its place" plus a stutter. Brought back in
//      a deliberately different, narrower form: the earlier SHARP style
//      (solid fill+stroke in the node's own accent colour, no shadowBlur),
//      shown only in RMS mode — which otherwise has nothing else to look
//      at, since the Zoom/Band tinting and FILTER curve's shaping are both
//      FREQ-mode-only. Amplitude is the real sensitivity gain applied to
//      level (level × 10^(dB/20), the exact formula the backend uses
//      before clamping to a DMX byte), same as the removed version — an
//      honest readout, not decoration.
//   2. Zoom window / active Band tinting — FREQ mode only.
//   3. FILTER curve — a stylised decorative bump centred on the active
//      Band (not the real analytic bandpass response — deliberately kept
//      simple, per discussion), flat in RMS mode since there's no shaping
//      to show. Purely illustrative, same spirit as the reference image's
//      "FILTER" curve.
//   4. Sensitivity line — crisp, on top of everything, unchanged mapping
//      (-20..+60dB → bottom..top).
// Giving the body real, constant height also fixes ports rendering outside
// the node when the settings panel is folded (see port offset comments
// at the call site).
const DISPLAY_H = 44;
const BAND_DISPLAY_W = 260;
function BandDisplay({ mode, bandLow, bandHigh, zoomMin, zoomMax, sensitivityDb, level, color }: {
  mode:          number;
  bandLow:       number;
  bandHigh:      number;
  zoomMin:       number;
  zoomMax:       number;
  sensitivityDb: number;
  level:         number; // 0-1, live audio level from port activity
  color:         string;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const history    = useRef<number[]>(new Array(BAND_DISPLAY_W).fill(0));

  // Resolved once per render — see resolveCssColor's comment above for why
  // this is required before anything reaches canvas or a concatenated
  // background string.
  const hexColor = resolveCssColor(color);

  const toX = (hz: number) => {
    const clamped = Math.max(FREQ_MIN, Math.min(FREQ_MAX, hz));
    return (Math.log(clamped / FREQ_MIN) / Math.log(FREQ_MAX / FREQ_MIN)) * BAND_DISPLAY_W;
  };
  const zoomX1 = toX(zoomMin), zoomX2 = toX(zoomMax);
  const bandX1 = toX(bandLow), bandX2 = toX(bandHigh);

  useEffect(() => {
    // Real sensitivity gain — same formula as AudioToDmxPax.cpp's
    // sensitivityGain(), not a separate display-only curve.
    const sensGain = Math.pow(10, sensitivityDb / 20);
    const gained = Math.min(1, level * sensGain);
    history.current.push(gained);
    if (history.current.length > BAND_DISPLAY_W) history.current.shift();

    const c = canvasRef.current; if (!c) return;
    const ctx = c.getContext('2d'); if (!ctx) return;
    ctx.clearRect(0, 0, BAND_DISPLAY_W, DISPLAY_H);
    ctx.fillStyle = '#0a0f18';
    ctx.fillRect(0, 0, BAND_DISPLAY_W, DISPLAY_H);

    // ── Layer 1: wave — RMS mode only, sharp (no glow) ─────────────────────
    if (mode === 0) {
      const waveBaseline = DISPLAY_H - 3;
      const waveSpan      = DISPLAY_H - 8;
      ctx.beginPath();
      history.current.forEach((v, i) => {
        const x = i, y = waveBaseline - v * waveSpan;
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      });
      ctx.lineTo(BAND_DISPLAY_W, waveBaseline);
      ctx.lineTo(0, waveBaseline);
      ctx.closePath();
      ctx.fillStyle = `${hexColor}55`;
      ctx.fill();

      ctx.beginPath();
      history.current.forEach((v, i) => {
        const x = i, y = waveBaseline - v * waveSpan;
        i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      });
      ctx.strokeStyle = hexColor;
      ctx.lineWidth   = 1;
      ctx.stroke();
    }

    // ── Layer 2: Zoom window / active Band tinting ────────────────────────
    if (mode === 1) {
      ctx.fillStyle = `${hexColor}18`;
      ctx.fillRect(zoomX1, 0, Math.max(1, zoomX2 - zoomX1), DISPLAY_H);
      ctx.fillStyle = `${hexColor}33`;
      ctx.fillRect(bandX1, 0, Math.max(1, bandX2 - bandX1), DISPLAY_H);
      ctx.strokeStyle = hexColor;
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(bandX1, 0); ctx.lineTo(bandX1, DISPLAY_H); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(bandX2, 0); ctx.lineTo(bandX2, DISPLAY_H); ctx.stroke();
    }

    // ── Layer 3: FILTER curve — stylised, not real filter math ───────────
    // A smooth raised-cosine bump centred on the active Band, tapering to
    // the baseline at ~1.5x the band's own width either side. Flat in RMS
    // mode (there's genuinely no shaping happening to show).
    //
    // peakPx scales with Sensitivity — not plotting its dB value as a
    // curve position (which would be wrong, Sensitivity isn't frequency-
    // dependent, same reasoning that kept it off this curve entirely and
    // on its own line), just scaling the decorative bump's overall size
    // as a stylistic echo of the fader, the same way a VU needle doesn't
    // need a labelled axis to be useful. Directly visualises "turn
    // Sensitivity up to compensate for a narrow band's lower energy" —
    // the curve visibly grows as you do. Range chosen so the default
    // (+20dB) lands exactly on the old fixed value (11px), for continuity.
    const baseline  = DISPLAY_H / 2;
    const sensNorm  = Math.max(0, Math.min(1, (sensitivityDb + 20) / 80)); // -20..+60 → 0..1
    const peakPx    = 4 + sensNorm * 14; // 4px..18px, 11px at the +20dB default
    const centreX   = mode === 1 ? (bandX1 + bandX2) / 2 : BAND_DISPLAY_W / 2;
    const halfWidth = mode === 1 ? Math.max(8, (bandX2 - bandX1) * 0.9) : BAND_DISPLAY_W;
    ctx.beginPath();
    for (let x = 0; x <= BAND_DISPLAY_W; x += 2) {
      const t = mode === 1 ? Math.max(-1, Math.min(1, (x - centreX) / halfWidth)) : 1;
      const bump = mode === 1 ? Math.cos(t * Math.PI * 0.5) ** 2 : 0;
      const y = baseline - bump * peakPx;
      x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    }
    ctx.strokeStyle = hexColor;
    ctx.lineWidth   = 1.5;
    ctx.stroke();
  }, [level, sensitivityDb, mode, bandLow, bandHigh, zoomMin, zoomMax]);

  // Sensitivity line — moves with the Sensitivity slider (-20..+60dB),
  // higher dB reads as higher up, same up-is-more convention as a fader.
  // Kept as a DOM overlay (not drawn on the canvas) — simpler, and it
  // genuinely needs to sit above everything else including the filter
  // curve, which is easiest as a separate element rather than having to
  // re-order canvas draw calls around it.
  const sensPct = ((60 - sensitivityDb) / (60 - (-20))) * 100;

  return (
    <div style={{ margin: '4px 8px', position: 'relative' }}>
      <div style={{ position: 'relative', height: DISPLAY_H, borderRadius: 2, overflow: 'hidden' }}>
        <canvas ref={canvasRef} width={BAND_DISPLAY_W} height={DISPLAY_H}
          style={{ display: 'block', width: '100%', height: '100%' }} />

        {/* Sensitivity line — crisp, on top of everything */}
        <div style={{
          position: 'absolute', left: 0, right: 0, top: `${sensPct}%`,
          height: 1, background: color, opacity: 0.9,
        }} />
        <span style={{
          position: 'absolute', top: `${sensPct}%`, right: 3,
          transform: 'translateY(-50%)', fontSize: 7, color,
          background: '#0a0f18', padding: '0 2px',
        }}>{sensitivityDb >= 0 ? '+' : ''}{sensitivityDb.toFixed(1)}dB</span>

        <span style={{ position: 'absolute', bottom: 1, left: 3, fontSize: 7, color: 'var(--text-muted)' }}>20</span>
        <span style={{ position: 'absolute', bottom: 1, right: 3, fontSize: 7, color: 'var(--text-muted)' }}>20k</span>

        {/* Band Low/High values, at the bottom of their own vertical
            boundary lines — same size as 20/20k above, so the chosen
            range is still readable when the settings panel (and its
            sliders) are folded away. FREQ mode only, matching the band
            lines themselves, which also only draw in that mode. */}
        {mode === 1 && (
          <>
            <span style={{
              position: 'absolute', bottom: 1, left: `${(bandX1 / BAND_DISPLAY_W) * 100}%`,
              transform: 'translateX(calc(-100% - 3px))', fontSize: 7, color: hexColor,
            }}>{freqLabel(bandLow)}</span>
            <span style={{
              position: 'absolute', bottom: 1, left: `${(bandX2 / BAND_DISPLAY_W) * 100}%`,
              transform: 'translateX(3px)', fontSize: 7, color: hexColor,
            }}>{freqLabel(bandHigh)}</span>
          </>
        )}
      </div>
    </div>
  );
}

// ── Main node ─────────────────────────────────────────────────────────────────
export default function AudioToDmxNode({ id, data, selected }: NodeProps) {
  const { setHint } = useContext(HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const [showSettings, setShowSettings] = useState(() => _settingsOpen.get(id) ?? false);

  const nodeData = data as any;
  const paxColourCategory = nodeData.paxName ? _paxInfoMap.get(nodeData.paxName)?.colourCategory : undefined;
  const theme  = detectPaxTheme(nodeData.ports ?? [], paxColourCategory);
  const ACCENT = theme.accent; // Converter -> fuchsia, auto-detected same as every other Pax
  // Same fix as BandDisplay's hexColor — ACCENT is "var(--value)", and
  // concatenating alpha onto a var() reference (`${ACCENT}18` etc. below)
  // is invalid CSS that fails silently, not just a canvas-only problem.
  const accentHex = resolveCssColor(ACCENT);

  // Parameters (0-4 are real backend params; zoomMin/zoomMax are UI-only —
  // see the settingsJson-restore effect for why they still ride along).
  // dampingMs is real (backend index 5) but appended at array position 7,
  // AFTER zoomMin/zoomMax rather than inserted before them — inserting it
  // at position 5 would have shifted every existing saved project's
  // zoomMin/zoomMax into the wrong slots on next load.
  const [mode,          setMode]          = useState(1);     // 0=RMS, 1=Freq Range
  const [sensitivityDb, setSensitivityDb] = useState(20);
  const [bandLow,       setBandLow]       = useState(200);
  const [bandHigh,      setBandHigh]      = useState(2000);
  const [dmxChannel,    setDmxChannel]    = useState(1);
  const [zoomMin,       setZoomMin]       = useState(FREQ_MIN);
  const [zoomMax,       setZoomMax]       = useState(FREQ_MAX);
  const [dampingMs,     setDampingMs]     = useState(0);      // 0 = none

  const portBodyRef = useRef<HTMLDivElement>(null);

  // Live audio level for the BandDisplay wave (RMS mode only) — same
  // source Envelope's own ccValue readout uses (per-node RMS telemetry,
  // available for any node with audio buffers, not just monitor-specific
  // nodes). Removed once already when the wave itself was removed;
  // reinstated now that the wave's back. Gated to RMS mode specifically —
  // re-subscribes on mode change (cheap; user-triggered, not per-frame),
  // so no level updates (and therefore no canvas redraws) happen in FREQ
  // mode, where the wave doesn't render anyway. The earlier stutter
  // complaint was against the glow/blur version's heavier per-frame
  // shadowBlur cost, not necessarily this plain fill+stroke — but there's
  // no reason to redraw 30x/sec for a layer that isn't even visible.
  const [level, setLevel] = useState(0);
  useEffect(() => {
    if (mode !== 0) { setLevel(0); return; }
    return Bridge.onPortActivity((entries: any[]) => {
      const e = entries.find((e: any) => e.id === id);
      if (e) setLevel(Math.max(e.l ?? 0, e.r ?? 0) / 1000);
    });
  }, [id, mode]);

  // Single source of truth for both mount-time defaults AND undo/redo sync
  // (same shape as EnvelopeNode.tsx). Deliberately tolerant of a settingsJson
  // array shorter than expected (pads missing trailing entries with their
  // own default) rather than discarding the whole restore on a length
  // mismatch — GenericNode.tsx had exactly that bug (silently dropping an
  // entire Undo because DMX Channel's addition changed the param count);
  // built correctly from the start here instead of inheriting it.
  useEffect(() => {
    const sj = nodeData?.settingsJson;
    const defaults = [1, 20, 200, 2000, 1, FREQ_MIN, FREQ_MAX, 0];
    let raw: number[] | null = null;
    try { raw = sj ? (JSON.parse(sj) as number[]) : null; } catch { raw = null; }
    const v = defaults.map((d, i) => (raw && raw[i] !== undefined) ? raw[i] : d);

    setMode(Math.round(v[0]));
    setSensitivityDb(v[1]);
    setBandLow(v[2]);
    setBandHigh(v[3]);
    setDmxChannel(Math.round(v[4]));
    setZoomMin(v[5]);
    setZoomMax(v[6]);
    setDampingMs(v[7]);
    // First 5 are real Pax parameters at matching backend indices.
    // Damping is also real, but at array position 7 (backend index 5) —
    // see the state declarations above for why it isn't at position 5.
    // zoomMin/zoomMax (positions 5-6) are UI-only, never sent to the
    // backend.
    for (let i = 0; i < 5; i++) Bridge.setPaxParameter(id, i, v[i]);
    Bridge.setPaxParameter(id, IDX_DAMPING, v[7]);
  }, [nodeData?.settingsJson]);

  // General-purpose committer — takes a partial patch of any of the 7
  // tracked values, applies it on top of current state, persists the
  // whole array, and pushes only the real Pax-parameter indices to the
  // backend. Needed (rather than EnvelopeNode's simpler single-index
  // setParam) because zoom changes must atomically clamp bandLow/bandHigh
  // in the same update — doing that as two separate setParam calls would
  // let the two intermediate states each get its own inconsistent commit.
  const applyUpdate = useCallback((patch: Partial<{
    mode: number; sensitivityDb: number; bandLow: number; bandHigh: number;
    dmxChannel: number; zoomMin: number; zoomMax: number; dampingMs: number;
  }>) => {
    const next = {
      mode: patch.mode ?? mode,
      sensitivityDb: patch.sensitivityDb ?? sensitivityDb,
      bandLow: patch.bandLow ?? bandLow,
      bandHigh: patch.bandHigh ?? bandHigh,
      dmxChannel: patch.dmxChannel ?? dmxChannel,
      zoomMin: patch.zoomMin ?? zoomMin,
      zoomMax: patch.zoomMax ?? zoomMax,
      dampingMs: patch.dampingMs ?? dampingMs,
    };
    setMode(next.mode); setSensitivityDb(next.sensitivityDb);
    setBandLow(next.bandLow); setBandHigh(next.bandHigh);
    setDmxChannel(next.dmxChannel);
    setZoomMin(next.zoomMin); setZoomMax(next.zoomMax);
    setDampingMs(next.dampingMs);

    Bridge.setNodeSettings(id, [
      next.mode, next.sensitivityDb, next.bandLow, next.bandHigh,
      next.dmxChannel, next.zoomMin, next.zoomMax, next.dampingMs,
    ]);
    if (patch.mode          !== undefined) Bridge.setPaxParameter(id, IDX_MODE,     next.mode);
    if (patch.sensitivityDb !== undefined) Bridge.setPaxParameter(id, IDX_SENS,     next.sensitivityDb);
    if (patch.bandLow       !== undefined) Bridge.setPaxParameter(id, IDX_BANDLOW,  next.bandLow);
    if (patch.bandHigh      !== undefined) Bridge.setPaxParameter(id, IDX_BANDHIGH, next.bandHigh);
    if (patch.dmxChannel    !== undefined) Bridge.setPaxParameter(id, IDX_CHANNEL,  next.dmxChannel);
    if (patch.dampingMs     !== undefined) Bridge.setPaxParameter(id, IDX_DAMPING,  next.dampingMs);
  }, [id, mode, sensitivityDb, bandLow, bandHigh, dmxChannel, zoomMin, zoomMax, dampingMs]);

  // Band handle drag — PUSHES the other handle along when dragged past it,
  // rather than locking in place at the boundary (the earlier behaviour:
  // dragging Low up to meet High just stopped Low dead at High-1, which
  // read as "stuck", not "met"). A 1Hz minimum gap is still enforced on
  // both handles so they can never fully cross or overlap to zero width.
  const onBandChange = useCallback((which: 'low' | 'high', v: number) => {
    if (which === 'low') {
      const newLow  = Math.max(zoomMin, Math.min(v, zoomMax - 1));
      const newHigh = newLow >= bandHigh - 1 ? Math.min(zoomMax, newLow + 1) : bandHigh;
      applyUpdate({ bandLow: newLow, bandHigh: newHigh });
    } else {
      const newHigh = Math.min(zoomMax, Math.max(v, zoomMin + 1));
      const newLow  = newHigh <= bandLow + 1 ? Math.max(zoomMin, newHigh - 1) : bandLow;
      applyUpdate({ bandHigh: newHigh, bandLow: newLow });
    }
  }, [applyUpdate, bandLow, bandHigh, zoomMin, zoomMax]);

  // Zoom Stepper change — narrows/widens the slider's own span. Clamps
  // bandLow/bandHigh into the new window in the same update if the window
  // moved past either of them, so the two handles never end up stranded
  // outside the visible track.
  const onZoomChange = useCallback((which: 'min' | 'max', v: number) => {
    let newMin = zoomMin, newMax = zoomMax;
    if (which === 'min') newMin = Math.max(FREQ_MIN, Math.min(Math.round(v), zoomMax - 10));
    else                 newMax = Math.min(FREQ_MAX, Math.max(Math.round(v), zoomMin + 10));
    const clampedLow  = Math.min(Math.max(bandLow,  newMin), newMax - 1);
    const clampedHigh = Math.max(Math.min(bandHigh, newMax), newMin + 1);
    applyUpdate({ zoomMin: newMin, zoomMax: newMax, bandLow: clampedLow, bandHigh: clampedHigh });
    Bridge.commitNodeSettings(id);
  }, [applyUpdate, zoomMin, zoomMax, bandLow, bandHigh, id]);

  const handleReset = useCallback(() => {
    applyUpdate({
      mode: 1, sensitivityDb: 20, bandLow: 200, bandHigh: 2000,
      dmxChannel: 1, zoomMin: FREQ_MIN, zoomMax: FREQ_MAX, dampingMs: 0,
    });
    Bridge.commitNodeSettings(id);
  }, [applyUpdate, id]);

  const ports    = nodeData?.ports ?? [];
  const inAudio  = ports.filter((p: any) => p.type === 'audio' && p.direction === 'input');
  const outAudio = ports.filter((p: any) => p.type === 'audio' && p.direction === 'output');
  const outDmx   = ports.filter((p: any) => p.type !== 'audio' && p.direction === 'output');
  const label    = nodeData?.label ?? 'Audio to DMX';

  return (
    <div
      onMouseEnter={() => setHint({ title: 'Audio to DMX', body: 'Isolates a frequency range (or whole-signal RMS) from audio and drives a DMX channel value.\nFirst Pax exercised hosted inside a DAW.' })}
      onMouseLeave={() => setHint(null)}
      style={{
        ...nodeContainerStyle(ACCENT, !!selected),
        minWidth: 300,
      }}
    >
      {/* IN/OUT ports — anchored to portBodyRef so they stay together.
          Output ports' offsets (7, 21) centre the pair on the band
          graphic's single-canvas content (4px top margin + 44px canvas =
          48px, centre ~26px down; symmetric 14px-apart positions around
          that land at 19 and 33, i.e. offset 7 and 21 once the +12px
          header gap is subtracted). */}
      {inAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={7} />
      ))}
      {outAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={7} />
      ))}
      {outDmx.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--dmx)" portBodyRef={portBodyRef} offset={21} />
      ))}

      {/* Header */}
      <div style={{
        display: 'flex', alignItems: 'center', padding: '4px 8px', gap: 4,
        background: `${accentHex}18`,
        borderBottom: collapsed ? 'none' : `1px solid ${accentHex}44`,
        cursor: 'pointer',
      }} onDoubleClick={toggleCollapsed}>

        <NodeHeaderButton onClick={toggleCollapsed}
          onHint={{ onMouseEnter: () => {}, onMouseLeave: () => {} }}>
          <NodeCollapseArrow collapsed={collapsed} accent={ACCENT} />
        </NodeHeaderButton>

        <div style={{ flex: 1, fontSize: '11px', fontWeight: 700, color: ACCENT,
          letterSpacing: '0.1em', fontFamily: "'Syne', sans-serif",
          textTransform: 'uppercase', userSelect: 'none' }}>
          {label}
        </div>

        {/* Mode badge — solid fill when active, matching Envelope's Amp/Band
            badge exactly (not an outline) */}
        <div className="nodrag" onClick={e => { e.stopPropagation(); applyUpdate({ mode: mode === 0 ? 1 : 0 }); Bridge.commitNodeSettings(id); }}
          style={{
            fontSize: 8, padding: '2px 5px', borderRadius: 2, cursor: 'pointer',
            background: mode === 1 ? ACCENT : 'var(--surface)',
            color:      mode === 1 ? '#000' : 'var(--text-muted)',
            fontWeight: 700, userSelect: 'none',
          }}>
          {mode === 0 ? 'RMS' : 'FREQ'}
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={handleReset}
            onHint={{ onMouseEnter: () => setHint({ title: 'Reset', body: 'Reset all parameters to defaults.' }), onMouseLeave: () => setHint(null) }}>
            <span style={{ fontSize: 11, fontWeight: 700 }}>R</span>
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={() => setShowSettings(v => { const next = !v; _settingsOpen.set(id, next); return next; })}
            onHint={{ onMouseEnter: () => setHint({ title: 'Settings', body: 'Configure Audio to DMX parameters.' }), onMouseLeave: () => setHint(null) }}>
            <span style={{
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              border: showSettings ? `1px solid ${ACCENT}` : '1px solid transparent',
              borderRadius: 3, padding: '1px',
            }}>
              <Settings size={11} color={showSettings ? ACCENT : 'var(--text-muted)'} />
            </span>
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={() => Bridge.removeNode(id)}
            onHint={{ onMouseEnter: () => setHint({ title: 'Delete node', body: 'Remove this node.' }), onMouseLeave: () => setHint(null) }}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {/* Body — always-visible band graphic; also gives the node real,
          constant height so ports never dangle outside it when the
          settings panel is folded (the old 6px spacer wasn't tall enough
          to contain where NodeHandle positions the ports below). */}
      {!collapsed && (
        <div ref={portBodyRef} style={{ paddingBottom: 6 }}>
          <BandDisplay mode={mode} bandLow={bandLow} bandHigh={bandHigh}
            zoomMin={zoomMin} zoomMax={zoomMax} sensitivityDb={sensitivityDb}
            level={level} color={ACCENT} />
        </div>
      )}

      {/* Settings */}
      {showSettings && !collapsed && (
        <div style={settingsPanelStyle}>

          <Stepper label="DMX Channel" value={dmxChannel} min={1} max={512} width={36} color={ACCENT} buttons
            onChange={v => { applyUpdate({ dmxChannel: v }); Bridge.commitNodeSettings(id); }} />

          <div style={sectionDividerStyle}>
            <SliderRow label="Sensitivity" value={sensitivityDb} min={-20} max={60} step={0}
              format={v => `${v >= 0 ? '+' : ''}${v.toFixed(1)}dB`}
              onChange={v => applyUpdate({ sensitivityDb: v })} color={ACCENT}
              onDoubleClick={() => applyUpdate({ sensitivityDb: 20 })}
              onCommit={() => Bridge.commitNodeSettings(id)} />
          </div>

          {/* Damping — one control, symmetric (same time constant rising
              and falling), applied to the final value identically in both
              Mode settings. Deliberately not a return to the old two-knob
              Attack/Release (removed 2026-07-27 for making things harder
              to dial in, not easier) — "None" at 0 preserves the exact
              prior behaviour for anyone who doesn't touch it. */}
          <SliderRow label="Damping" value={dampingMs} min={0} max={500} step={0}
            format={v => v < 1 ? 'None' : `${v.toFixed(0)}ms`}
            onChange={v => applyUpdate({ dampingMs: v })} color={ACCENT}
            onDoubleClick={() => applyUpdate({ dampingMs: 0 })}
            onCommit={() => Bridge.commitNodeSettings(id)} />

          {/* Band filters — only in Freq Range mode */}
          {mode === 1 && (
            <div style={sectionDividerStyle}>
              <div style={{ color: 'var(--text-muted)', fontSize: 8, marginBottom: 4 }}>Freq Band</div>
              <FreqBandDual
                bandLow={bandLow} bandHigh={bandHigh}
                zoomMin={zoomMin} zoomMax={zoomMax}
                color={ACCENT}
                onBandChange={onBandChange}
                onZoomChange={onZoomChange}
                onCommit={() => Bridge.commitNodeSettings(id)}
              />
            </div>
          )}
        </div>
      )}
    </div>
  );
}
