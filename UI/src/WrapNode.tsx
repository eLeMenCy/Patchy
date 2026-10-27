// Patchy — Wraps (v0.0.930): several nodes shown as one box.
//
// A wrap is UI / project state only (backend: GraphModel::WrapData). The
// processing graph never sees it: members stay ordinary nodes with ordinary
// connections. App.tsx turns the graph into what ReactFlow draws:
//   folded wrap → members hidden, one "wrap" node; edges crossing its border
//                 redrawn to it with their own ids, and its ports reuse the
//                 inner ports' ids — so the activity flashes (keyed by edge
//                 id / handle id) light the wrap with no extra code.
//   open wrap   → members shown, a "wrapFrame" node drawn behind them.
//
//   WrapNode  — the folded box: name, buttons (settings, open in place,
//               enter — coming next, unwrap, delete), one row per face port,
//               a one-line summary of the Pax values inside and (⚙) a panel
//               with every Pax parameter inside, grouped by node (v0.0.931).
//   WrapFrame — the open frame: dashed outline + title bar (fold, unwrap);
//               dragging the title bar moves the wrap's nodes (v0.0.930).

import { fadersOfSettings, faderLabel, maxValue } from './KbdFaders';   // v0.0.956
import { useContext, useEffect, useRef, useState } from 'react';
import { NodeProps, EdgeProps, useUpdateNodeInternals, Handle, Position, BaseEdge, getBezierPath } from '@xyflow/react';
import { Package, Maximize2, Minimize2, LogIn, Ungroup, X, Settings, Pin, BookmarkPlus, Power } from 'lucide-react';
import { Bridge } from './Bridge';
import type { RawWrap, RawWrapPin, PaxParamInfo } from './Bridge';
import { HintContext } from './HintPanel';
import { NodeHandle, NodeHeaderButton, EditableTitle, freqLabel, niceStep, readoutBoxStyle } from './NodeUtils';
import ValueBox from './ValueBox';   // v0.0.943

export const WRAP_ACCENT = 'var(--generic)';

export interface WrapPort {
  handleId: string;          // the inner port's own id
  label:    string;          // "MIDI In"
  nodeName: string;          // the inner node it belongs to
  dir:      'in' | 'out';
  colour:   string;
}

export interface WrapActions {
  open:   (wrapId: string) => void;
  close:  (wrapId: string) => void;
  unwrap: (wrapId: string) => void;
  remove: (wrapId: string) => void;
  rename: (wrapId: string, name: string) => void;
  // v0.0.931 — a Pax parameter edited from the wrap's panel: same messages as
  // the node's own controls (setNodeSettings + setPaxParameter while
  // dragging, commitNodeSettings at the end = one undo step)
  setParam:    (nodeId: string, index: number, value: number) => void;
  commitParam: (nodeId: string) => void;
  // v0.0.932 — the whole pin list (pin / unpin / rename = one undo step)
  setPins:     (wrapId: string, pins: RawWrapPin[]) => void;
  enter:       (wrapId: string) => void;   // v0.0.937 — the wrap on its own (⌘↑)
  saveToLibrary: (wrapId: string) => void; // v0.0.938 — into ~/Documents/Patchy/Wraps
  // v0.0.941 — open frame folded straight to its header (the box, header
  // only); unfolding that header goes back to the open frame
  foldHeader:  (wrapId: string) => void;
  // v0.0.949 — built-in nodes' controls (batch 1: Audio Player, DMX / ArtNet Console)
  playerTransport: (nodeId: string, cmd: 'toggle' | 'start', playing: boolean) => void;
  playerLevel:     (nodeId: string, level: number, commit: boolean) => void;
  consoleBlackout: (nodeId: string, kind: 'dmxConsole' | 'artNetConsole', on: boolean) => void;
  // v0.0.950 — console faders (user's idea): channel 1-512, value 0-255
  consoleFader:        (nodeId: string, kind: 'dmxConsole' | 'artNetConsole', ch: number, value: number, commit: boolean) => void;
  consoleFaderChannel: (nodeId: string, faders: number[], commit: boolean) => void;   // the whole list (channels; length = count)
  kbdFader:            (nodeId: string, k: number, value: number, commit: boolean) => void;   // v0.0.956 — MIDI Keyboard fader k
  // v0.0.951 — batch 2: MIDI Out channel filter, each node's ⏻, the wrap's ⏻
  midiOutChannels: (nodeId: string, channels: number[]) => void;
  setDisabled:     (nodeId: string, disabled: boolean) => void;
  wrapPower:       (wrapId: string) => void;
}

// v0.0.949 — a built-in node inside the wrap with controls worth reaching
// from the folded box. Controls by index (also the pin index):
//   Audio Player: 0 Transport (play / pause, back to start), 1 Level
//   DMX / ArtNet Console: 0 Blackout
export interface WrapBuiltin {
  nodeId:   string;
  kind:     'player' | 'dmxConsole' | 'artNetConsole' | 'midiOut' | 'morpher' | 'midiPlayer' | 'keyboard';   // v0.0.956 + MIDI Keyboard (faders)   // v0.0.951 — + MIDI Out, Morpher; v0.0.954 MIDI File Player
  name:     string;
  accent:   string;
  settings: Record<string, unknown>;   // the node's settingsJson, parsed
  disabled?: boolean;     // v0.0.951 — its ⏻ is off
  canDisable?: boolean;   // consoles have no ⏻
}
// v0.0.950 — a console also has 1-4 Faders (index 1-4), each on a channel of
// your choice (◀ [ch] ▶), remembered in the console's settings ("wrapFaders")
// v0.0.952 — the maximum is a Preference (1-16, default 4): App sets it
let maxWrapFaders = 4;
export const setMaxWrapFaders = (n: number) => { maxWrapFaders = Math.max (1, Math.min (16, Math.round (n || 4))); };
export const fadersOf = (b: WrapBuiltin): number[] => {
  const a = b.settings.wrapFaders;
  return Array.isArray (a) && a.length > 0
    ? a.slice (0, maxWrapFaders).map (n => Math.max (1, Math.min (512, Math.round (Number (n) || 1))))
    : [1];
};
export const controlsOf = (b: WrapBuiltin): string[] =>
  b.kind === 'player'  ? ['Transport', 'Level']
  : b.kind === 'midiPlayer' ? ['Transport']
  : b.kind === 'keyboard' ? fadersOfSettings (b.settings).map (faderLabel)   // v0.0.956 — its faders
  : b.kind === 'midiOut' ? ['Channels']
  : b.kind === 'morpher' ? []                       // its ⏻ (bypass) only, on its title
  : ['Blackout', ...fadersOf (b).map ((_, k) => `Fader ${k + 1}`)];
/** v0.0.951 — pin index of a node's ⏻ (any node with a ⏻ in the panel) */
export const POWER_PIN = -1;
/** A console's 512 channel values from its settings (base64), zeros when never set. */
export function consoleChannels (b: WrapBuiltin): Uint8Array {
  const out = new Uint8Array (512);
  const b64 = b.settings[b.kind === 'dmxConsole' ? 'dmxChannels' : 'artNetChannels'];
  if (typeof b64 !== 'string') return out;
  try { const bin = atob (b64); for (let i = 0; i < 512 && i < bin.length; i++) out[i] = bin.charCodeAt (i); } catch { /* zeros */ }
  return out;
}

// v0.0.931 — a generic Pax inside the wrap, with its current values
export interface WrapPax {
  nodeId: string;
  name:   string;            // custom name or label
  params: PaxParamInfo[];
  values: number[];          // one per param (settingsJson, else default)
  shown:  boolean[];         // param applies right now (e.g. Spectrumyser's active bands)
  choices: Record<number, string[]>;   // named buttons for a param (e.g. Amp / Band)
  pairs:   [number, number][];         // Low / High params drawn as one two-head slider
  zoom?:   [number, number];           // the node's own display range for those pairs
  accent: string;            // the Pax's own colour (its node header's)
  liveColour: string;        // its output wires' colour (live values)
  disabled?: boolean;        // v0.0.951 — its ⏻ is off
}

export interface WrapNodeData {
  wrap:    RawWrap;
  ports:   WrapPort[];
  pax:     WrapPax[];
  builtins?: WrapBuiltin[];   // v0.0.949
  power?:   { anyOn: boolean; count: number };   // v0.0.951 — the wrap's ⏻ (members that have one)
  actions: WrapActions;
  [key: string]: unknown;
}

// Shared value formatting (face summary + panel)
const isToggle  = (p: PaxParamInfo) => p.step >= 1 && p.min === 0 && p.max === 1;
const fmtValue  = (p: PaxParamInfo, v: number) =>
  isToggle (p) ? (v >= 0.5 ? 'On' : 'Off')
  : p.step >= 1 ? Math.round (v).toString()
  : v.toFixed (Math.abs (p.max - p.min) >= 100 ? 1 : 2);

// v0.0.933 — Low/High pairs: the role of param i, and the pair's label
// ("Band Low (Hz)" → "Band", "Band 2 Low Hz" → "Band 2")
function pairRole (x: WrapPax, i: number): { role: 'lo' | 'hi'; lo: number; hi: number } | null {
  for (const [lo, hi] of x.pairs) {
    if (i === lo) return { role: 'lo', lo, hi };
    if (i === hi) return { role: 'hi', lo, hi };
  }
  return null;
}
const pairLabel = (p: PaxParamInfo) => p.name.replace (/\s*Low\b.*$/i, '') || 'Band';
const isHz      = (p: PaxParamInfo) => /hz/i.test (p.name);
const fmtPairEnd = (p: PaxParamInfo, v: number) => isHz (p) ? freqLabel (v) : fmtValue (p, v);
const fmtChoice  = (x: WrapPax, i: number, p: PaxParamInfo, v: number) =>
  x.choices[i]?.[Math.round (v)] ?? fmtValue (p, v);

