// ValueHubNode.tsx — v0.0.992: one node UI for the "Value hub" converters
// (X → Value, Value → X — the converter matrix, 2026-11-19). Each Pax gets a
// spec below; the component draws:
//   • the header's message selector (click: next, ⇧-click: previous) — the
//     same idea as Audio to DMX's RMS / FREQ toggle (user's pointer)
//   • a one-line summary in the body (the ports are centred on it)
//   • the ⚙ panel: the parameters that apply to the chosen message, with the
//     read-only "last" value live from the port activity (paxReadOnly)
// Settings are a flat array, one value per Pax parameter, like Envelope /
// Channel Filter, so undo / redo / wraps work the generic way.

import { useState, useEffect, useCallback, useContext, useRef } from 'react';
import ValueBox from './ValueBox';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, useNodeDisabled, NodeHeaderButton, NodeCollapseArrow, settingsPanelStyle,
         EditableTitle, commitModelName, SettingsPanelHeader, SliderRow, _paxInfoMap, detectPaxTheme, readoutBoxStyle } from './NodeUtils';
import { HintContext } from './HintPanel';
import { LearnButton } from './LearnButton';
import { useMidiLearn, LearnedEvent } from './Learn';
import { Settings, X, Power } from 'lucide-react';

const _settingsOpen = new Map<string, boolean>();

export interface HubParam {
  idx:    number;
  label:  string;
  min:    number;
  max:    number;
  step:   number;               // 0 = continuous
  toggle?: boolean;             // drawn as an on / off button
  channels?: number;            // v0.0.993 — a channel mask: 16 buttons + "Any"; the value = the older single-channel parameter's index
  numbox?: boolean;             // drawn as a number box (+ Learn when the spec has one)
  fmt?:   (v: number) => string;
  shown?: (msg: number) => boolean;
}

export interface HubSpec {
  title:     string;
  accent:    string;            // the protocol side's colour (header, controls)
  msgIdx:    number;            // parameter holding the message type
  msgNames:  string[];          // header selector labels
  msgHints:  string[];          // one line each, for the hint panel
  params:    HubParam[];
  defaults:  number[];          // one per Pax parameter (read-only included)
  roIdx:     number;            // the read-only "last" parameter
  roLabel:   string;
  summary:   (v: number[]) => string;
  roFmt?:    (v: number, msg: number) => string;
  ranges?:   { lo: number; hi: number; inv: number; label: string }[];   // min / max pairs: one rail, two heads, + invert
  // MIDI learn (like Channel Filter): the next event at the MIDI input sets these parameters
  learn?:    (ev: LearnedEvent, v: number[]) => [number, number][];
}

