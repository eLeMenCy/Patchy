// Patchy — Audio to OSC node UI (band-style graphic, PRESERVED ALTERNATIVE)
//
// Preserved 2026-08-26 when the user asked to try the simpler level-meter
// design first instead — this file is NOT currently registered in
// App.tsx's nodeTypes map or paxName-matching condition (see
// AudioPeakToOscNode.tsx, which now holds the level-meter version). Kept
// here in full, working order specifically so the user can revert to this
// "similar in spirit to AudioToDmxNode" graphic if they end up preferring
// it over the simpler one — swap which file's content lives at
// AudioPeakToOscNode.tsx to switch back, no App.tsx changes needed either
// way since only the file path itself matters, not this exported function
// name (renamed to AudioPeakToOscNode_BandStyle here only to avoid a
// name collision if this file and the active one are ever both open/
// imported at once — App.tsx never references this identifier directly).

import { useEffect, useRef, useState, useCallback, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, NodeHeaderButton, NodeCollapseArrow, nodeContainerStyle, settingsPanelStyle, sectionDividerStyle, _paxInfoMap, detectPaxTheme, resolveCssColor, SliderRow, Stepper, FreqBandDual, freqLabel } from './NodeUtils';
import { HintContext } from './HintPanel';
import { Settings, X } from 'lucide-react';

// Bespoke UI for AudioPeakToOscPax (2026-08-26), built per the user's own
// request after comparing this node's original generic look against
// EnvelopeNode/AudioToDmxNode/SpectrumyserNode — closely mirrors
// AudioToDmxNode.tsx throughout (the most directly analogous existing
// bespoke component: same "audio → non-audio protocol" shape, same
// Mode/Sensitivity/Damping parameters), including reusing its own
// dual-head frequency-range slider (now shared via NodeUtils.tsx) rather
// than rebuilding an equivalent — per the user's own explicit request.
// Deliberately scoped to visual polish and interaction feel only, per the
// user's own framing — no DSP/behavioural changes at all; every value
// this file reads/writes maps 1:1 to AudioPeakToOscPax.cpp's own existing
// parameters, unchanged.

// Persist settings panel open state across graph updates (survives undo/redo)
const _settingsOpen = new Map<string, boolean>();

const FREQ_MIN = 20, FREQ_MAX = 20000;

// Param indices — must match AudioPeakToOscPax.cpp's PAX_getParameterInfo,
// plus two UI-only entries (8, 9) that never go to the backend at all —
// see the settingsJson-restore effect below for why they're still part of
// the same persisted array. Unlike AudioToDmxNode.tsx's own history, this
// Pax is brand new (built 2026-08-25) with no existing saved projects to
// protect, so all 8 real parameters map directly to array positions 0-7 in
// their own natural backend order — no AudioToDmxPax-style "append after
// the UI-only slots" workaround was needed here.
const IDX_MODE = 0, IDX_MEASUREMENT = 1, IDX_SENS = 2, IDX_BANDLOW = 3,
      IDX_BANDHIGH = 4, IDX_DAMPING = 5, IDX_SENDMODE = 6, IDX_MAXRATE = 7;