// Summary for the face: live (read-only) values first, each in its Pax's
// colour, then the settable ones in grey — "● Current Value 0.786 · Min 0.00
// · …" (up to 2 lines). A parameter name found in more than one Pax of the
// wrap is prefixed with its node's name; a Pax's only live value then shows
// the node's name alone ("● OSC TO VALUE 0.5"). Live values take the colour
// of the Pax's output wires (what the value IS once sent), settings grey.
interface SummaryPart { text: string; colour?: string }
function paxSummary (pax: WrapPax[], live: Map<string, number>, pinned: Set<string>) {
  const count = new Map<string, number>();
  for (const x of pax) for (const p of x.params) count.set (p.name, (count.get (p.name) ?? 0) + 1);
  const liveParts: SummaryPart[] = [], setParts: SummaryPart[] = [];
  for (const x of pax)
    x.params.forEach ((p, i) => {
      if (pinned.has (`${x.nodeId}:${i}`)) return;   // already on the box (v0.0.932)
      if (x.shown[i] === false) return;               // not in use (v0.0.933)
      const pr = pairRole (x, i);
      if (pr?.role === 'hi') return;                  // shown with its Low
      if (pr) {
        const hp = x.params[pr.hi];
        setParts.push ({ text: `${pairLabel (p)} ${fmtPairEnd (p, x.values[pr.lo] ?? p.defaultValue)}–${fmtPairEnd (hp, x.values[pr.hi] ?? hp.defaultValue)}` });
        return;
      }
      const v    = p.readOnly ? (live.get (`${x.nodeId}:${i}`) ?? x.values[i] ?? p.defaultValue) : (x.values[i] ?? p.defaultValue);
      const shared   = (count.get (p.name) ?? 0) > 1;
      const onlyLive = p.readOnly && x.params.filter (q => q.readOnly).length === 1;
      const name = ! shared ? p.name : onlyLive ? x.name.toUpperCase() : `${x.name.toUpperCase()} ${p.name}`;
      if (p.readOnly) liveParts.push ({ text: `● ${name} ${fmtValue (p, v)}`, colour: x.liveColour });
      else            setParts.push  ({ text: `${name} ${fmtChoice (x, i, p, v)}` });
    });
  const parts = [...liveParts, ...setParts];
  return { parts, full: parts.map (q => q.text).join (' · ') };
}

// ── One parameter in the wrap's panel (same kinds of control as the node) ───
// `labelNode` replaces the parameter name (the pinned controls on the box
// show their own, renamable label); `big` = a pinned live value, larger.
function WrapParamControl ({ x, p, i, live, actions, labelNode, big = false }: {
  x: WrapPax; p: PaxParamInfo; i: number; live: number | undefined; actions: WrapActions;
  labelNode?: React.ReactNode; big?: boolean;
}) {
  const { setHint } = useContext (HintContext);
  // v0.0.943 — ⌘ / Ctrl held while dragging a pair's head: the pair moves together
  const cmd = useRef (false);
  useEffect (() => {
    const k = (e: KeyboardEvent) => { if (e.key === 'Meta' || e.key === 'Control') cmd.current = e.type === 'keydown'; };
    window.addEventListener ('keydown', k); window.addEventListener ('keyup', k);
    return () => { window.removeEventListener ('keydown', k); window.removeEventListener ('keyup', k); };
  }, []);
  const v = x.values[i] ?? p.defaultValue;
  const row: React.CSSProperties = { display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 8 };
  const label = labelNode ?? <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{p.name}</span>;

  if (p.readOnly)
    return (
      <div style={row}>{label}
        <span style={{ fontSize: big ? 15 : 9, fontWeight: big ? 700 : 400, color: x.liveColour, fontVariantNumeric: 'tabular-nums' }}>
          {fmtValue (p, live ?? v)}
        </span>
      </div>
    );

  // Low / High pair → one two-head slider (Low pushes High and back, like
  // the nodes' own band sliders), within the node's display range
  const pr = pairRole (x, i);
  if (pr) {
    const hp = x.params[pr.hi];
    const lo = x.values[pr.lo] ?? p.defaultValue, hi = x.values[pr.hi] ?? hp.defaultValue;
    const [zMin, zMax] = x.zoom ?? [p.min, hp.max];
    const step = Math.max (p.step, 0) || (zMax - zMin) / 1000;
    const setLo = (v: number) => {
      const nl = Math.max (zMin, Math.min (v, zMax - step));
      actions.setParam (x.nodeId, pr.lo, nl);
      if (nl >= hi - step) actions.setParam (x.nodeId, pr.hi, Math.min (zMax, nl + step));
    };
    const setHi = (v: number) => {
      const nh = Math.min (zMax, Math.max (v, zMin + step));
      actions.setParam (x.nodeId, pr.hi, nh);
      if (nh <= lo + step) actions.setParam (x.nodeId, pr.lo, Math.max (zMin, nh - step));
    };
    const commit = () => actions.commitParam (x.nodeId);
    const boxStep = p.step >= 1 ? 1 : niceStep (zMax - zMin);
    const movePair = (dist: number, ratio: number) => {   // keeps the gap (Hz: ratio)
      let l = isHz (p) ? lo * ratio : lo + dist, h = isHz (p) ? hi * ratio : hi + dist;
      if (l < zMin) { h = isHz (p) ? h * zMin / l : h + (zMin - l); l = zMin; }
      if (h > zMax) { l = isHz (p) ? l * zMax / h : l - (h - zMax); h = zMax; }
      if (p.step >= 1) { l = Math.round (l); h = Math.round (h); }
      actions.setParam (x.nodeId, pr.lo, Math.max (zMin, l));
      actions.setParam (x.nodeId, pr.hi, Math.min (zMax, h));
    };
    return (
      <div>
        <div style={row}>{labelNode ?? <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{pairLabel (p)}</span>}
          {/* v0.0.943 — both ends are value boxes; ⌥ + drag moves the pair
              together (Hz: same ratio, otherwise same distance) */}
          <span style={{ display: 'inline-flex', alignItems: 'center', gap: 3, fontSize: 9, color: 'var(--text-dim)' }}>
            <ValueBox value={lo} min={zMin} max={hi} step={boxStep} log={isHz (p)} format={q => fmtPairEnd (p, q)} accent={x.accent}
              onLive={setLo} onCommit={q => { setLo (q); commit(); }} onRelease={commit}
              onLinked={q => movePair (q - lo, q / lo)} linkKey="cmd" linkMin={zMin} linkMax={zMax}
              width="auto" style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9, minWidth: 30 }} />
            –
            <ValueBox value={hi} min={lo} max={zMax} step={boxStep} log={isHz (p)} format={q => fmtPairEnd (hp, q)} accent={x.accent}
              onLive={setHi} onCommit={q => { setHi (q); commit(); }} onRelease={commit}
              onLinked={q => movePair (q - hi, q / hi)} linkKey="cmd" linkMin={zMin} linkMax={zMax}
              width="auto" style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9, minWidth: 30 }} />
            {isHz (p) ? ' Hz' : ''}
          </span>
        </div>
        <div className="dual-range nodrag"
          onMouseEnter={() => setHint ({ title: `${pairLabel (p)} — ${x.name}`, body: `Two heads: Low and High (they push each other); ⌘ + drag a head or a value moves both together. Range shown: the node's own zoom (${fmtPairEnd (p, zMin)}–${fmtPairEnd (hp, zMax)}), changed on the node. Double-click a head to reset it.` })}
          onMouseLeave={() => setHint (null)}
          style={{ position: 'relative', height: 16, marginTop: 2 }}>
          <input type="range" min={zMin} max={zMax} step={step} value={Math.min (zMax, Math.max (zMin, lo))}
            onMouseDown={e => { cmd.current = e.metaKey || e.ctrlKey; }}
            onChange={e => { const q = Number (e.target.value); if (cmd.current) movePair (q - lo, q / lo); else setLo (q); }} onMouseUp={commit} onKeyUp={commit}
            onDoubleClick={() => { setLo (p.defaultValue); commit(); }}
            style={{ width: '100%', top: 5, left: 0, ['--thumb-color' as any]: x.accent }} />
          <input type="range" min={zMin} max={zMax} step={step} value={Math.min (zMax, Math.max (zMin, hi))}
            onMouseDown={e => { cmd.current = e.metaKey || e.ctrlKey; }}
            onChange={e => { const q = Number (e.target.value); if (cmd.current) movePair (q - hi, q / hi); else setHi (q); }} onMouseUp={commit} onKeyUp={commit}
            onDoubleClick={() => { setHi (hp.defaultValue); commit(); }}
            style={{ width: '100%', top: 5, left: 0, ['--thumb-color' as any]: x.accent }} />
        </div>
      </div>
    );
  }

  // Named choices (e.g. Amp / Band) — as on the node
  const names = x.choices[i];
  if (names)
    return (
      <div style={row}>{label}
        <div style={{ display: 'flex', gap: 4 }}>
          {names.map ((t, val) => {
            const on = Math.round (v) === val;
            return (
              <div key={val} onClick={() => { actions.setParam (x.nodeId, i, val); actions.commitParam (x.nodeId); }}
                style={{ fontSize: 9, padding: '2px 6px', borderRadius: 3, cursor: 'pointer',
                         border: `1px solid ${on ? x.accent : 'var(--border)'}`,
                         color: on ? x.accent : 'var(--text-muted)',
                         background: on ? 'color-mix(in srgb, currentColor 15%, transparent)' : 'transparent' }}>
                {t}
              </div>
            );
          })}
        </div>
      </div>
    );

  if (isToggle (p))
    return (
      <div style={row}>{label}
        <div style={{ display: 'flex', gap: 4 }}>
          {['Off', 'On'].map ((t, val) => {
            const on = Math.round (v) === val;
            return (
              <div key={val} onClick={() => { actions.setParam (x.nodeId, i, val); actions.commitParam (x.nodeId); }}
                style={{ fontSize: 9, padding: '2px 6px', borderRadius: 3, cursor: 'pointer',
                         border: `1px solid ${on ? WRAP_ACCENT : 'var(--border)'}`,
                         color: on ? WRAP_ACCENT : 'var(--text-muted)',
                         background: on ? 'color-mix(in srgb, var(--generic) 15%, transparent)' : 'transparent' }}>
                {t}
              </div>
            );
          })}
        </div>
      </div>
    );

  if (p.name === 'DMX Channel')
    return (
      <div style={row}>{label}
        <ValueBox value={Math.round (v)} min={p.min} max={p.max} accent={WRAP_ACCENT} width={44}
          onLive={r => actions.setParam (x.nodeId, i, r)}
          onCommit={r => { actions.setParam (x.nodeId, i, r); actions.commitParam (x.nodeId); }}
          style={{ fontSize: 9, textAlign: 'right', color: WRAP_ACCENT }} />
      </div>
    );

  return (
    <div>
      <div style={row}>{label}
        <ValueBox value={v} min={p.min} max={p.max} step={p.step >= 1 ? 1 : niceStep (p.max - p.min)} format={q => fmtValue (p, q)}
          accent={WRAP_ACCENT} onLive={q => actions.setParam (x.nodeId, i, q)}
          onCommit={q => { actions.setParam (x.nodeId, i, q); actions.commitParam (x.nodeId); }}
          width={56} style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9 }} />
      </div>
      <input type="range" className="nodrag" min={p.min} max={p.max} step={(p.max - p.min) / 1000} value={v}
        onChange={e => { let nv = parseFloat (e.target.value); if (p.step >= 1) nv = Math.round (nv); actions.setParam (x.nodeId, i, nv); }}
        onMouseUp={() => actions.commitParam (x.nodeId)}
        onKeyUp={() => actions.commitParam (x.nodeId)}
        onDoubleClick={() => { actions.setParam (x.nodeId, i, p.defaultValue); actions.commitParam (x.nodeId); }}
        onMouseEnter={() => setHint ({ title: `${p.name} — ${x.name}`, body: `Range: ${p.min} to ${p.max}${p.step >= 1 ? ' (integer)' : ''}.\nDouble-click to reset to default (${p.defaultValue}). Same control as on the node inside the wrap.` })}
        onMouseLeave={() => setHint (null)}
        style={{ width: '100%', display: 'block', height: 18, margin: 0, ['--thumb-color' as any]: WRAP_ACCENT }} />
    </div>
  );
}