// v0.0.992 — a min / max pair as one rail with two heads, linear, behaving
// like Audio to DMX's band (user): the heads can't cross (inverting is the
// row's INV button), ⌘ / Ctrl held while dragging a head moves both together
// (the gap kept, stopped at the rail's ends); the two boxes at the rail's ends
// take any value (one typed outside the rail widens it).
export function RangeDual ({ label, lo, hi, inverted = false, accent, onChange, onSet, onInvert, onCommit }: {
  label: string; lo: number; hi: number; inverted?: boolean; accent: string;
  onChange: (which: 'lo' | 'hi', v: number) => void;
  onSet:    (lo: number, hi: number) => void;
  onInvert?: () => void;   // no INV button without it (Value Console)
  onCommit: () => void;
}) {
  // the rail is frozen during a drag (refitted on release): shrinking under the
  // pointer made a head jump (user, wrap panel: 0–5, high head → 0 gave 1.0)
  const held = useRef<[number, number] | null> (null);
  const [wMin, wMax] = held.current ?? [Math.floor (Math.min (0, lo, hi)), Math.ceil (Math.max (1, lo, hi))];
  const hold = () => {
    held.current = [wMin, wMax];
    const up = () => { held.current = null; window.removeEventListener ('pointerup', up); };
    window.addEventListener ('pointerup', up);
  };
  const POS = 1000;
  const r2 = (v: number) => Math.round (v * 100) / 100;
  const toPos   = (v: number) => (v - wMin) / (wMax - wMin) * POS;
  const fromPos = (p: number) => r2 (wMin + p / POS * (wMax - wMin));
  const cmd = useRef (false);
  useEffect (() => {
    const k = (e: KeyboardEvent) => { if (e.key === 'Meta' || e.key === 'Control') cmd.current = e.type === 'keydown'; };
    window.addEventListener ('keydown', k); window.addEventListener ('keyup', k);
    return () => { window.removeEventListener ('keydown', k); window.removeEventListener ('keyup', k); };
  }, []);
  const head = (which: 'lo' | 'hi', v: number) => {
    if (cmd.current) {                                   // move both, keeping the gap
      const gap = hi - lo;
      let nlo = which === 'lo' ? v : v - gap;
      nlo = Math.max (wMin, Math.min (wMax - gap, nlo));
      onSet (r2 (nlo), r2 (nlo + gap));
    }
    // no crossing: a head pushes the other along (user), up to the rail's end
    else if (which === 'lo') { if (v > hi) onSet (v, v); else onChange ('lo', v); }
    else                     { if (v < lo) onSet (v, v); else onChange ('hi', v); }
  };
  const typed = (which: 'lo' | 'hi', v: number) =>   // a typed value pushes the other one too
    which === 'lo' ? (v > hi ? onSet (v, v) : onChange ('lo', v)) : (v < lo ? onSet (v, v) : onChange ('hi', v));
  const box = (which: 'lo' | 'hi') => (
    <ValueBox value={which === 'lo' ? lo : hi} min={-1000} max={1000} step={0.01} accent={accent}
      format={v => (Math.round (v * 1000) / 1000).toString ()}
      onLive={v => typed (which, v)} onCommit={v => { typed (which, v); onCommit (); }}
      width={34} style={{ ...readoutBoxStyle (accent), textAlign: 'center' }} />
  );
  return (
    <div className="nodrag" onMouseDown={e => e.stopPropagation ()} style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
      <span style={{ color: 'var(--text-muted)', minWidth: 60, fontSize: 8 }}>{label}</span>
      {box ('lo')}
      <div className="dual-range" style={{ position: 'relative', flex: 1, minWidth: 0, height: 16 }}>
        {(['lo', 'hi'] as const).map (w => (
          <input key={w} type="range" min={0} max={POS} step={1} value={toPos (w === 'lo' ? lo : hi)}
            onMouseDown={e => { cmd.current = e.metaKey || e.ctrlKey; hold (); }}
            onMouseUp={onCommit} onKeyUp={onCommit}
            onChange={e => head (w, fromPos (Number (e.target.value)))}
            onDoubleClick={e => { e.stopPropagation (); onSet (0, 1); onCommit (); }}
            style={{ width: '100%', top: 5, left: 0, ['--thumb-color' as any]: accent }} />
        ))}
      </div>
      {box ('hi')}
      {onInvert && <div onClick={onInvert} title="Invert: high in → low out"
        style={{ fontSize: 8, fontWeight: 700, padding: '1px 4px', borderRadius: 2, cursor: 'pointer',
                 background: inverted ? accent : 'var(--bg)', color: inverted ? '#000' : 'var(--text-muted)',
                 border: `1px solid ${inverted ? accent : 'var(--border)'}` }}>INV</div>}
    </div>
  );
}

