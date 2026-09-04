// Patchy — Audio to OSC node UI

import { useEffect, useRef, useState, useCallback, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, NodeHeaderButton, NodeCollapseArrow, nodeContainerStyle, settingsPanelStyle, sectionDividerStyle, _paxInfoMap, detectPaxTheme, resolveCssColor, SliderRow, Stepper, FreqBandDual } from './NodeUtils';
import { HintContext } from './HintPanel';
import { Settings, X } from 'lucide-react';

// Bespoke UI for AudioPeakToOscPax (2026-08-26), built per the user's own
// request after comparing this node's original generic look against
// EnvelopeNode/AudioToDmxNode/SpectrumyserNode — settings panel closely
// mirrors AudioToDmxNode.tsx throughout (the most directly analogous
// existing bespoke component), including reusing its own dual-head
// frequency-range slider (now shared via NodeUtils.tsx) rather than
// rebuilding an equivalent — per the user's own explicit request.
//
// The always-visible graphic itself is the SIMPLE level-meter option
// (SimpleLevelMeter below) — a genuine second attempt, not the first: an
// initial version closely mirrored AudioToDmxNode's own four-layer band
// graphic too, but the user clarified afterward they'd actually meant to
// try the simpler option first (a misread on this file's own author's
// part, corrected once caught). That original band-style version is
// preserved in full, working order at AudioPeakToOscNode_BandStyle.tsx —
// not deleted — specifically so it can be swapped back in if the user
// ends up preferring it over this simpler one; see that file's own header
// for how to switch.
//
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

// ── Level meter — simple, honest horizontal bar, tried 2026-08-26 as an
// alternative to the "similar in spirit" band-style graphic (preserved in
// full at AudioPeakToOscNode_BandStyle.tsx) per the user's own request to
// compare a simpler option before committing to either. Deliberately no
// canvas, no history buffer, no frequency axis, no filter-curve decoration
// at all — a single CSS-width-transition bar showing the same real
// sensitivity-gained level (level × 10^(dB/20), the exact value this Pax
// actually emits as OSC, clamped 0-1) as the band-style version's own wave
// did, just without the extra visual machinery around it. Deliberately NOT
// gated to Mode=0 the way the band-style wave was — that gating existed
// there because Freq Range mode had its own competing visual (the band
// tinting/filter curve) to look at instead; this simpler design has
// nothing else to show, so the meter stays live and meaningful in both
// Mode settings.
function SimpleLevelMeter({ level, sensitivityDb, color }: {
  level:         number; // 0-1, live audio level from port activity
  sensitivityDb: number;
  color:         string;
}) {
  const gain = Math.pow(10, sensitivityDb / 20);
  const pct  = Math.max(0, Math.min(1, level * gain)) * 100;

  return (
    <div style={{ margin: '4px 8px' }}>
      <div style={{
        position: 'relative', height: 16, borderRadius: 2, overflow: 'hidden',
        background: '#0a0f18', border: '1px solid var(--border)',
      }}>
        {/* transform: scaleX, not width — fixed 2026-08-27 after the
            user reported real, measured CPU/GPU load with this node in
            the graph (Activity Monitor showed a genuine spike, not just
            perceived heat). Animating `width` is a layout-triggering CSS
            property, so a continuously-updating width (level changes at
            ~30fps via onPortActivity, even from a silent/idle signal)
            forces the browser to repeatedly recalculate layout and
            repaint, never letting the transition settle — a well-known
            performance anti-pattern. scaleX is GPU-composited and never
            touches layout at all, visually identical (the bar still
            grows from the left, via transformOrigin) but without the
            layout-thrash cost. */}
        <div style={{
          position: 'absolute', top: 0, left: 0, bottom: 0, width: '100%',
          transformOrigin: 'left center',
          transform: `scaleX(${pct / 100})`,
          background: color, transition: 'transform 60ms linear',
        }} />
        <span style={{
          position: 'absolute', top: '50%', left: 4, transform: 'translateY(-50%)',
          fontSize: 8, color: 'var(--text-muted)', mixBlendMode: 'difference',
        }}>
          {(pct / 100).toFixed(2)}
        </span>
        <span style={{
          position: 'absolute', top: '50%', right: 4, transform: 'translateY(-50%)',
          fontSize: 7, color: 'var(--text-muted)', mixBlendMode: 'difference',
        }}>
          {sensitivityDb >= 0 ? '+' : ''}{sensitivityDb.toFixed(1)}dB
        </span>
      </div>
    </div>
  );
}