const titleStyle: React.CSSProperties = {
  fontSize: 11, fontWeight: 700, color: 'var(--text)', letterSpacing: '0.1em',
  fontFamily: "'Syne', sans-serif",
};

// ── v0.0.951 — a node's ⏻ in the wrap (its title in the panel, or pinned) ───
function PowerToggle ({ disabled, name, onToggle, size = 10 }: {
  disabled: boolean; name: string; onToggle: () => void; size?: number;
}) {
  const { setHint } = useContext (HintContext);
  return (
    <span onClick={onToggle}
      onMouseEnter={() => setHint ({ title: disabled ? 'Enable' : 'Disable', body: `${name}: ${disabled ? 'switch back on' : 'switch off'} — same as the node's own ⏻ (cut or pass-through, depending on the node). Undoable.` })}
      onMouseLeave={() => setHint (null)}
      style={{ cursor: 'pointer', display: 'inline-flex', alignItems: 'center', color: disabled ? 'var(--text-muted)' : WRAP_ACCENT,
               opacity: disabled ? 0.55 : 1 }}>
      <Power size={size} />
    </span>
  );
}

// ── v0.0.949 — one built-in node's control in the wrap (panel or pinned) ────
const LEVEL_MIN_DB = -60;   // same mapping as the Audio Player's own level slider
const levelToDb = (l: number) => l <= 0 ? LEVEL_MIN_DB : Math.max (LEVEL_MIN_DB, 20 * Math.log10 (l));
const dbToLevel = (db: number) => db <= LEVEL_MIN_DB ? 0 : Math.pow (10, db / 20);
const fmtDb = (db: number) => db <= LEVEL_MIN_DB ? '-∞' : db.toFixed (1);

function BuiltinControl ({ b, i, actions, playing, labelNode }: {
  b: WrapBuiltin; i: number; actions: WrapActions; playing: boolean; labelNode?: React.ReactNode;
}) {
  const { setHint } = useContext (HintContext);
  const row: React.CSSProperties = { display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 8 };
  const label = labelNode ?? <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{controlsOf (b)[i]}</span>;
  const btn = (on: boolean, accent: string): React.CSSProperties => ({
    fontSize: 9, padding: '2px 8px', borderRadius: 3, cursor: 'pointer', display: 'inline-flex', alignItems: 'center', gap: 4,
    border: `1px solid ${on ? accent : 'var(--border)'}`, color: on ? accent : 'var(--text-muted)',
    background: on ? 'color-mix(in srgb, currentColor 15%, transparent)' : 'transparent',
  });
  const tip = (title: string, body: string) => ({ onMouseEnter: () => setHint ({ title, body }), onMouseLeave: () => setHint (null) });

  if ((b.kind === 'player' || b.kind === 'midiPlayer') && i === 0)
    return (
      <div style={row}>{label}
        <div style={{ display: 'flex', gap: 4 }}>
          <span onClick={() => actions.playerTransport (b.nodeId, 'start', playing)} style={btn (false, b.accent)}
            {...tip ('Back to start', `${b.name}: back to the beginning of the file.`)}>⏮</span>
          <span onClick={() => actions.playerTransport (b.nodeId, 'toggle', playing)} style={btn (playing, b.accent)}
            {...tip (playing ? 'Pause' : 'Play', `${b.name}: ${playing ? 'pause' : 'start'} playback — same as on the node.`)}>
            {playing ? '❚❚ Pause' : '▶ Play'}
          </span>
        </div>
      </div>
    );

  if (b.kind === 'player' && i === 1) {
    const lvl = typeof b.settings.level === 'number' ? b.settings.level as number : 0.1;
    const db  = levelToDb (lvl);
    return (
      <div>
        <div style={row}>{label}
          <ValueBox value={db} min={LEVEL_MIN_DB} max={0} step={0.5} fineStep={0.1} format={fmtDb} minWord="-∞" accent={b.accent}
            parse={t => /inf|∞/i.test (t) ? LEVEL_MIN_DB : parseFloat (t.replace (',', '.'))}
            onLive={v => actions.playerLevel (b.nodeId, dbToLevel (v), false)}
            onCommit={v => actions.playerLevel (b.nodeId, dbToLevel (v), true)}
            width={48} style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9 }} />
        </div>
        <input type="range" className="nodrag" min={LEVEL_MIN_DB} max={0} step="any" value={db}
          onChange={e => actions.playerLevel (b.nodeId, dbToLevel (parseFloat (e.target.value)), false)}
          onMouseUp={() => actions.playerLevel (b.nodeId, dbToLevel (db), true)}
          onDoubleClick={() => actions.playerLevel (b.nodeId, 0.1, true)}
          {...tip (`Level — ${b.name}`, `${fmtDb (db)} dB. Double-click to reset. Same control as on the node.`)}
          style={{ width: '100%', display: 'block', height: 18, margin: 0, ['--thumb-color' as any]: b.accent }} />
      </div>
    );
  }

  // v0.0.951 — MIDI Out: its channel filter (16 buttons; none = all channels)
  if (b.kind === 'midiOut') {
    const chans = Array.isArray (b.settings.channels) ? (b.settings.channels as number[]) : [];
    return (
      <div>
        <div style={row}>{label}
          <span style={{ fontSize: 9, color: 'var(--text-dim)' }}>{chans.length === 0 ? 'Omni' : `Ch ${chans.join (', ')}`}</span>
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 2, marginTop: 3 }}
          {...tip (`Channels — ${b.name}`, 'Which channels reach the device; none lit = all channels (Omni). Same buttons as the node\'s own channel panel. Undoable.')}>
          {Array.from ({ length: 16 }, (_, k) => k + 1).map (c => {
            const on = chans.includes (c);
            return (
              <span key={c} onClick={() => actions.midiOutChannels (b.nodeId, on ? chans.filter (x => x !== c) : [...chans, c].sort ((x, y) => x - y))}
                style={{ width: 17, height: 15, display: 'inline-flex', alignItems: 'center', justifyContent: 'center', fontSize: 8,
                         borderRadius: 2, cursor: 'pointer', userSelect: 'none',
                         background: on ? b.accent : 'var(--surface)', color: on ? '#000' : 'var(--text-muted)',
                         border: `1px solid ${on ? b.accent : 'var(--border)'}` }}>{c}</span>
            );
          })}
        </div>
      </div>
    );
  }

  // v0.0.956 — MIDI Keyboard: fader i (label, value, slider)
  if (b.kind === 'keyboard') {
    const f = fadersOfSettings (b.settings)[i];
    if (! f) return null;
    const max = maxValue (f.type);
    return (
      <div>
        <div style={row}>{label}
          <ValueBox value={f.value} min={0} max={max} accent={b.accent} coarse={max > 127 ? 64 : 1}
            onLive={v => actions.kbdFader (b.nodeId, i, v, false)}
            onCommit={v => actions.kbdFader (b.nodeId, i, v, true)}
            width={max > 127 ? 44 : 34} style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9 }} />
        </div>
        <input type="range" className="nodrag" min={0} max={max} step={1} value={f.value}
          onChange={e => actions.kbdFader (b.nodeId, i, Number (e.target.value), false)}
          onMouseUp={() => actions.kbdFader (b.nodeId, i, f.value, true)}
          onDoubleClick={() => actions.kbdFader (b.nodeId, i, 0, true)}
          {...tip (`${faderLabel (f)} — ${b.name}`, `${f.value} (0-${max}). Double-click for 0. Same fader as on the Keyboard.`)}
          style={{ width: '100%', display: 'block', height: 18, margin: 0, ['--thumb-color' as any]: b.accent }} />
      </div>
    );
  }

  // DMX / ArtNet Console: Fader k (i = k + 1) — ◀ [channel] ▶, value, slider
  if (i >= 1) {
    const kind   = b.kind as 'dmxConsole' | 'artNetConsole';
    const faders = fadersOf (b);
    const k = i - 1, ch = faders[k] ?? 1;
    const val = consoleChannels (b)[ch - 1] ?? 0;
    const setCh = (c: number, commit: boolean) => {
      const next = [...faders]; next[k] = Math.max (1, Math.min (512, Math.round (c)));
      actions.consoleFaderChannel (b.nodeId, next, commit);
    };
    const arrow = (dir: -1 | 1) => (
      <span onClick={() => setCh (ch + dir, true)} {...tip (dir < 0 ? 'Previous channel' : 'Next channel', 'Picks which console channel this fader drives (1-512); remembered with the project.')}
        style={{ cursor: 'pointer', fontSize: 9, color: 'var(--text-muted)', padding: '0 2px', userSelect: 'none' }}>{dir < 0 ? '◀' : '▶'}</span>
    );
    return (
      <div>
        <div style={row}>{label}
          <span style={{ display: 'inline-flex', alignItems: 'center', gap: 3 }}>
            {arrow (-1)}
            <ValueBox value={ch} min={1} max={512} format={c => `Ch ${c}`} accent={b.accent}
              parse={t => parseInt (t.replace (/[^0-9]/g, ''), 10)}
              onLive={c => setCh (c, false)} onCommit={c => setCh (c, true)}
              width={46} style={{ fontSize: 9, padding: '1px 2px' }} />
            {arrow (1)}
            <ValueBox value={val} min={0} max={255} accent={b.accent}
              onLive={v => actions.consoleFader (b.nodeId, kind, ch, v, false)}
              onCommit={v => actions.consoleFader (b.nodeId, kind, ch, v, true)}
              width={34} style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9 }} />
          </span>
        </div>
        <input type="range" className="nodrag" min={0} max={255} step={1} value={val}
          onChange={e => actions.consoleFader (b.nodeId, kind, ch, Number (e.target.value), false)}
          onMouseUp={() => actions.consoleFader (b.nodeId, kind, ch, val, true)}
          onDoubleClick={() => actions.consoleFader (b.nodeId, kind, ch, 0, true)}
          {...tip (`Ch ${ch} — ${b.name}`, `${val} (0-255). Double-click for 0. Same channel as on the console.`)}
          style={{ width: '100%', display: 'block', height: 18, margin: 0, ['--thumb-color' as any]: b.accent }} />
      </div>
    );
  }

  // DMX / ArtNet Console: Blackout
  const on = b.settings.blackout === true;
  return (
    <div style={row}>{label}
      <span onClick={() => actions.consoleBlackout (b.nodeId, b.kind as 'dmxConsole' | 'artNetConsole', ! on)}
        style={btn (on, '#ef4444')}
        {...tip ('Blackout', `${b.name}: all of its channels to 0 while on — same as the console's own Blackout. Undoable.`)}>
        {on ? '■ Blackout on' : 'Blackout'}
      </span>
    </div>
  );
}