// v0.0.993 — channels as a 16-bit mask (user: several channels, + "Any"):
// bit 0 = channel 1, 65535 = Any (all 16). The mask parameter at 0 = not set:
// the older single Channel parameter applies (graphs saved before 993).
export const CH_ALL = 0xFFFF;
export function channelMaskOf (v: number[], maskIdx: number, legacyIdx: number): number {
  const m = Math.round (v[maskIdx] ?? 0) & CH_ALL;
  if (m) return m;
  const ch = Math.round (v[legacyIdx] ?? 0);
  return ch >= 1 && ch <= 16 ? 1 << (ch - 1) : CH_ALL;
}
export function channelText (mask: number, anyWord = 'any'): string {
  if ((mask & CH_ALL) === CH_ALL) return anyWord;
  const on = Array.from ({ length: 16 }, (_, k) => k + 1).filter (c => (mask >> (c - 1)) & 1);
  return on.join (', ');
}
// 16 buttons, two rows, "Any" spanning both. As in MIDI Out Device: each
// click toggles a channel (from Any, a click keeps only that one; the last
// one off = Any again); "Any" clears the rest.
export function ChannelButtons ({ mask, accent, onPick, label = 'Channel' }: { mask: number; accent: string; onPick: (m: number) => void; label?: string }) {
  const all = (mask & CH_ALL) === CH_ALL;
  const cell = (key: number, text: string, on: boolean, click: () => void, extra: React.CSSProperties = {}) => (
    <div key={key} onClick={click}
      style={{ fontSize: 8, textAlign: 'center', padding: '2px 0', borderRadius: 2, cursor: 'pointer', display: 'flex',
               alignItems: 'center', justifyContent: 'center',
               background: on ? accent : 'var(--bg)', color: on ? '#000' : 'var(--text-muted)', border: `1px solid ${on ? accent : 'var(--border)'}`, ...extra }}>
      {text}
    </div>
  );
  const toggle = (c: number) => {
    const bit = 1 << (c - 1);
    const next = all ? bit : (mask ^ bit) & CH_ALL;
    onPick (next === 0 ? CH_ALL : next);
  };
  return (
    <div className="nodrag" onMouseDown={e => e.stopPropagation ()} style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
      <span style={{ color: 'var(--text-muted)', minWidth: 60, fontSize: 8 }}>{label}</span>
      <div style={{ flex: 1, display: 'grid', gridTemplateColumns: '1.4fr repeat(8, 1fr)', gap: 2 }}>
        {cell (0, 'Any', all, () => onPick (CH_ALL), { gridRow: '1 / span 2', gridColumn: 1 })}
        {Array.from ({ length: 16 }, (_, k) => cell (k + 1, String (k + 1), ! all && !! ((mask >> k) & 1), () => toggle (k + 1),
          { gridRow: k < 8 ? 1 : 2, gridColumn: (k % 8) + 2 }))}
      </div>
    </div>
  );
}

const any = (v: number, word = 'any') => v < 0 ? word : String (Math.round (v));
const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = (n: number) => `${NOTE[((Math.round (n) % 12) + 12) % 12]}${Math.floor (Math.round (n) / 12) - 1}`;
const num = (v: number) => (Math.round (v * 1000) / 1000).toString ();