// ── Level display — small always-visible graphic, "similar in spirit" to
// AudioToDmxNode's own BandDisplay per the user's own explicit request
// (rather than a simpler level-meter alternative) — same four-layer
// structure (wave / zoom+band tinting / filter curve / sensitivity line),
// adapted for this Pax's own two independent axes:
//   - Mode (whole signal vs Freq Range) drives the same wave-vs-band-
//     tinting choice AudioToDmxPax's own Mode did — Measurement (RMS vs
//     Peak) doesn't change which layers draw, only what the underlying
//     level itself represents; the wave shows whichever measurement is
//     actually active, an honest readout either way rather than a
//     separate visual per combination.
//   - No DMX Channel concept for OSC, so there's nothing analogous to
//     plot as a "channel" readout — the graphic stays purely about the
//     level and frequency shaping, same as the original for its own Mode.
const DISPLAY_H = 44;
const BAND_DISPLAY_W = 260;
function LevelDisplay({ mode, bandLow, bandHigh, zoomMin, zoomMax, sensitivityDb, level, color }: {
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

  const hexColor = resolveCssColor(color, '#e879f9');

  const toX = (hz: number) => {
    const clamped = Math.max(FREQ_MIN, Math.min(FREQ_MAX, hz));
    return (Math.log(clamped / FREQ_MIN) / Math.log(FREQ_MAX / FREQ_MIN)) * BAND_DISPLAY_W;
  };
  const zoomX1 = toX(zoomMin), zoomX2 = toX(zoomMax);
  const bandX1 = toX(bandLow), bandX2 = toX(bandHigh);

  useEffect(() => {
    // Real sensitivity gain — same formula as AudioPeakToOscPax.cpp's own
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

    // ── Layer 1: wave — whole-signal mode only, sharp (no glow) ───────────
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
    // Same decorative bump as AudioToDmxNode's own — see that file's own
    // comment for the full reasoning (scales with Sensitivity, not a real
    // analytic bandpass response).
    const baseline  = DISPLAY_H / 2;
    const sensNorm  = Math.max(0, Math.min(1, (sensitivityDb + 20) / 80)); // -20..+60 → 0..1
    const peakPx    = 4 + sensNorm * 14;
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
export default function AudioPeakToOscNode_BandStyle({ id, data, selected }: NodeProps) {
  const { setHint } = useContext(HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const [showSettings, setShowSettings] = useState(() => _settingsOpen.get(id) ?? false);

  const nodeData = data as any;
  const paxColourCategory = nodeData.paxName ? _paxInfoMap.get(nodeData.paxName)?.colourCategory : undefined;
  const theme  = detectPaxTheme(nodeData.ports ?? [], paxColourCategory);
  const ACCENT = theme.accent; // Converter -> fuchsia, auto-detected same as every other Pax
  const accentHex = resolveCssColor(ACCENT, '#e879f9');

  // Parameters 0-7 are all real backend params (see IDX_* above); zoomMin/
  // zoomMax (positions 8-9) are UI-only — see the settingsJson-restore
  // effect for why they still ride along in the same persisted array.
  const [mode,          setMode]          = useState(1);     // 0=Whole, 1=Freq Range
  const [measurement,   setMeasurement]   = useState(0);     // 0=RMS, 1=Peak
  const [sensitivityDb, setSensitivityDb] = useState(20);
  const [bandLow,       setBandLow]       = useState(200);
  const [bandHigh,      setBandHigh]      = useState(2000);
  const [dampingMs,     setDampingMs]     = useState(0);      // 0 = none
  const [sendMode,      setSendMode]      = useState(0);      // 0=Change, 1=Rate
  const [maxRateHz,     setMaxRateHz]     = useState(30);
  const [zoomMin,       setZoomMin]       = useState(FREQ_MIN);
  const [zoomMax,       setZoomMax]       = useState(FREQ_MAX);

  const portBodyRef = useRef<HTMLDivElement>(null);

  // Live audio level for the LevelDisplay wave (whole-signal mode only) —
  // same source AudioToDmxNode's own BandDisplay wave uses (per-node RMS
  // telemetry, available for any node with audio buffers). Gated to Mode=0
  // specifically, same reasoning as the original: no level updates (and
  // therefore no canvas redraws) happen in Freq Range mode, where the wave
  // doesn't render anyway.
  const [level, setLevel] = useState(0);
  useEffect(() => {
    if (mode !== 0) { setLevel(0); return; }
    return Bridge.onPortActivity((entries: any[]) => {
      const e = entries.find((e: any) => e.id === id);
      if (e) setLevel(Math.max(e.l ?? 0, e.r ?? 0) / 1000);
    });
  }, [id, mode]);

  // Single source of truth for both mount-time defaults AND undo/redo sync
  // (same shape as AudioToDmxNode.tsx's own). Tolerant of a settingsJson
  // array shorter than expected, same reasoning as that file's own comment.
  useEffect(() => {
    const sj = nodeData?.settingsJson;
    const defaults = [1, 0, 20, 200, 2000, 0, 0, 30, FREQ_MIN, FREQ_MAX];
    let raw: number[] | null = null;
    try { raw = sj ? (JSON.parse(sj) as number[]) : null; } catch { raw = null; }
    const v = defaults.map((d, i) => (raw && raw[i] !== undefined) ? raw[i] : d);

    setMode(Math.round(v[0]));
    setMeasurement(Math.round(v[1]));
    setSensitivityDb(v[2]);
    setBandLow(v[3]);
    setBandHigh(v[4]);
    setDampingMs(v[5]);
    setSendMode(Math.round(v[6]));
    setMaxRateHz(v[7]);
    setZoomMin(v[8]);
    setZoomMax(v[9]);
    // First 8 are real Pax parameters at matching backend indices;
    // positions 8-9 (zoomMin/zoomMax) are UI-only, never sent to the backend.
    for (let i = 0; i < 8; i++) Bridge.setPaxParameter(id, i, v[i]);
  }, [nodeData?.settingsJson]);

  // General-purpose committer — same shape as AudioToDmxNode.tsx's own
  // applyUpdate, for the same reason: zoom changes must atomically clamp
  // bandLow/bandHigh in the same update.
  const applyUpdate = useCallback((patch: Partial<{
    mode: number; measurement: number; sensitivityDb: number; bandLow: number; bandHigh: number;
    dampingMs: number; sendMode: number; maxRateHz: number; zoomMin: number; zoomMax: number;
  }>) => {
    const next = {
      mode: patch.mode ?? mode,
      measurement: patch.measurement ?? measurement,
      sensitivityDb: patch.sensitivityDb ?? sensitivityDb,
      bandLow: patch.bandLow ?? bandLow,
      bandHigh: patch.bandHigh ?? bandHigh,
      dampingMs: patch.dampingMs ?? dampingMs,
      sendMode: patch.sendMode ?? sendMode,
      maxRateHz: patch.maxRateHz ?? maxRateHz,
      zoomMin: patch.zoomMin ?? zoomMin,
      zoomMax: patch.zoomMax ?? zoomMax,
    };
    setMode(next.mode); setMeasurement(next.measurement); setSensitivityDb(next.sensitivityDb);
    setBandLow(next.bandLow); setBandHigh(next.bandHigh);
    setDampingMs(next.dampingMs); setSendMode(next.sendMode); setMaxRateHz(next.maxRateHz);
    setZoomMin(next.zoomMin); setZoomMax(next.zoomMax);

    Bridge.setNodeSettings(id, [
      next.mode, next.measurement, next.sensitivityDb, next.bandLow, next.bandHigh,
      next.dampingMs, next.sendMode, next.maxRateHz, next.zoomMin, next.zoomMax,
    ]);
    if (patch.mode          !== undefined) Bridge.setPaxParameter(id, IDX_MODE,        next.mode);
    if (patch.measurement   !== undefined) Bridge.setPaxParameter(id, IDX_MEASUREMENT, next.measurement);
    if (patch.sensitivityDb !== undefined) Bridge.setPaxParameter(id, IDX_SENS,        next.sensitivityDb);
    if (patch.bandLow       !== undefined) Bridge.setPaxParameter(id, IDX_BANDLOW,     next.bandLow);
    if (patch.bandHigh      !== undefined) Bridge.setPaxParameter(id, IDX_BANDHIGH,    next.bandHigh);
    if (patch.dampingMs     !== undefined) Bridge.setPaxParameter(id, IDX_DAMPING,     next.dampingMs);
    if (patch.sendMode      !== undefined) Bridge.setPaxParameter(id, IDX_SENDMODE,    next.sendMode);
    if (patch.maxRateHz     !== undefined) Bridge.setPaxParameter(id, IDX_MAXRATE,     next.maxRateHz);
  }, [id, mode, measurement, sensitivityDb, bandLow, bandHigh, dampingMs, sendMode, maxRateHz, zoomMin, zoomMax]);

  // Band handle drag / Zoom stepper change — identical logic to
  // AudioToDmxNode.tsx's own onBandChange/onZoomChange, reused verbatim
  // (not extracted, since both also close over their own file-local
  // applyUpdate/state — same reasoning FreqBandDual itself didn't need
  // this duplication, being a pure, self-contained control).
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
      mode: 1, measurement: 0, sensitivityDb: 20, bandLow: 200, bandHigh: 2000,
      dampingMs: 0, sendMode: 0, maxRateHz: 30, zoomMin: FREQ_MIN, zoomMax: FREQ_MAX,
    });
    Bridge.commitNodeSettings(id);
  }, [applyUpdate, id]);

  const ports    = nodeData?.ports ?? [];
  const inAudio  = ports.filter((p: any) => p.type === 'audio' && p.direction === 'input');
  const outAudio = ports.filter((p: any) => p.type === 'audio' && p.direction === 'output');
  const outOsc   = ports.filter((p: any) => p.type !== 'audio' && p.direction === 'output');
  const label    = nodeData?.label ?? 'Audio to OSC';

  return (
    <div
      onMouseEnter={() => setHint({ title: 'Audio to OSC', body: 'Extracts an RMS or Peak level (whole signal or an isolated frequency band) from audio and emits it as an OSC float.' })}
      onMouseLeave={() => setHint(null)}
      style={{
        ...nodeContainerStyle(ACCENT, !!selected),
        minWidth: 300,
      }}
    >
      {/* IN/OUT ports — anchored to portBodyRef so they stay together,
          same offsets as AudioToDmxNode.tsx's own for the same reason
          (centres the pair on the level graphic's single-canvas content). */}
      {inAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={7} />
      ))}
      {outAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={7} />
      ))}
      {outOsc.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--osc)" portBodyRef={portBodyRef} offset={21} />
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

        {/* Three compact badges — Mode / Measurement / Send Mode — per the
            user's own explicit request, same solid-fill-when-active style
            as AudioToDmxNode's own single badge, just three side by side
            rather than pushing two of these into the settings panel only. */}
        <div className="nodrag" onClick={e => { e.stopPropagation(); applyUpdate({ mode: mode === 0 ? 1 : 0 }); Bridge.commitNodeSettings(id); }}
          style={{
            fontSize: 8, padding: '2px 5px', borderRadius: 2, cursor: 'pointer',
            background: mode === 1 ? ACCENT : 'var(--surface)',
            color:      mode === 1 ? '#000' : 'var(--text-muted)',
            fontWeight: 700, userSelect: 'none',
          }}>
          {mode === 0 ? 'WHOLE' : 'FREQ'}
        </div>

        <div className="nodrag" onClick={e => { e.stopPropagation(); applyUpdate({ measurement: measurement === 0 ? 1 : 0 }); Bridge.commitNodeSettings(id); }}
          style={{
            fontSize: 8, padding: '2px 5px', borderRadius: 2, cursor: 'pointer',
            background: measurement === 1 ? ACCENT : 'var(--surface)',
            color:      measurement === 1 ? '#000' : 'var(--text-muted)',
            fontWeight: 700, userSelect: 'none',
          }}>
          {measurement === 0 ? 'RMS' : 'PEAK'}
        </div>

        <div className="nodrag" onClick={e => { e.stopPropagation(); applyUpdate({ sendMode: sendMode === 0 ? 1 : 0 }); Bridge.commitNodeSettings(id); }}
          style={{
            fontSize: 8, padding: '2px 5px', borderRadius: 2, cursor: 'pointer',
            background: sendMode === 1 ? ACCENT : 'var(--surface)',
            color:      sendMode === 1 ? '#000' : 'var(--text-muted)',
            fontWeight: 700, userSelect: 'none',
          }}>
          {sendMode === 0 ? 'CHANGE' : 'RATE'}
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={handleReset}
            onHint={{ onMouseEnter: () => setHint({ title: 'Reset', body: 'Reset all parameters to defaults.' }), onMouseLeave: () => setHint(null) }}>
            <span style={{ fontSize: 11, fontWeight: 700 }}>R</span>
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={() => setShowSettings(v => { const next = !v; _settingsOpen.set(id, next); return next; })}
            onHint={{ onMouseEnter: () => setHint({ title: 'Settings', body: 'Configure Audio to OSC parameters.' }), onMouseLeave: () => setHint(null) }}>
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

      {/* Body — always-visible level graphic; also gives the node real,
          constant height so ports never dangle outside it when the
          settings panel is folded. */}
      {!collapsed && (
        <div ref={portBodyRef} style={{ paddingBottom: 6 }}>
          <LevelDisplay mode={mode} bandLow={bandLow} bandHigh={bandHigh}
            zoomMin={zoomMin} zoomMax={zoomMax} sensitivityDb={sensitivityDb}
            level={level} color={ACCENT} />
        </div>
      )}

      {/* Settings */}
      {showSettings && !collapsed && (
        <div style={settingsPanelStyle}>

          {/* Max Rate — only meaningful in Send Mode=Rate; conditionally
              shown, same treatment as Freq Band being conditional on Mode
              below (a control that only affects behaviour in one of two
              states isn't shown in the other). */}
          {sendMode === 1 && (
            <Stepper label="Max Rate (Hz)" value={maxRateHz} min={1} max={100} width={36} color={ACCENT} buttons
              onChange={v => { applyUpdate({ maxRateHz: v }); Bridge.commitNodeSettings(id); }} />
          )}

          <div style={sectionDividerStyle}>
            <SliderRow label="Sensitivity" value={sensitivityDb} min={-20} max={60} step={0}
              format={v => `${v >= 0 ? '+' : ''}${v.toFixed(1)}dB`}
              onChange={v => applyUpdate({ sensitivityDb: v })} color={ACCENT}
              onDoubleClick={() => applyUpdate({ sensitivityDb: 20 })}
              onCommit={() => Bridge.commitNodeSettings(id)} />
          </div>

          {/* Damping — same one-control, symmetric approach as
              AudioToDmxNode's own — see that file's own comment. */}
          <SliderRow label="Damping" value={dampingMs} min={0} max={500} step={0}
            format={v => v < 1 ? 'None' : `${v.toFixed(0)}ms`}
            onChange={v => applyUpdate({ dampingMs: v })} color={ACCENT}
            onDoubleClick={() => applyUpdate({ dampingMs: 0 })}
            onCommit={() => Bridge.commitNodeSettings(id)} />

          {/* Band filters — only in Freq Range mode, same dual-head
              slider AudioToDmxNode.tsx uses, now shared via NodeUtils.tsx
              per the user's own explicit request. */}
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