// ── Folded wrap ──────────────────────────────────────────────────────────────
export function WrapNode ({ id, data, selected }: NodeProps) {
  const { wrap, ports, pax, actions, power } = data as WrapNodeData;
  const builtins = (data as WrapNodeData).builtins ?? [];   // v0.0.949
  const { setHint } = useContext (HintContext);
  const updateNodeInternals = useUpdateNodeInternals();
  const [showSettings, setShowSettings] = useState (false);

  // v0.0.941 — fold to the header like any node (double-click the header, or
  // the global ⇕ fold), not saved — same as nodes. Ports then sit on the header.
  // fromOpen: folded to the header straight from the open frame — starts
  // folded, and unfolding it reopens the frame (where it came from)
  // (derived, not state: nothing can unfold it but its own header / ⤢)
  const fromOpen = !! (data as { fromOpen?: boolean }).fromOpen;
  const [ownCollapsed, setCollapsed] = useState (false);
  const forceCollapsed = (data as { _forceCollapsed?: boolean })._forceCollapsed;
  useEffect (() => { if (forceCollapsed !== undefined) setCollapsed (forceCollapsed); }, [forceCollapsed]);
  const collapsed = fromOpen || ownCollapsed;
  const toggleHeader = () => {
    if (fromOpen) { actions.open (wrap.id); return; }   // back to the open frame
    setCollapsed (c => ! c);
  };
  useEffect (() => { const t = setTimeout (() => updateNodeInternals (id), 1); return () => clearTimeout (t); },
             [collapsed, id, updateNodeInternals]);

  // Live values of read-only Pax parameters (summary + panel); followed only
  // when the wrap holds at least one
  const [live, setLive] = useState<Map<string, number>>(new Map());
  const paxKey  = pax.map (x => x.nodeId).join ('|');
  const hasLive = pax.some (x => x.params.some (p => p.readOnly));
  // v0.0.932 — pinned controls, in pin order (pins of nodes no longer in the
  // wrap, or of parameters that no longer exist, are skipped)
  const pins: RawWrapPin[] = wrap.pins ?? [];
  const pinKey    = (nodeId: string, index: number) => `${nodeId}:${index}`;
  const pinnedSet = new Set (pins.map (q => pinKey (q.nodeId, q.index)));
  const pinned = pins.flatMap (q => {
    const x = pax.find (y => y.nodeId === q.nodeId);
    const p = x?.params[q.index];
    return x && p && x.shown[q.index] !== false ? [{ q, x, p }] : [];
  });
  // v0.0.949 — pinned built-in controls (kept in pin order with the Pax ones)
  const pinnedB = pins.flatMap (q => {
    const b = builtins.find (y => y.nodeId === q.nodeId);
    return b && controlsOf (b)[q.index] ? [{ q, b }] : [];
  });
  const pinOrder = (k: string) => pins.findIndex (q => pinKey (q.nodeId, q.index) === k);
  // v0.0.953 — reorder the pinned controls, list-style (user): the grabbed row
  // follows the pointer, the others slide aside to open its gap; drop = one
  // undo step. Pointer events (not HTML5 drag, which can't animate the rest).
  const rowRefs = useRef<(HTMLDivElement | null)[]> ([]);
  const [drag, setDrag] = useState<{ from: number; dy: number; heights: number[] } | null> (null);
  const targetOf = (d: { from: number; dy: number; heights: number[] }) => {
    const tops: number[] = []; let acc = 0;
    for (const h of d.heights) { tops.push (acc); acc += h; }
    const centre = tops[d.from] + d.heights[d.from] / 2 + d.dy;
    let t = d.from;
    for (let j = d.from + 1; j < d.heights.length; j++) if (centre > tops[j] + d.heights[j] / 2) t = j;
    for (let j = d.from - 1; j >= 0; j--)               if (centre < tops[j] + d.heights[j] / 2) t = j;
    return t;
  };
  const movePin = (from: string, to: string) => {
    const fi = pinOrder (from), ti = pinOrder (to);
    if (fi < 0 || ti < 0 || fi === ti) return;
    const next = [...pins]; const [m] = next.splice (fi, 1); next.splice (ti, 0, m);
    actions.setPins (wrap.id, next);
  };
  const togglePin = (nodeId: string, index: number) =>
    actions.setPins (wrap.id, pinnedSet.has (pinKey (nodeId, index))
      ? pins.filter (q => pinKey (q.nodeId, q.index) !== pinKey (nodeId, index))
      : [...pins, { nodeId, index, label: '' }]);
  const renamePin = (nodeId: string, index: number, label: string) =>
    actions.setPins (wrap.id, pins.map (q => q.nodeId === nodeId && q.index === index ? { ...q, label } : q));

  // v0.0.951 — a node's title in the panel: name, then its ⏻ and the 📌 to
  // pin that ⏻ on the box (any node that has one)
  const titleRow = (nodeId: string, name: string, accent: string, power: boolean, disabled: boolean) => {
    const on = pinnedSet.has (pinKey (nodeId, POWER_PIN));
    return (
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 2 }}>
        {power && (
          <span onClick={() => togglePin (nodeId, POWER_PIN)}
            {...hint (on ? 'Unpin' : 'Pin on the box', on ? 'Takes this ⏻ off the folded box (it stays here). Undoable.'
                                                         : 'Shows this node\'s ⏻ on the folded box itself. Undoable.')}
            style={{ cursor: 'pointer', display: 'inline-flex', color: on ? WRAP_ACCENT : 'var(--text-muted)', opacity: on ? 1 : 0.45 }}>
            <Pin size={10} fill={on ? 'currentColor' : 'none'} />
          </span>
        )}
        <span style={{ fontSize: 9, fontWeight: 700, letterSpacing: '0.08em', color: accent, textTransform: 'uppercase',
                       opacity: disabled ? 0.5 : 1 }}>{name}</span>
        {power && <PowerToggle disabled={disabled} name={name} onToggle={() => actions.setDisabled (nodeId, ! disabled)} />}
      </div>
    );
  };
  // pinned ⏻s: any node of the wrap that's in the panel and has one
  const powerNodes = [
    ...pax.map (x => ({ nodeId: x.nodeId, name: x.name, disabled: !! x.disabled })),
    ...builtins.filter (b => b.canDisable).map (b => ({ nodeId: b.nodeId, name: b.name, disabled: !! b.disabled })),
  ];
  const pinnedP = pins.flatMap (q => {
    if (q.index !== POWER_PIN) return [];
    const nd = powerNodes.find (y => y.nodeId === q.nodeId);
    return nd ? [{ q, nd }] : [];
  });

  const summary0 = paxSummary (pax, live, pinnedSet);
  // v0.0.949 — built-ins in the summary: a playing Player, a console in blackout
  const [playingMap, setPlayingMap] = useState<Map<string, boolean>> (new Map());
  const playerKey = builtins.filter (b => b.kind === 'player' || b.kind === 'midiPlayer').map (b => b.nodeId).join ('|');
  useEffect (() => {
    if (! playerKey) return;
    const ids = new Set (playerKey.split ('|'));
    const onStatus = (st: { nodeId: string; playing: boolean }[]) => {
      const got = st.filter (x => ids.has (x.nodeId));
      if (got.length === 0) return;
      setPlayingMap (prev => {
        if (got.every (x => prev.get (x.nodeId) === x.playing)) return prev;
        const next = new Map (prev); got.forEach (x => next.set (x.nodeId, x.playing)); return next;
      });
    };
    const u1 = Bridge.onAudioPlayerStatus (onStatus);
    const u2 = Bridge.onMidiFilePlayerStatus (onStatus);   // v0.0.954
    return () => { u1(); u2(); };
  }, [playerKey]);
  // v0.0.951 — nodes switched off (unless their ⏻ is pinned)
  const offParts: SummaryPart[] = [
    ...pax.filter (x => x.disabled).map (x => ({ nodeId: x.nodeId, name: x.name })),
    ...builtins.filter (b => b.canDisable && b.disabled).map (b => ({ nodeId: b.nodeId, name: b.name })),
  ].filter (z => ! pinnedSet.has (pinKey (z.nodeId, POWER_PIN))).map (z => ({ text: `○ ${z.name} off` }));
  const builtinParts: SummaryPart[] = builtins.flatMap (b =>
    b.kind === 'player' || b.kind === 'midiPlayer' ? (pinnedSet.has (pinKey (b.nodeId, 0)) ? [] : [{ text: `${playingMap.get (b.nodeId) ? '▶' : '❚❚'} ${b.name}` }])
    : b.settings.blackout === true && ! pinnedSet.has (pinKey (b.nodeId, 0)) ? [{ text: `■ ${b.name} blackout`, colour: '#ef4444' }] : []);
  const summary = { parts: [...offParts, ...summary0.parts, ...builtinParts],
                    full: [...offParts, ...summary0.parts, ...builtinParts].map (q => q.text).join (' · ') };

  // Hovering the summary shows it in full in the hint panel, refreshed with
  // the live values (a native title tooltip is a frozen snapshot)
  const [summaryHover, setSummaryHover] = useState (false);
  useEffect (() => {
    if (summaryHover) setHint ({ title: `${wrap.name || 'WRAP'} — values`, body: summary.parts.map (q => q.text).join ('\n') });
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [summaryHover, summary.full]);
  // the summary hides when the panel opens (no mouseleave then)
  useEffect (() => { if (showSettings && summaryHover) { setSummaryHover (false); setHint (null); } },
  // eslint-disable-next-line react-hooks/exhaustive-deps
  [showSettings]);
  useEffect (() => {
    if (! hasLive) return;
    const ids = new Set (pax.map (x => x.nodeId));
    return Bridge.onPortActivity (entries => {
      const got = entries.filter (e => ids.has (e.id) && e.paxReadOnly?.length);
      if (got.length === 0) return;
      setLive (prev => {
        const next = new Map (prev);
        for (const e of got) for (const r of e.paxReadOnly) next.set (`${e.id}:${r.index}`, r.value);
        return next;
      });
    });
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [hasLive, paxKey]);

  const ins  = ports.filter (p => p.dir === 'in');
  const outs = ports.filter (p => p.dir === 'out');
  const rows = Math.max (ins.length, outs.length, 1);

  // Handles appear/disappear with the face ports: tell ReactFlow
  const portsKey = ports.map (p => p.handleId).join ('|');
  useEffect (() => { updateNodeInternals (id); }, [id, portsKey, updateNodeInternals]);

  const hint = (title: string, body: string) => ({
    onMouseEnter: () => setHint ({ title, body }), onMouseLeave: () => setHint (null),
  });

  const portLabel = (p: WrapPort | undefined, align: 'left' | 'right') => p ? (
    <div style={{ textAlign: align, minWidth: 0 }}>
      <div style={{ fontSize: 10, color: 'var(--text)', whiteSpace: 'nowrap' }}>{p.label}</div>
      <div style={{ fontSize: 8, color: 'var(--text-muted)', whiteSpace: 'nowrap', overflow: 'hidden',
                    textOverflow: 'ellipsis', maxWidth: 120 }}>{p.nodeName}</div>
    </div>
  ) : <div />;

  return (
    <div onDoubleClick={() => actions.open (wrap.id)} style={{
      minWidth: 220, background: 'var(--surface)',
      border: `1px solid ${selected ? WRAP_ACCENT : 'var(--border-hi)'}`, borderTop: `3px solid ${WRAP_ACCENT}`,
      borderRadius: 'var(--radius)', position: 'relative', fontFamily: "'JetBrains Mono', monospace",
      boxShadow: selected ? `0 0 0 1px ${WRAP_ACCENT}, 0 8px 32px var(--generic-glow)`
                          : '0 4px 16px rgba(0,0,0,.5), 4px 4px 0 -1px var(--surface2), 4px 4px 0 0 var(--border)',
    }}>
      {ports.map ((p, i) => (
        <NodeHandle key={p.handleId} nodeId={id} portId={p.handleId} label={p.label} direction={p.dir}
          colour={p.colour} index={0} total={1}
          anchor={`row${(p.dir === 'in' ? ins : outs).indexOf (p)}`} />
      ))}

      {/* Header — double-click folds it to the header (v0.0.941); the body's
          double-click still opens the wrap in place */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 4, padding: '6px 8px', cursor: 'pointer',
                    borderBottom: collapsed ? 'none' : '1px solid var(--border)', userSelect: 'none' }}
        onDoubleClick={e => { e.stopPropagation(); toggleHeader(); }}
        onMouseEnter={e => { if (! e.currentTarget.contains (e.relatedTarget as Node))
          setHint ({ title: 'Wrap', body: 'Several nodes shown as one box. Its ports are the connections that cross its border. Double-click (or ⤢) to open it in place. Nothing changes in the processing: wrapping only tidies the canvas.' }); }}
        onMouseLeave={e => { if (! e.currentTarget.contains (e.relatedTarget as Node)) setHint (null); }}>
        <FoldTriangle collapsed={collapsed} colour={WRAP_ACCENT} />
        <Package size={12} color={WRAP_ACCENT} />
        <EditableTitle display={wrap.name || 'WRAP'} value={wrap.name === 'WRAP' ? '' : wrap.name} placeholder="Wrap"
          onCommit={name => actions.rename (wrap.id, name || 'WRAP')} textStyle={titleStyle} />
        <span style={{ fontSize: 9, color: 'var(--text-muted)', marginLeft: 2 }}>{wrap.nodeIds.length} nodes</span>
        <div style={{ flex: 1 }} />
        <div style={{ display: 'flex', gap: 4 }} onDoubleClick={e => e.stopPropagation()}>
          {(pax.length > 0 || builtins.length > 0) && ! collapsed && (
            <NodeHeaderButton onClick={() => setShowSettings (v => ! v)} active={showSettings} activeAccent={WRAP_ACCENT}
              onHint={hint ('Wrap settings', 'Every Pax parameter inside this wrap, grouped by node — the same controls as on the nodes themselves (one undo step per change).')}>
              <Settings size={11} color={showSettings ? WRAP_ACCENT : 'var(--text-muted)'} />
            </NodeHeaderButton>
          )}
          <NodeHeaderButton onClick={() => actions.open (wrap.id)}
            onHint={hint ('Open in place', 'Shows the nodes inside this wrap, where they are, in a frame. Fold it back with the frame\'s ⤡ button.')}>
            <Maximize2 size={13} />
          </NodeHeaderButton>
          <NodeHeaderButton onClick={() => actions.saveToLibrary (wrap.id)}
            onHint={hint ('Save to library', 'Saves this wrap (its nodes, inner wires, settings, pins) to your wrap library under its name — then drag it from the sidebar\'s Wraps section into any project. Asks before replacing one of the same name.')}>
            <BookmarkPlus size={13} />
          </NodeHeaderButton>
          <NodeHeaderButton onClick={() => actions.enter (wrap.id)}
            onHint={hint ('Enter', 'Shows only this wrap\'s nodes, its outside connections on rails at the sides (⌘↑). Back with "Main" at the top, ⌘↓ or Esc.')}>
            <LogIn size={13} />
          </NodeHeaderButton>
          {power && power.count > 0 && (   // v0.0.951 — the wrap's ⏻
            <NodeHeaderButton onClick={() => actions.wrapPower (wrap.id)} active={power.anyOn} activeAccent={WRAP_ACCENT}
              onHint={hint (power.anyOn ? 'Switch the wrap off' : 'Switch the wrap on',
                `${power.anyOn ? 'Switches off' : 'Switches back on'} every node inside that has a ⏻ (${power.count}) — each as its own ⏻ does (cut or pass-through). One undo step.`)}>
              <Power size={11} />
            </NodeHeaderButton>
          )}
          <NodeHeaderButton onClick={() => actions.unwrap (wrap.id)}
            onHint={hint ('Unwrap', 'Removes the wrap and puts its nodes back on the canvas as they are (⌘⇧G). Undoable.')}>
            <Ungroup size={13} />
          </NodeHeaderButton>
          <NodeHeaderButton onClick={() => actions.remove (wrap.id)} danger
            onHint={hint ('Delete wrap', 'Deletes the wrap AND the nodes inside it. Undoable (⌘Z).')}>
            <X size={13} />
          </NodeHeaderButton>
        </div>
      </div>

      {/* Face ports: inputs left, outputs right */}
      {! collapsed && <>
      <div style={{ padding: '6px 10px 8px' }}>
        {ports.length === 0 && (
          <div style={{ fontSize: 9, color: 'var(--text-muted)', padding: '4px 0' }}>no connections across the wrap's border</div>
        )}
        {ports.length > 0 && Array.from ({ length: rows }, (_, i) => (
          <div key={i} data-port-anchor={`row${i}`}
            style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 12, alignItems: 'center', minHeight: 26 }}>
            {portLabel (ins[i], 'left')}
            {portLabel (outs[i], 'right')}
          </div>
        ))}
        {/* 📌 Pinned controls (v0.0.932): working controls, renamable label,
            unpin with their own 📌. Hidden while ⚙ is open (like the summary)
            so pinning there never moves the list under the cursor. */}
        {(pinned.length > 0 || pinnedB.length > 0 || pinnedP.length > 0) && ! showSettings && (
          <div className="nodrag" onDoubleClick={e => e.stopPropagation()}
            style={{ marginTop: ports.length > 0 ? 6 : 0, paddingTop: 4, borderTop: '1px dashed var(--border)', minWidth: 220 }}>
            {[...pinned.map (e => ({ k: pinKey (e.q.nodeId, e.q.index), el: (() => { const { q, x, p } = e;
              const def = `${pairRole (x, q.index) ? pairLabel (p) : p.name} · ${x.name}`;
              return (
                <div key={pinKey (q.nodeId, q.index)} style={{ padding: '3px 0', display: 'flex', alignItems: 'center', gap: 6 }}>
                  <span onClick={() => togglePin (q.nodeId, q.index)}
                    {...hint ('Unpin', 'Takes this control off the box (it stays in the ⚙ panel). Undoable.')}
                    style={{ cursor: 'pointer', display: 'inline-flex', flexShrink: 0, color: WRAP_ACCENT }}>
                    <Pin size={10} fill="currentColor" />
                  </span>
                  <div style={{ flex: 1, minWidth: 0 }}>
                  <WrapParamControl x={x} p={p} i={q.index} live={live.get (pinKey (x.nodeId, q.index))} actions={actions}
                    big={p.readOnly}
                    labelNode={
                      <EditableTitle display={q.label || def} value={q.label} placeholder={def}
                        onCommit={name => renamePin (q.nodeId, q.index, name)}
                        textStyle={{ fontSize: 9, color: p.readOnly ? x.liveColour : 'var(--text-muted)', letterSpacing: '0.04em' }} />
                    } />
                  </div>
                </div>
              ); }) () })),
              // v0.0.949 — pinned built-in controls
              ...pinnedB.map (({ q, b }) => {
                const def = `${controlsOf (b)[q.index]} · ${b.name}`;
                return { k: pinKey (q.nodeId, q.index), el: (
                  <div key={pinKey (q.nodeId, q.index)} style={{ padding: '3px 0', display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span onClick={() => togglePin (q.nodeId, q.index)}
                      {...hint ('Unpin', 'Takes this control off the box (it stays in the ⚙ panel). Undoable.')}
                      style={{ cursor: 'pointer', display: 'inline-flex', flexShrink: 0, color: WRAP_ACCENT }}>
                      <Pin size={10} fill="currentColor" />
                    </span>
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <BuiltinControl b={b} i={q.index} actions={actions} playing={playingMap.get (b.nodeId) ?? false}
                        labelNode={
                          <EditableTitle display={q.label || def} value={q.label} placeholder={def}
                            onCommit={name => renamePin (q.nodeId, q.index, name)}
                            textStyle={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.04em' }} />
                        } />
                    </div>
                  </div>
                ) };
              }),
              // v0.0.951 — pinned ⏻s
              ...pinnedP.map (({ q, nd }) => {
                const def = nd.name;
                return { k: pinKey (q.nodeId, q.index), el: (
                  <div key={pinKey (q.nodeId, q.index)} style={{ padding: '3px 0', display: 'flex', alignItems: 'center', gap: 6 }}>
                    <span onClick={() => togglePin (q.nodeId, q.index)}
                      {...hint ('Unpin', 'Takes this ⏻ off the box (it stays in the ⚙ panel). Undoable.')}
                      style={{ cursor: 'pointer', display: 'inline-flex', flexShrink: 0, color: WRAP_ACCENT }}>
                      <Pin size={10} fill="currentColor" />
                    </span>
                    <div style={{ flex: 1, minWidth: 0, display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 8 }}>
                      <EditableTitle display={q.label || def} value={q.label} placeholder={def}
                        onCommit={name => renamePin (q.nodeId, q.index, name)}
                        textStyle={{ fontSize: 9, color: nd.disabled ? 'var(--text-muted)' : 'var(--text-dim)', letterSpacing: '0.04em' }} />
                      <PowerToggle disabled={nd.disabled} name={nd.name} size={12} onToggle={() => actions.setDisabled (nd.nodeId, ! nd.disabled)} />
                    </div>
                  </div>
                ) };
              })].sort ((a, b) => pinOrder (a.k) - pinOrder (b.k)).map ((e, idx, all) => {
                // v0.0.953 — the grabbed row follows the pointer; rows between
                // its place and the target slide by its height to open the gap
                let ty = 0;
                if (drag) {
                  const t = targetOf (drag), h = drag.heights[drag.from] ?? 0;
                  if (idx === drag.from) ty = drag.dy;
                  else if (drag.from < t && idx > drag.from && idx <= t) ty = -h;
                  else if (drag.from > t && idx >= t && idx < drag.from) ty = h;
                }
                const grabbed = drag?.from === idx;
                const onGrip = (ev: React.PointerEvent) => {
                  ev.preventDefault(); ev.stopPropagation();
                  // node pixels, not screen pixels: the canvas is zoomed (the row
                  // followed the cursor too fast / too slow otherwise)
                  const heights = all.map ((_, j) => rowRefs.current[j]?.offsetHeight ?? 0);
                  const own = rowRefs.current[idx];
                  const zoom = own && own.offsetHeight > 0 ? own.getBoundingClientRect().height / own.offsetHeight : 1;
                  const y0 = ev.clientY;
                  let cur = { from: idx, dy: 0, heights };
                  setDrag (cur);
                  const move = (m: PointerEvent) => { cur = { ...cur, dy: (m.clientY - y0) / zoom }; setDrag (cur); };
                  const up = () => {
                    window.removeEventListener ('pointermove', move, true);
                    window.removeEventListener ('pointerup',   up,   true);
                    const t = targetOf (cur);
                    setDrag (null);
                    if (t !== cur.from) movePin (all[cur.from].k, all[t].k);
                  };
                  window.addEventListener ('pointermove', move, true);
                  window.addEventListener ('pointerup',   up,   true);
                };
                return (
                  <div key={e.k} ref={el => { rowRefs.current[idx] = el; }}
                    style={{ display: 'flex', alignItems: 'center', gap: 3, position: 'relative',
                             transform: `translateY(${ty}px)`,
                             transition: grabbed || ! drag ? 'none' : 'transform 150ms ease',
                             zIndex: grabbed ? 5 : 'auto',
                             background: grabbed ? 'var(--surface2)' : 'transparent',
                             boxShadow: grabbed ? '0 6px 18px rgba(0,0,0,.5)' : 'none', borderRadius: 4 }}>
                    <span onPointerDown={onGrip}
                      {...hint ('Move', 'Drag to change this control\'s place on the box — the others make room. Undoable.')}
                      style={{ cursor: grabbed ? 'grabbing' : 'grab', color: grabbed ? WRAP_ACCENT : 'var(--text-dim)', fontSize: 12,
                               lineHeight: 1, padding: '0 2px', userSelect: 'none', touchAction: 'none' }}>⋮⋮</span>
                    <div style={{ flex: 1, minWidth: 0 }}>{e.el}</div>
                  </div>
                );
              })}
          </div>
        )}

        {/* Value summary (panel closed): live values first, then the
            settings; 2 lines at rest ("…" beyond). Hovering opens the whole
            summary as an overlay at the same place (live, nothing below
            moves) and also sends it to the hint panel. */}
        {! showSettings && summary.parts.length > 0 && (() => {
          const text = summary.parts.map ((q, k) => (
            <span key={k}>{k > 0 && ' · '}<span style={q.colour ? { color: q.colour } : undefined}>{q.text}</span></span>
          ));
          const base: React.CSSProperties = { fontSize: 9, color: 'var(--text-muted)', lineHeight: 1.35,
                                              fontVariantNumeric: 'tabular-nums', wordBreak: 'break-word' };
          return (
            <div style={{ position: 'relative', marginTop: 6, maxWidth: 260 }}
              onMouseEnter={() => setSummaryHover (true)}
              onMouseLeave={() => { setSummaryHover (false); setHint (null); }}>
              <div style={{ ...base, display: '-webkit-box', WebkitBoxOrient: 'vertical', WebkitLineClamp: 2, overflow: 'hidden',
                            visibility: summaryHover ? 'hidden' : 'visible' }}>
                {text}
              </div>
              {summaryHover && (
                <div style={{ ...base, position: 'absolute', top: -4, left: -6, right: -6, zIndex: 10,
                              padding: '3px 6px',
                              // solid: --surface2 is 88 % opaque, the box outline showed through
                              background: 'linear-gradient(var(--surface2), var(--surface2)), var(--bg)', border: '1px solid var(--border-hi)',
                              borderRadius: 4, boxShadow: '0 6px 20px rgba(0,0,0,.5)' }}>
                  {text}
                </div>
              )}
            </div>
          );
        })()}
      </div>

      {/* ⚙ panel: every Pax parameter inside, grouped by node */}
      {showSettings && (
        <div className="nodrag" onDoubleClick={e => e.stopPropagation()}
          style={{ padding: '4px 10px 8px', borderTop: '1px solid var(--border)', minWidth: 240 }}>
          {pax.map (x => (
            <div key={x.nodeId} style={{ paddingTop: 6 }}>
              {titleRow (x.nodeId, x.name, x.accent, true, !! x.disabled)}
              {x.params.map ((p, i) => {
                if (x.shown[i] === false) return null;   // e.g. a Spectrumyser band not in use
                if (pairRole (x, i)?.role === 'hi') return null;   // drawn with its Low (two-head slider)
                const on = pinnedSet.has (pinKey (x.nodeId, i));
                return (
                  <div key={i} style={{ padding: '2px 4px', margin: '4px -4px 0', display: 'flex', alignItems: 'flex-start', gap: 6,
                                        borderRadius: 3, background: on ? 'color-mix(in srgb, var(--generic) 10%, transparent)' : 'transparent' }}>
                    <span onClick={() => togglePin (x.nodeId, i)}
                      {...hint (on ? 'Unpin' : 'Pin on the box', on
                        ? 'Takes this control off the folded box (it stays here). Undoable.'
                        : 'Shows this control on the folded box itself (once ⚙ is closed), ready to use; rename it there with the ✎, unpin it there or here. Undoable.')}
                      style={{ cursor: 'pointer', display: 'inline-flex', paddingTop: 1, flexShrink: 0,
                               color: on ? WRAP_ACCENT : 'var(--text-muted)', opacity: on ? 1 : 0.45 }}>
                      <Pin size={10} fill={on ? 'currentColor' : 'none'} />
                    </span>
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <WrapParamControl x={x} p={p} i={i} live={live.get (pinKey (x.nodeId, i))} actions={actions} />
                    </div>
                  </div>
                );
              })}
            </div>
          ))}
          {/* v0.0.949 — built-in nodes' controls */}
          {builtins.map (b => (
            <div key={b.nodeId} style={{ paddingTop: 6 }}>
              {titleRow (b.nodeId, b.name, b.accent, !! b.canDisable, !! b.disabled)}
              {controlsOf (b).map ((_, i) => {
                const on = pinnedSet.has (pinKey (b.nodeId, i));
                return (
                  <div key={i} style={{ padding: '2px 4px', margin: '4px -4px 0', display: 'flex', alignItems: 'flex-start', gap: 6,
                                        borderRadius: 3, background: on ? 'color-mix(in srgb, var(--generic) 10%, transparent)' : 'transparent' }}>
                    <span onClick={() => togglePin (b.nodeId, i)}
                      {...hint (on ? 'Unpin' : 'Pin on the box', on
                        ? 'Takes this control off the folded box (it stays here). Undoable.'
                        : 'Shows this control on the folded box itself (once ⚙ is closed), ready to use; rename it there with the ✎, unpin it there or here. Undoable.')}
                      style={{ cursor: 'pointer', display: 'inline-flex', paddingTop: 1, flexShrink: 0,
                               color: on ? WRAP_ACCENT : 'var(--text-muted)', opacity: on ? 1 : 0.45 }}>
                      <Pin size={10} fill={on ? 'currentColor' : 'none'} />
                    </span>
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <BuiltinControl b={b} i={i} actions={actions} playing={playingMap.get (b.nodeId) ?? false} />
                    </div>
                  </div>
                );
              })}
              {(b.kind === 'dmxConsole' || b.kind === 'artNetConsole') && (() => {   // v0.0.950 — 1 to 4 faders (consoles only)
                const f = fadersOf (b);
                const small: React.CSSProperties = { fontSize: 9, padding: '1px 6px', borderRadius: 3, cursor: 'pointer',
                                                     border: '1px solid var(--border)', color: 'var(--text-muted)' };
                return (
                  <div style={{ display: 'flex', gap: 4, justifyContent: 'flex-end', marginTop: 4 }}>
                    {f.length > 1 && (
                      <span style={small} onClick={() => actions.consoleFaderChannel (b.nodeId, f.slice (0, -1), true)}
                        {...hint ('Remove a fader', 'Removes the last fader (its pin goes with it). Undoable.')}>− fader</span>
                    )}
                    {f.length < maxWrapFaders && (
                      <span style={small} onClick={() => actions.consoleFaderChannel (b.nodeId, [...f, Math.min (512, f[f.length - 1] + 1)], true)}
                        {...hint ('Add a fader', `Up to ${maxWrapFaders} faders (Preferences → Wraps), each on its own channel (it starts on the next one). Undoable.`)}>+ fader</span>
                    )}
                  </div>
                );
              }) ()}
            </div>
          ))}
        </div>
      )}
      </>}
    </div>
  );
}

// v0.0.941 — the same fold indicator as on the nodes' headers
export function FoldTriangle ({ collapsed, colour }: { collapsed: boolean; colour: string }) {
  return (
    <span style={{ fontSize: 12, color: colour, opacity: 0.7, display: 'inline-block',
                   transform: collapsed ? 'rotate(0deg)' : 'rotate(90deg)', transition: 'transform 0.2s' }}>
      <svg width="8" height="10" viewBox="0 0 8 10" style={{ display: 'block' }}>
        <polygon points="0,0 8,5 0,10" fill="currentColor" />
      </svg>
    </span>
  );
}

// ── Open wrap: frame drawn behind its members ────────────────────────────────
export interface WrapFrameData {
  wrap:    RawWrap;
  actions: WrapActions;
  ports:   WrapPort[];   // v0.0.934 — border ports, ordered top to bottom
  drop?:   'join' | 'leave';   // v0.0.935 — ⌥-drag in progress: what a drop would do
  power?:  { anyOn: boolean; count: number };   // v0.0.951 — the wrap's ⏻
  [key: string]: unknown;
}

export const FRAME_PAD  = 18;
export const FRAME_HEAD = 26;
// v0.0.934 — border ports on the open frame: first one FRAME_PORT_TOP below
// the title bar, then every FRAME_PORT_GAP px (App makes the frame tall enough)
export const FRAME_PORT_TOP = 18;
export const FRAME_PORT_GAP = 26;

// One border port: two handles at the same spot — the outer one keeps the
// inner port's own id (outside wires, connections, activity), the inner one
// "<id>~in" carries the wire's inside half to the inner node.
function FramePort ({ q, top }: { q: WrapPort; top: number }) {
  const { setHint } = useContext (HintContext);
  const isIn = q.dir === 'in';
  const dot: React.CSSProperties = { background: q.colour, width: 10, height: 10, border: '2px solid var(--bg)', top, pointerEvents: 'all' };
  return (
    <>
      {/* outer: inputs take wires from the left, outputs give them to the right */}
      <Handle type={isIn ? 'target' : 'source'} position={isIn ? Position.Left : Position.Right}
        id={q.handleId} style={dot}
        onMouseEnter={() => setHint ({ title: `${q.label} — ${q.nodeName}`, body: `Wrap ${isIn ? 'input' : 'output'}: wires from outside connect here; inside, it leads to ${q.nodeName}'s ${q.label}.` })}
        onMouseLeave={() => setHint (null)} />
      {/* inner: the inside half starts / ends here (not connectable) */}
      <Handle type={isIn ? 'source' : 'target'} position={isIn ? Position.Right : Position.Left}
        id={`${q.handleId}~in`} isConnectable={false}
        // its class places it on the opposite side: pull it back onto the
        // border (centre on the edge, like the outer one)
        style={{ ...dot, opacity: 0, pointerEvents: 'none',
                 ...(isIn ? { left: -14, right: 'auto' } : { right: -14, left: 'auto' }) } as React.CSSProperties} />
      {/* small label just inside the frame */}
      <div style={{ position: 'absolute', top: top - 7, [isIn ? 'left' : 'right']: 10, pointerEvents: 'none',
                    fontSize: 8, lineHeight: '14px', whiteSpace: 'nowrap', color: 'var(--text-muted)',
                    fontFamily: "'JetBrains Mono', monospace", textAlign: isIn ? 'left' : 'right' }}>
        <span style={{ color: 'var(--text-dim)' }}>{q.label}</span> · {q.nodeName}
      </div>
    </>
  );
}

export function WrapFrame ({ id, data }: NodeProps) {
  const { wrap, actions, ports = [], drop, power } = data as WrapFrameData;
  // ⌥-drag highlight: join = solid, brighter; leave = red dashed
  const dropCol = drop === 'join' ? 'var(--accent)' : drop === 'leave' ? '#f87171' : null;
  const { setHint } = useContext (HintContext);
  const updateNodeInternals = useUpdateNodeInternals();
  const ins  = ports.filter (q => q.dir === 'in');
  const outs = ports.filter (q => q.dir === 'out');
  const portsKey = ports.map (q => q.handleId).join ('|');
  useEffect (() => { updateNodeInternals (id); }, [id, portsKey, updateNodeInternals]);
  const topOf = (k: number) => FRAME_HEAD + FRAME_PORT_TOP + k * FRAME_PORT_GAP;
  return (
    <div style={{
      position: 'relative',
      width: '100%', height: '100%', pointerEvents: 'none', boxSizing: 'border-box',
      border: dropCol ? `2px ${drop === 'join' ? 'solid' : 'dashed'} ${dropCol}`
                      : `1.5px dashed color-mix(in srgb, var(--generic) 70%, transparent)`, borderRadius: 10,
      background: dropCol ? `color-mix(in srgb, ${dropCol} 8%, transparent)` : 'color-mix(in srgb, var(--generic) 4%, transparent)',
      boxShadow: dropCol ? `0 0 18px color-mix(in srgb, ${dropCol} 45%, transparent)` : undefined,
    }}>
      <div className="wrap-frame-drag" style={{
        pointerEvents: 'auto', cursor: 'grab', display: 'flex', alignItems: 'center', gap: 6, height: FRAME_HEAD,
        padding: '0 8px', borderBottom: `1px dashed color-mix(in srgb, var(--generic) 45%, transparent)`,
        background: 'color-mix(in srgb, var(--generic) 10%, var(--bg))', borderRadius: '9px 9px 0 0',
        fontFamily: "'JetBrains Mono', monospace", userSelect: 'none',
      }}
        onDoubleClick={e => { e.stopPropagation(); actions.foldHeader (wrap.id); }}>
        <FoldTriangle collapsed={false} colour={WRAP_ACCENT} />
        <Package size={12} color={WRAP_ACCENT} />
        <EditableTitle display={wrap.name || 'WRAP'} value={wrap.name === 'WRAP' ? '' : wrap.name} placeholder="Wrap"
          onCommit={name => actions.rename (wrap.id, name || 'WRAP')} textStyle={titleStyle} />
        <span style={{ fontSize: 9, color: dropCol ?? 'var(--text-muted)' }}>
          {drop === 'join' ? 'drop to add' : drop === 'leave' ? 'drop to take out' : 'open'}
        </span>
        <div style={{ flex: 1 }} />
        <NodeHeaderButton onClick={() => actions.saveToLibrary (wrap.id)}
          onHint={{ onMouseEnter: () => setHint ({ title: 'Save to library', body: 'Saves this wrap to your wrap library under its name (sidebar → Wraps).' }), onMouseLeave: () => setHint (null) }}>
          <BookmarkPlus size={13} />
        </NodeHeaderButton>
        <NodeHeaderButton onClick={() => actions.enter (wrap.id)}
          onHint={{ onMouseEnter: () => setHint ({ title: 'Enter', body: 'Shows only this wrap\'s nodes (⌘↑). Back with "Main" at the top, ⌘↓ or Esc.' }), onMouseLeave: () => setHint (null) }}>
          <LogIn size={13} />
        </NodeHeaderButton>
        <NodeHeaderButton onClick={() => actions.close (wrap.id)}
          onHint={{ onMouseEnter: () => setHint ({ title: 'Fold the wrap', body: 'Folds these nodes back into one box.' }), onMouseLeave: () => setHint (null) }}>
          <Minimize2 size={13} />
        </NodeHeaderButton>
        {power && power.count > 0 && (   // v0.0.951 — the wrap's ⏻
          <NodeHeaderButton onClick={() => actions.wrapPower (wrap.id)} active={power.anyOn} activeAccent={WRAP_ACCENT}
            onHint={{ onMouseEnter: () => setHint ({ title: power.anyOn ? 'Switch the wrap off' : 'Switch the wrap on',
              body: `${power.anyOn ? 'Switches off' : 'Switches back on'} every node inside that has a ⏻ (${power.count}). One undo step.` }), onMouseLeave: () => setHint (null) }}>
            <Power size={11} />
          </NodeHeaderButton>
        )}
        <NodeHeaderButton onClick={() => actions.unwrap (wrap.id)}
          onHint={{ onMouseEnter: () => setHint ({ title: 'Unwrap', body: 'Removes the wrap; its nodes stay where they are. Undoable.' }), onMouseLeave: () => setHint (null) }}>
          <Ungroup size={13} />
        </NodeHeaderButton>
        {/* v0.0.940 — same ✕ as the folded box (user, 2026-10-17) */}
        <NodeHeaderButton onClick={() => actions.remove (wrap.id)} danger
          onHint={{ onMouseEnter: () => setHint ({ title: 'Delete wrap', body: 'Deletes the wrap AND the nodes inside it. Undoable (⌘Z).' }), onMouseLeave: () => setHint (null) }}>
          <X size={13} />
        </NodeHeaderButton>
      </div>
      {ins.map  ((q, k) => <FramePort key={q.handleId} q={q} top={topOf (k)} />)}
      {outs.map ((q, k) => <FramePort key={q.handleId} q={q} top={topOf (k)} />)}
    </div>
  );
}

// ── A wire crossing open-wrap frames (v0.0.934) ──────────────────────────────
// Still the one real connection (real id + class → resting colour, activity
// flash, selection, delete all as for any wire), drawn through the frame
// port(s) it crosses: a bezier into each port from the left, out of it to the
// right, then on to its end.
export function WrapRouteEdge ({ id, sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition,
                                 style, markerEnd, interactionWidth, data }: EdgeProps) {
  const vias = ((data as any)?.vias ?? []) as { x: number; y: number }[];
  const pts = [{ x: sourceX, y: sourceY, pos: sourcePosition },
               ...vias.map (v => ({ x: v.x, y: v.y, pos: Position.Right })),
               { x: targetX, y: targetY, pos: targetPosition }];
  let d = '';
  for (let k = 0; k + 1 < pts.length; k++) {
    const a = pts[k], b = pts[k + 1];
    const [seg] = getBezierPath ({ sourceX: a.x, sourceY: a.y, sourcePosition: a.pos,
                                   targetX: b.x, targetY: b.y,
                                   targetPosition: k + 1 < pts.length - 1 ? Position.Left : b.pos });
    d += (d ? ' ' : '') + seg;
  }
  return <BaseEdge id={id} path={d} style={style} markerEnd={markerEnd} interactionWidth={interactionWidth} />;
}

// ── Enter view: the outside connections as a rail (v0.0.937) ─────────────────
// Left rail = what comes in from outside, right rail = what goes out. Each
// port's handle reuses the inner port's id (wires keep their ids → colour and
// flashes as usual). Display only: nothing outside to wire from in here.
export const RAIL_GAP = 30;
export function WrapRail ({ id, data }: NodeProps) {
  const { side, ports } = data as { side: 'in' | 'out'; ports: WrapPort[] };
  const { setHint } = useContext (HintContext);
  const updateNodeInternals = useUpdateNodeInternals();
  const key = ports.map (q => q.handleId).join ('|');
  useEffect (() => { updateNodeInternals (id); }, [id, key, updateNodeInternals]);
  const isIn = side === 'in';
  if (ports.length === 0) return <div style={{ width: 1, height: 1 }} />;
  return (
    <div style={{ position: 'relative', width: 170, height: 24 + ports.length * RAIL_GAP,
                  borderRadius: 8, border: '1px dashed color-mix(in srgb, var(--generic) 55%, transparent)',
                  background: 'color-mix(in srgb, var(--generic) 6%, var(--bg))',
                  fontFamily: "'JetBrains Mono', monospace", userSelect: 'none' }}>
      <div style={{ fontSize: 8, letterSpacing: '0.1em', color: 'var(--text-muted)', padding: '5px 8px',
                    textAlign: isIn ? 'left' : 'right' }}>{isIn ? 'FROM OUTSIDE' : 'TO OUTSIDE'}</div>
      {ports.map ((q, k) => {
        const top = 24 + k * RAIL_GAP + RAIL_GAP / 2 - 4;
        return (
          <div key={q.handleId}>
            <Handle type={isIn ? 'source' : 'target'} position={isIn ? Position.Right : Position.Left}
              id={q.handleId} isConnectable={false}
              style={{ background: q.colour, width: 10, height: 10, border: '2px solid var(--bg)', top }}
              onMouseEnter={() => setHint ({ title: `${q.label} — ${q.nodeName}`, body: `Wrap ${isIn ? 'input' : 'output'}: ${isIn ? 'comes from outside into' : 'goes from'} ${q.nodeName}'s ${q.label}${isIn ? '' : ' to outside'}.` })}
              onMouseLeave={() => setHint (null)} />
            <div style={{ position: 'absolute', top: top - 7, [isIn ? 'right' : 'left']: 12, fontSize: 9, lineHeight: '14px',
                          whiteSpace: 'nowrap', color: 'var(--text-muted)', textAlign: isIn ? 'right' : 'left', pointerEvents: 'none' }}>
              <span style={{ color: 'var(--text-dim)' }}>{q.label}</span> · {q.nodeName}
            </div>
          </div>
        );
      })}
    </div>
  );
}