export const HUB_SPECS: Record<string, HubSpec> = {
  'MIDI to Value': {
    title: 'MIDI to Value', accent: 'var(--midi)', msgIdx: 0,
    msgNames: ['CC', 'VEL', 'NOTE', 'BEND', 'AT', 'PROG'],
    msgHints: ['Control change: its value', 'Note velocity (note off = 0): a gate or a level',
               'Note number of each note played', 'Pitch bend (centre = middle of the range)',
               'Channel aftertouch', 'Program change number'],
    params: [
      { idx: 1, label: 'Number',  min: -1, max: 127, step: 1, numbox: true, fmt: v => v < 0 ? 'Any' : String (Math.round (v)), shown: m => m === 0 || m === 1 },
      { idx: 7, label: 'Channel', min: 0,  max: 65535, step: 1, channels: 2 },
    ],
    ranges: [{ lo: 3, hi: 4, inv: 6, label: 'Output' }],
    defaults: [0, -1, 0, 0, 1, 0, 0, 0], roIdx: 5, roLabel: 'Last',
    learn: (ev, v) => {
      const m = Math.round (v[0]);
      const msg = ev.type === 4 ? 0 : ev.type === 2 ? (m === 2 ? 2 : 1) : ev.type === 7 ? 3 : ev.type === 6 ? 4 : ev.type === 5 ? 5 : m;
      return [[0, msg], [7, 1 << (ev.ch - 1)], ...((msg === 0 || msg === 1) ? [[1, ev.d1] as [number, number]] : [])];
    },
    summary: v => {
      const m = Math.round (v[0]), mk = channelMaskOf (v, 7, 2), ch = mk === CH_ALL ? 'any ch' : `ch ${channelText (mk)}`;
      const what = m === 0 ? `CC ${any (v[1])}` : m === 1 ? `vel ${v[1] < 0 ? 'any note' : noteName (v[1])}`
                 : m === 2 ? 'note number' : m === 3 ? 'pitch bend' : m === 4 ? 'aftertouch' : 'program';
      return `${what} · ${ch} → ${num (v[3])}…${num (v[4])}`;
    },
  },
  'Value to MIDI': {
    title: 'Value to MIDI', accent: 'var(--midi)', msgIdx: 0,
    msgNames: ['CC', 'GATE', 'NOTE', 'BEND', 'PROG'],
    msgHints: ['Control change: the value sets the CC value', 'Note gate: crossing Threshold plays the note, back under releases it',
               'Note pitch: the value picks the note (a new one releases the last)', 'Pitch bend (0.5 = centre)', 'Program change'],
    params: [
      { idx: 1, label: 'Number',    min: 0, max: 127, step: 1, numbox: true, fmt: v => String (Math.round (v)), shown: m => m === 0 || m === 1 },
      { idx: 11, label: 'Channel',  min: 0, max: 65535, step: 1, channels: 2 },
      { idx: 5, label: 'Threshold', min: 0, max: 1,   step: 0, fmt: v => v.toFixed (2), shown: m => m === 1 },
      { idx: 6, label: 'Velocity',  min: 1, max: 127, step: 1, fmt: v => String (Math.round (v)), shown: m => m === 1 || m === 2 },
      { idx: 7, label: 'On change only', min: 0, max: 1, step: 1, toggle: true, shown: m => m !== 1 },
      { idx: 8, label: 'Max rate /s', min: 0, max: 100, step: 1, fmt: v => v < 1 ? 'no limit' : String (Math.round (v)) },
    ],
    defaults: [0, 1, 1, 0, 1, 0.5, 100, 1, 50, 0, 0, 0], roIdx: 9, roLabel: 'Last sent',
    ranges: [{ lo: 3, hi: 4, inv: 10, label: 'Input' }],
    learn: (ev, v) => {   // wire a controller into its MIDI In: it learns what to send
      const m = Math.round (v[0]);
      const msg = ev.type === 4 ? 0 : ev.type === 2 ? (m === 2 ? 2 : 1) : ev.type === 7 ? 3 : ev.type === 5 ? 4 : m;
      return [[0, msg], [11, 1 << (ev.ch - 1)], ...((msg === 0 || msg === 1) ? [[1, ev.d1] as [number, number]] : [])];
    },
    summary: v => {
      const m = Math.round (v[0]), mk = channelMaskOf (v, 11, 2), ch = mk === CH_ALL ? 'all ch' : `ch ${channelText (mk)}`;
      const what = m === 0 ? `CC ${Math.round (v[1])}` : m === 1 ? `gate ${noteName (v[1])} @ ${v[5].toFixed (2)}`
                 : m === 2 ? 'note pitch' : m === 3 ? 'pitch bend' : 'program';
      return `${num (v[3])}…${num (v[4])} → ${what} · ${ch}`;
    },
    roFmt: (v, m) => m === 2 ? `${noteName (v)} (${Math.round (v)})` : String (Math.round (v)),
  },
};