// ── Main node ─────────────────────────────────────────────────────────────────
export default function AudioPeakToOscNode({ id, data, selected }: NodeProps) {
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

  // Live audio level for SimpleLevelMeter — same source AudioToDmxNode's
  // own BandDisplay wave uses (per-node RMS telemetry, available for any
  // node with audio buffers). NOT gated to Mode=0 the way the band-style
  // version's wave was — see SimpleLevelMeter's own comment for why: this
  // simpler design has no competing visual to show in Freq Range mode, so
  // the meter needs to stay live in both.
  const [level, setLevel] = useState(0);
  const lastLevelRef = useRef(0);
  useEffect(() => {
    // Gated to !collapsed (added 2026-08-27, after the user's own heat
    // report): when the node is collapsed, SimpleLevelMeter isn't even
    // rendered (see the body's own `{!collapsed && (...)}` guard below),
    // so there was never any reason to keep polling and re-rendering at
    // ~30fps in that state.
    //
    // Change-threshold filtering added 2026-08-27 (2nd pass), after the
    // user recalled this project's own prior history with exactly this
    // class of bug: AudioToDmxNode's own BandDisplay wave, once built,
    // caused a real, reported "stutter" traced to precisely the same
    // root cause — an unconditional 30fps onPortActivity subscription
    // driving a full re-render/redraw every single tick, regardless of
    // whether the underlying value had genuinely moved (see
    // SessionLog.md, 2026-08-01, "wave removed entirely" then
    // "reinstated... gated to RMS mode"). That established fix gated the
    // subscription to one Mode only, since the wave itself was only ever
    // visible in that mode — but this simpler meter deliberately needs
    // to stay live in *both* Mode settings (the whole reason the
    // earlier mode-gate was removed in the first place), so the same
    // fix can't be reapplied verbatim. Filtering by a genuine-change
    // threshold instead achieves the same goal a different way: still
    // live in both modes, but no longer re-rendering the entire
    // component tree on every single tick when the level is silent or
    // holding steady, only when it's actually moved by more than noise-
    // floor jitter.
    if (collapsed) return;
    return Bridge.onPortActivity((entries: any[]) => {
      const e = entries.find((e: any) => e.id === id);
      if (!e) return;
      const next = Math.max(e.l ?? 0, e.r ?? 0) / 1000;
      if (Math.abs(next - lastLevelRef.current) < 0.0005) return;
      lastLevelRef.current = next;
      setLevel(next);
    });
  }, [id, collapsed]);

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
      {/* IN/OUT ports — anchored to portBodyRef so they stay together.
          Corrected 2026-08-26 (2nd pass) after the user caught two real
          issues in the first attempt:
            1. Still off by ~5px, not perfectly centred — a genuinely
               precise match to half of NodeHandle's own 10px handle
               height (see NodeUtils.tsx). CSS `top` positions an
               element's own TOP EDGE, not its centre — the earlier
               offset=0 calculation correctly placed 12px (the meter's
               own vertical centre) at the handle's top edge, but never
               accounted for the handle's own height meaning its actual
               visual centre then sits 5px *below* that point. Subtracting
               5 (half of 10) here corrects for that, landing the
               handle's own centre — not its top edge — on the meter's.
            2. The downstream Audio port hadn't actually disappeared —
               outAudio and outOsc are both on the node's right side, and
               both had been set to the exact same offset, so one was
               stacked directly on top of the other, completely hiding
               it. They need to be genuinely separated, not just each
               individually centred — a ±5px symmetric split around the
               same centre-corrected value keeps both visible and
               reasonably close together, proportionate to this shorter
               meter (a smaller gap than the old band-style graphic's own
               14px split between two ports on a much taller canvas). */}
      {inAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={-5} />
      ))}
      {outAudio.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--audio)" portBodyRef={portBodyRef} offset={-10} />
      ))}
      {outOsc.map((p: any) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour="var(--osc)" portBodyRef={portBodyRef} offset={0} />
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
          <SimpleLevelMeter level={level} sensitivityDb={sensitivityDb} color={ACCENT} />
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