export default function ValueHubNode ({ id, data, selected }: NodeProps) {
  const d = data as any;
  const spec = HUB_SPECS[d.paxName as string];
  const { setHint } = useContext (HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, d._forceCollapsed);
  const { disabled, toggleDisabled }   = useNodeDisabled (id, d.disabled);
  const [showSettings, setShowSettings] = useState (() => _settingsOpen.get (id) ?? false);
  const [vals, setVals] = useState<number[]> (spec?.defaults ?? []);
  const [ro, setRo]     = useState<number | null> (null);
  const valsRef = useRef<number[]> (vals);
  valsRef.current = vals;
  // v0.0.992 — Learn (as in Channel Filter): the next event at the MIDI input
  const learn = useMidiLearn<'num'> (id, (_t, ev) => {
    if (! spec?.learn) return;
    const next = [...valsRef.current];
    for (const [i, v] of spec.learn (ev, valsRef.current)) { next[i] = v; Bridge.setPaxParameter (id, i, v); }
    setVals (next);
    Bridge.setNodeSettings (id, next);
    Bridge.commitNodeSettings (id);
  });

  // restore (mount, undo / redo): pad missing entries with their default
  useEffect (() => {
    if (! spec) return;
    let raw: number[] | null = null;
    try { raw = d.settingsJson ? JSON.parse (d.settingsJson) as number[] : null; } catch { raw = null; }
    const v = spec.defaults.map ((def, i) => (raw && typeof raw[i] === 'number') ? raw[i] : def);
    setVals (v);
    v.forEach ((x, i) => { if (i !== spec.roIdx) Bridge.setPaxParameter (id, i, x); });
  }, [d.settingsJson]);   // eslint-disable-line react-hooks/exhaustive-deps

  // the read-only "last" value, live
  useEffect (() => Bridge.onPortActivity ((entries: any[]) => {
    const e = entries.find (x => x.id === id);
    const r = e?.paxReadOnly?.find ((p: any) => p.index === spec?.roIdx);
    if (r) setRo (prev => prev === r.value ? prev : r.value);
  }), [id, spec?.roIdx]);

  const setParam = useCallback ((idx: number, v: number, commit: boolean) => {
    setVals (prev => {
      const next = [...prev]; next[idx] = v;
      Bridge.setNodeSettings (id, next);
      return next;
    });
    Bridge.setPaxParameter (id, idx, v);
    if (commit) Bridge.commitNodeSettings (id);
  }, [id]);

  if (! spec) return null;
  const msg    = Math.round (vals[spec.msgIdx] ?? 0);
  const n      = spec.msgNames.length;
  // v0.0.992 — the converter colour, as in the sidebar (user), not the protocol's
  const accent = detectPaxTheme (d.ports ?? [], d.paxName ? _paxInfoMap.get (d.paxName)?.colourCategory : undefined).accent;
  const customName: string = d.customName ?? '';
  const ports  = (d.ports ?? []) as { id: string; label: string; direction?: string }[];
  const isIn   = (p: { id: string }) => p.id.endsWith ('_in');
  const colourOf = (p: { label: string }) => /midi/i.test (p.label) ? 'var(--midi)' : /osc/i.test (p.label) ? 'var(--osc)'
                 : /mqtt/i.test (p.label) ? 'var(--mqtt)' : /udp/i.test (p.label) ? 'var(--udp)' : /dmx/i.test (p.label) ? 'var(--dmx)' : 'var(--value)';
  const ins = ports.filter (isIn), outs = ports.filter (p => ! isIn (p));

  const resetAll = () => {
    setVals (spec.defaults);
    Bridge.setNodeSettings (id, spec.defaults);
    spec.defaults.forEach ((x, i) => { if (i !== spec.roIdx) Bridge.setPaxParameter (id, i, x); });
    Bridge.commitNodeSettings (id);
  };

  return (
    <div style={{
      display: 'inline-block', minWidth: 220, background: 'var(--surface)',
      border: `1px solid ${selected ? accent : 'var(--border)'}`, borderTop: `3px solid ${accent}`,
      borderRadius: 'var(--radius)', fontFamily: "'JetBrains Mono', monospace",
      filter: disabled ? 'grayscale(0.8) opacity(0.55)' : 'none', transition: 'filter .15s', position: 'relative',
    }}>
      {ins.map ((p, i)  => <NodeHandle key={p.id} nodeId={id} portId={p.id} label={p.label} direction="in"  colour={colourOf (p)} index={i} total={ins.length}  anchor="label" />)}
      {outs.map ((p, i) => <NodeHandle key={p.id} nodeId={id} portId={p.id} label={p.label} direction="out" colour={colourOf (p)} index={i} total={outs.length} anchor="label" />)}

      <div style={{ display: 'flex', alignItems: 'center', padding: '4px 8px', gap: 4, cursor: 'pointer',
                    borderBottom: collapsed ? 'none' : `1px solid color-mix(in srgb, ${accent} 27%, transparent)` }}
           onDoubleClick={toggleCollapsed}>
        <NodeCollapseArrow collapsed={collapsed} accent={accent} />
        <div style={{ flex: 1, minWidth: 0 }}>
          <EditableTitle display={customName || spec.title} value={customName} placeholder={spec.title}
            onCommit={commitModelName (id)}
            textStyle={{ fontSize: '11px', fontWeight: 700, color: accent, letterSpacing: '0.1em', fontFamily: "'Syne', sans-serif",
                         whiteSpace: 'nowrap', textTransform: 'uppercase', userSelect: 'none' }} />
        </div>
        {/* message selector — click: next, ⇧-click: previous */}
        <div className="nodrag" onDoubleClick={e => e.stopPropagation ()}
          onClick={e => { e.stopPropagation (); setParam (spec.msgIdx, (msg + (e.shiftKey ? n - 1 : 1)) % n, true); }}
          onMouseEnter={() => setHint ({ title: `Message: ${spec.msgNames[msg]}`, body: `${spec.msgHints[msg]}. Click: next type, ⇧-click: previous (${spec.msgNames.join (' · ')}).` })}
          onMouseLeave={() => setHint (null)}
          style={{ fontSize: 9, fontWeight: 700, padding: '1px 6px', borderRadius: 3, cursor: 'pointer', minWidth: 30, textAlign: 'center',
                   background: accent, color: '#000', userSelect: 'none' }}>
          {spec.msgNames[msg]}
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={toggleDisabled} active={! disabled} activeAccent={accent}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Disable / Enable Node', body: 'A disabled converter sends nothing.' }), onMouseLeave: () => setHint (null) }}>
            <Power size={11} />
          </NodeHeaderButton>
        </div>
        <div onDoubleClick={e => e.stopPropagation ()}>
          <NodeHeaderButton onClick={() => setShowSettings (v => { const next = ! v; _settingsOpen.set (id, next); return next; })}
            active={showSettings} activeAccent={accent}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Settings', body: `${spec.title}: number, channel, ranges${spec.title.startsWith ('Value') ? ', rate' : ''}.` }), onMouseLeave: () => setHint (null) }}>
            <Settings size={11} />
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
        <div style={{ padding: '8px 12px', display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 3 }}>
          <div data-port-anchor="label" style={{ fontSize: 9, color: 'var(--text-dim)', whiteSpace: 'nowrap' }}>{spec.summary (vals)}</div>
          <div style={{ fontSize: 8, color: 'var(--text-muted)' }}>
            {spec.roLabel}: <span style={{ color: accent }}>{ro === null ? '—' : (spec.roFmt ? spec.roFmt (ro, msg) : String (Math.round (ro)))}</span>
          </div>
        </div>
      )}

      {showSettings && ! collapsed && (
        <div style={settingsPanelStyle}>
          <SettingsPanelHeader title={spec.title.toUpperCase ()} onReset={resetAll}
            onClose={() => { _settingsOpen.set (id, false); setShowSettings (false); }} />
          {/* v0.0.992 — one row per setting, separated by a line (user); the ranges last */}
          {[
            ...spec.params.filter (p => ! p.shown || p.shown (msg)).map (p => p.channels !== undefined ? (
              <ChannelButtons key={p.idx} mask={channelMaskOf (vals, p.idx, p.channels)} accent={accent}
                onPick={m => setParam (p.idx, m, true)} />
            ) : p.numbox ? (
              <div key={p.idx} className="nodrag" style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <span style={{ color: 'var(--text-muted)', minWidth: 60, fontSize: 8 }}>{p.label}</span>
                <ValueBox value={vals[p.idx] ?? spec.defaults[p.idx]} min={p.min} max={p.max} step={1} accent={accent}
                  format={p.fmt ?? num} onLive={v => setParam (p.idx, Math.round (v), false)}
                  onCommit={v => { setParam (p.idx, Math.round (v), false); Bridge.commitNodeSettings (id); }}
                  width={40} style={{ ...readoutBoxStyle (accent), textAlign: 'center' }} />
                {spec.learn && <LearnButton armed={learn.target === 'num'} onClick={() => learn.start ('num')} accent={accent}
                                            what="the message type, number and channel" />}
              </div>
            ) : p.toggle ? (
              <div key={p.idx} className="nodrag" style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
                <span style={{ color: 'var(--text-muted)', minWidth: 60, fontSize: 8 }}>{p.label}</span>
                <div onClick={() => setParam (p.idx, (vals[p.idx] ?? 0) >= 0.5 ? 0 : 1, true)}
                  style={{ fontSize: 9, padding: '1px 8px', borderRadius: 3, cursor: 'pointer',
                           background: (vals[p.idx] ?? 0) >= 0.5 ? accent : 'var(--surface)',
                           color: (vals[p.idx] ?? 0) >= 0.5 ? '#000' : 'var(--text-muted)', border: `1px solid ${accent}` }}>
                  {(vals[p.idx] ?? 0) >= 0.5 ? 'ON' : 'OFF'}
                </div>
              </div>
            ) : (
              <SliderRow key={p.idx} label={p.label} value={vals[p.idx] ?? spec.defaults[p.idx]} min={p.min} max={p.max} step={p.step}
                format={p.fmt ?? num} color={accent}
                onChange={v => setParam (p.idx, v, false)}
                onDoubleClick={() => setParam (p.idx, spec.defaults[p.idx], false)}
                onCommit={() => Bridge.commitNodeSettings (id)} />
            )),
            ...(spec.ranges ?? []).map (r => (
              <RangeDual key={`r${r.lo}`} label={r.label} lo={vals[r.lo] ?? spec.defaults[r.lo]} hi={vals[r.hi] ?? spec.defaults[r.hi]}
                inverted={(vals[r.inv] ?? 0) >= 0.5} accent={accent}
                onChange={(w, v) => setParam (w === 'lo' ? r.lo : r.hi, v, false)}
                onSet={(lo, hi) => { setParam (r.lo, lo, false); setParam (r.hi, hi, false); }}
                onInvert={() => setParam (r.inv, (vals[r.inv] ?? 0) >= 0.5 ? 0 : 1, true)}
                onCommit={() => Bridge.commitNodeSettings (id)} />
            )),
          ].map ((row, k, all) => (
            <div key={k} style={{ padding: '5px 0', borderBottom: k < all.length - 1 ? '1px solid var(--border)' : 'none' }}>{row}</div>
          ))}
        </div>
      )}
    </div>
  );
}
