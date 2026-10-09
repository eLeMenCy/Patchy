// Patchy — MIDI MORPHER node UI (v0.0.925 → v0.0.927)
//
// One rule per node (see Source/MidiMorpherNode.h for the engine and the
// exact semantics), two faces on the same settingsJson:
//   Advanced — MidiDash-style IN/OUT table in the body, full rule editor.
//   Basic    — the rule as a sentence in the body ("CC 7 on ch 3 → CC 110"),
//              a friendly When / Send / Value editor (MorpherBasic.ts does
//              the Data1/Data2/Pull work). A rule Basic can't express is
//              shown read-only there, with "Edit in Advanced".
// The mode is per node (settingsJson "mode", default Basic): switch in the
// panel or with the B/A badge in the header. Pure logic: MorpherCore.ts.
//
// State = settingsJson (keys shared with MidiMorpherNode::parseRule):
//   inCh / outCh       0 = Any (IN) / Copy (OUT), 1-16
//   inMsg / outMsg     0 = Any / Copy, 1 Note Off, 2 Note On, 3 Poly AT,
//                      4 CC, 5 Program, 6 Channel AT, 7 Pitch
//   inD1 inD2 outD1 outD2   [lo, hi] — hi -1 = "Max" (per message type)
//   outD1Pull / outD2Pull   OUT Data1 takes IN Data2 / OUT Data2 takes IN Data1
//   unmatched          'pass' | 'block'
// Discrete edits → commitSettingsChange (one undo step); value scrubs →
// setNodeSettings while dragging + commitNodeSettings on release. The
// backend applies every change to the running node live.

import { useCallback, useContext, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Funnel, FunnelX } from 'lucide-react';
import { Bridge } from './Bridge';
import type { RawPort } from './Bridge';
import { HintContext } from './HintPanel';
import { NodeSelect } from './NodeSelect';
import {
  NodeHandle, NodeHeader, NodeHeaderButton, Checkbox, SettingsPanelHeader,
  useNodeSettings, useNodeDelete, useNodeDisabled, useNodeCollapsed,
  commitModelName, nodeContainerStyle,
} from './NodeUtils';
import {
  Range, MorpherRule, DEFAULT_RULE, MSG_NAMES, hasData2, data1Max, parseRule, normalise,
  fmtLo, fmtHi, clampTo, rLo, rHi, scaleValue, SlotMap, slotMap, ruleSummary, ruleNotes,
  MorphSide, decodeMorph, eventText,
} from './MorpherCore';
import {
  BasicRule, BASIC_KINDS, ValueChoice, fromAdvanced, toAdvanced, normaliseBasic, basicSentence,
  hasNumber, valueChoices, valueMax, outKindOf, numberText, parseNote, kindShort, applyLearned,
} from './MorpherBasic';
import { useMidiLearn } from './Learn';

// ── Learn (v0.0.929) ─────────────────────────────────────────────────────────
type LearnTarget = 'in' | 'out';
interface LearnProps { target: LearnTarget | null; start: (t: LearnTarget) => void }

function LearnButton ({ armed, onClick }: { armed: boolean; onClick: () => void }) {
  return (
    <span onClick={onClick}
      title={armed ? 'Listening — move a control or play a key (click to cancel)'
                   : 'Learn: move a control or play a key on a connected controller to fill this in'}
      style={{ fontSize: 9, letterSpacing: 0, textTransform: 'none', cursor: 'pointer', padding: '1px 6px',
               borderRadius: 3, border: `1px solid ${armed ? ACCENT : 'var(--border)'}`,
               color: armed ? 'var(--bg)' : 'var(--text-dim)', background: armed ? ACCENT : 'transparent',
               animation: armed ? 'patchyLearnPulse 1s ease-in-out infinite' : 'none' }}>
      {armed ? '● Listening…' : 'Learn'}
    </span>
  );
}
// One keyframe for the pulsing Learn button (injected once)
if (typeof document !== 'undefined' && ! document.getElementById ('patchy-learn-kf')) {
  const st = document.createElement ('style');
  st.id = 'patchy-learn-kf';
  st.textContent = '@keyframes patchyLearnPulse { 0%,100% { opacity: 1 } 50% { opacity: .55 } }';
  document.head.appendChild (st);
}

export interface MidiMorpherNodeData {
  label:         string;
  nodeType:      28;
  ports:         RawPort[];
  disabled?:     boolean;
  settingsJson?: string;
  customName?:   string;
  [key: string]: unknown;
}

const ACCENT = 'var(--midi)';


// ── Range value: drag up/down to scrub, double-click to type ─────────────────
function RangeValue ({ value, isHi, max, onChange, onChangeLinked, onRelease, onCommit, dim, format, parse }: {
  value:     number;          // hi: -1 = Max
  isHi:      boolean;
  max:       number;
  onChange:  (v: number) => void;   // live, while dragging
  onChangeLinked: (v: number) => void;   // ⌥ Option + drag: low AND high of the row to v (0..max), this drag only
  onRelease: () => void;            // drag ended (closes the drag's undo step)
  onCommit:  (v: number) => void;   // typed value (one undo step)
  dim?:      boolean;
  format?:   (v: number) => string;   // v0.0.927 — single values in Basic (note names…)
  parse?:    (t: string) => number;   //            typed text → value (NaN = ignore)
}) {
  const [editing, setEditing] = useState (false);
  const [draft,   setDraft]   = useState ('');
  // Drag state. `cur` = this field's current value as a number (Max → max).
  const drag = useRef<{ y: number; v: number; cur: number; linked: boolean; lastY: number } | null> (null);
  const done = useRef (false);   // Enter/Escape already handled — ignore the blur that follows the unmount

  // Latest callbacks: the window listeners below live for the whole drag and
  // must not call the closures from when it started (after linking, the other
  // end of the range has moved; an old closure would put it back).
  const cbs = useRef ({ onChange, onChangeLinked, onRelease });
  cbs.current = { onChange, onChangeLinked, onRelease };

  const shown  = format ? format (value) : isHi ? fmtHi (value) : fmtLo (value);
  const actual = isHi && value < 0 ? max : value;
  const store  = (v: number) => (isHi && v >= max ? -1 : Math.max (0, Math.min (max, v)));
  const step   = max > 127 ? 128 : 1;   // pitch bend: one MIDI-ish step per 3 px

  const commitDraft = () => {
    if (done.current) return;
    done.current = true;
    const t = draft.trim().toLowerCase();
    const n = parse ? parse (draft) : t === 'max' ? max : t === 'min' ? 0 : parseInt (t, 10);
    if (! isNaN (n)) onCommit (store (n));
    setEditing (false);
  };

  const onMouseDown = (e: React.MouseEvent) => {
    // No stopPropagation here (nor on the panel): React's stop also stops the
    // NATIVE event at the root, so window/document listeners — this drag's
    // mouseup, NodeSelect's outside-click — never fire and controls stay
    // "stuck". The `nodrag` class is what keeps ReactFlow from moving the node.
    e.preventDefault();

    // v0.0.926 (user's idea) — ⌥ Option links the row's low and high: both go
    // to this field's value and follow the drag together (a single value such
    // as CC 7–7). Read LIVE during the drag: pressing ⌥ mid-drag links from
    // the current value, releasing it unlinks (only this field keeps moving).
    // Each switch re-anchors the drag at the current mouse position/value.
    drag.current = { y: e.clientY, v: actual, cur: actual, linked: false, lastY: e.clientY };

    const setLinked = (on: boolean, y: number) => {
      const d = drag.current;
      if (! d || d.linked === on) return;
      d.linked = on; d.y = y; d.v = d.cur;
      if (on) cbs.current.onChangeLinked (d.cur);
    };
    setLinked (e.altKey, e.clientY);

    const move = (ev: MouseEvent) => {
      const d = drag.current;
      if (! d) return;
      d.lastY = ev.clientY;
      setLinked (ev.altKey, ev.clientY);
      const delta = Math.round ((d.y - ev.clientY) / 3) * step;
      const next  = clampTo (d.v + delta, max);
      if (next === d.cur) return;
      d.cur = next;
      if (d.linked) cbs.current.onChangeLinked (next);
      else          cbs.current.onChange (store (next));
    };
    const key = (ev: KeyboardEvent) => {
      if (ev.key === 'Alt' && drag.current) setLinked (ev.type === 'keydown', drag.current.lastY);
    };
    const up = () => {
      drag.current = null;
      window.removeEventListener ('mousemove', move, true);
      window.removeEventListener ('mouseup',   up,   true);
      window.removeEventListener ('keydown',   key,  true);
      window.removeEventListener ('keyup',     key,  true);
      cbs.current.onRelease();
    };
    // Capture phase: runs before anything below window can stop the event.
    window.addEventListener ('mousemove', move, true);
    window.addEventListener ('mouseup',   up,   true);
    window.addEventListener ('keydown',   key,  true);
    window.addEventListener ('keyup',     key,  true);
  };

  const box: React.CSSProperties = {
    width: 46, textAlign: 'center', fontSize: 10, padding: '2px 2px', borderRadius: 3,
    border: '1px solid var(--border)', background: 'var(--surface)',
    color: dim ? 'var(--text-muted)' : 'var(--text)', fontFamily: "'JetBrains Mono', monospace",
    opacity: dim ? 0.5 : 1,
  };

  return editing ? (
    <input autoFocus value={draft} className="nodrag"
      onChange={e => setDraft (e.target.value)}
      onBlur={commitDraft}
      onKeyDown={e => { e.stopPropagation(); if (e.key === 'Enter') commitDraft(); if (e.key === 'Escape') { done.current = true; setEditing (false); } }}
      style={{ ...box, outline: 'none', borderColor: ACCENT }} />
  ) : (
    <span className="nodrag" title="Drag up/down • ⌥ Option + drag: low and high together • double-click to type (a number, min or max)"
      onMouseDown={onMouseDown}
      onDoubleClick={e => { e.stopPropagation(); done.current = false; setDraft (format ? format (actual) : String (actual)); setEditing (true); }}
      style={{ ...box, cursor: 'ns-resize', userSelect: 'none', display: 'inline-block' }}>
      {shown}
    </span>
  );
}


const WARN  = '#fbbf24';
const FLASH_MS = 120;

// ── Transfer curve: how one OUT data value follows its IN source ────────────
function TransferCurve ({ m, label }: { m: SlotMap; label: string }) {
  const W = 120, H = 56, P = 4;
  const lo = rLo (m.srcRange, m.srcMax), hi = rHi (m.srcRange, m.srcMax);
  const a = Math.min (lo, hi), b = Math.max (lo, hi);
  const x = (v: number) => P + (v / m.srcMax) * (W - 2 * P);
  const y = (v: number) => H - P - (v / m.outMax) * (H - 2 * P);
  const out = (v: number) => scaleValue (m.srcHas, v, m.srcRange, m.srcMax, m.outRange, m.outMax);
  const pts: string[] = [];
  for (let i = 0; i <= 48; i++) {
    const v = a + (b - a) * i / 48;
    pts.push (`${x (v).toFixed (1)},${y (out (v)).toFixed (1)}`);
  }
  const shade = { fill: 'var(--text-muted)', opacity: 0.15 };
  return (
    <div style={{ opacity: m.exists ? 1 : 0.35 }}>
      <div style={{ fontSize: 9, color: 'var(--text-dim)', marginBottom: 2, whiteSpace: 'nowrap' }}>
        {label} <span style={{ color: 'var(--text-muted)' }}>← {m.srcHas ? m.srcName : `${m.srcName} (none)`}</span>
      </div>
      <svg width={W} height={H} style={{ display: 'block', background: 'var(--surface)', border: '1px solid var(--border)', borderRadius: 3 }}>
        {a > 0        && <rect x={P}     y={P} width={x (a) - P}     height={H - 2 * P} {...shade} />}
        {b < m.srcMax && <rect x={x (b)} y={P} width={W - P - x (b)} height={H - 2 * P} {...shade} />}
        {a === b
          ? <circle cx={x (a)} cy={y (out (a))} r={2.5} fill={ACCENT} />
          : <polyline points={pts.join (' ')} fill="none" stroke={ACCENT} strokeWidth={1.5} />}
      </svg>
      <div style={{ display: 'flex', justifyContent: 'space-between', fontSize: 8, color: 'var(--text-muted)', marginTop: 1 }}>
        <span>IN {a}–{b}</span><span>OUT {out (a)}→{out (b)}</span>
      </div>
    </div>
  );
}

// ── Basic | Advanced switch (top of both panels) ────────────────────────────
function ModeSwitch ({ mode, onPick }: { mode: 'basic' | 'advanced'; onPick: (m: 'basic' | 'advanced') => void }) {
  const btn = (m: 'basic' | 'advanced', label: string) => (
    <div onClick={() => mode !== m && onPick (m)}
      style={{ flex: 1, textAlign: 'center', padding: '3px 0', fontSize: 10, cursor: mode === m ? 'default' : 'pointer',
               background: mode === m ? 'color-mix(in srgb, var(--midi) 22%, transparent)' : 'transparent',
               color: mode === m ? ACCENT : 'var(--text-dim)', fontWeight: mode === m ? 700 : 400 }}>{label}</div>
  );
  return (
    <div style={{ display: 'flex', border: '1px solid var(--border)', borderRadius: 4, overflow: 'hidden', marginBottom: 8 }}>
      {btn ('basic', 'Basic')}{btn ('advanced', 'Advanced')}
    </div>
  );
}

const panelStyle: React.CSSProperties = {
  position: 'absolute', top: 0, left: '100%', marginLeft: 6, width: 300,
  background: 'var(--surface2)', border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
  padding: '10px 12px', zIndex: 1000, boxShadow: '0 8px 32px rgba(0,0,0,.6)',
  fontFamily: "'JetBrains Mono', monospace", userSelect: 'none',
};

const VALUE_LABELS: Record<ValueChoice, string> = {
  same: 'same', inverted: 'inverted', fixed: 'fixed at', limited: 'limited to',
};

// ── Basic panel: When / Send / Value in plain words ─────────────────────────
function BasicPanel ({ rule, onLive, onRelease, onCommit, onClose, onReset, learn }: {
  learn:     LearnProps;
  rule:      MorpherRule;
  onLive:    (r: MorpherRule) => void;
  onRelease: () => void;
  onCommit:  (r: MorpherRule) => void;
  onClose:   () => void;
  onReset:   () => void;
}) {
  const b = fromAdvanced (rule);
  const conv = rule.noteNames;

  const head = (
    <>
      <SettingsPanelHeader title="MIDI Morpher" onReset={onReset} onClose={onClose} />
      <ModeSwitch mode="basic" onPick={m => onCommit ({ ...rule, mode: m })} />
    </>
  );

  // Too complex for Basic: show it, offer the way out.
  if (! b) {
    return (
      <div className="nodrag" onDoubleClick={e => e.stopPropagation()} style={panelStyle}>
        {head}
        <div style={{ fontSize: 10, color: 'var(--text)', marginBottom: 6 }}>{ruleSummary (rule)}</div>
        <div style={{ fontSize: 9, color: 'var(--text-muted)', lineHeight: 1.5, marginBottom: 8 }}>
          This rule uses Advanced settings (ranges, swaps…) that Basic can't show. It works as it is;
          edit it in Advanced, or press R to start again from a simple rule.
        </div>
        <div onClick={() => onCommit ({ ...rule, mode: 'advanced' })}
          style={{ textAlign: 'center', padding: '4px 0', fontSize: 10, cursor: 'pointer', borderRadius: 4,
                   border: `1px solid ${ACCENT}`, color: ACCENT }}>Edit in Advanced</div>
      </div>
    );
  }

  const set  = (patch: Partial<BasicRule>) => onCommit (toAdvanced (normaliseBasic ({ ...b, ...patch }), rule));
  const live = (patch: Partial<BasicRule>) => onLive   (toAdvanced (normaliseBasic ({ ...b, ...patch }), rule));

  const outType = outKindOf (b);
  const choices = valueChoices (b);
  const vmax    = valueMax (outType);
  const anyNote = [b.inKind, outType].some (k => k >= 1 && k <= 3);

  const section = (title: string, learnFor?: LearnTarget) => (
    <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase',
                  marginTop: 10, marginBottom: 6, borderTop: '1px solid var(--border)', paddingTop: 6,
                  display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
      <span>{title}</span>
      {learnFor && <LearnButton armed={learn.target === learnFor} onClick={() => learn.start (learnFor)} />}
    </div>
  );
  const row = (children: React.ReactNode) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 6, flexWrap: 'wrap' }}>{children}</div>
  );
  const word = (t: string) => <span style={{ fontSize: 10, color: 'var(--text-dim)' }}>{t}</span>;
  const select = (value: string, options: { id: string; name: string }[], onPick: (v: string) => void, width = 150) => (
    <div style={{ width }}>
      <NodeSelect value={value} onChange={onPick} options={options} accent={ACCENT} showEmpty={false} />
    </div>
  );
  const chOptions = (first: string) => [{ id: '0', name: first },
    ...Array.from ({ length: 16 }, (_, i) => ({ id: String (i + 1), name: `ch ${i + 1}` }))];
  const kinds = (withSame: boolean) => [
    ...(withSame ? [{ id: '0', name: 'Same' }] : []),
    ...BASIC_KINDS.filter (k => ! withSame || k.code !== 0).map (k => ({ id: String (k.code), name: k.name })),
  ];
  // One value field (no Min/Max words; note names for notes)
  const one = (kind: number, v: number, max: number, onV: (n: number) => void, onVLive: (n: number) => void) => (
    <RangeValue value={v} isHi={false} max={max}
      format={n => numberText (kind, n, conv)} parse={t => parseNote (t, conv)}
      onChange={onVLive} onChangeLinked={onVLive} onRelease={onRelease} onCommit={onV} />
  );

  return (
    <div className="nodrag" onDoubleClick={e => e.stopPropagation()} style={panelStyle}>
      {head}

      {section ('When', 'in')}
      {row (<>
        {select (String (b.inKind), kinds (false), v => set ({ inKind: Number (v) }))}
      </>)}
      {hasNumber (b.inKind) && row (<>
        <Checkbox checked={b.inNum === null} label="any" accent={ACCENT}
          onChange={any => set ({ inNum: any ? null : (b.inKind <= 3 ? 60 : 0) })} />
        {b.inNum !== null && one (b.inKind, b.inNum, 127, n => set ({ inNum: n }), n => live ({ inNum: n }))}
      </>)}
      {row (<>{word ('on')}{select (String (b.inCh), chOptions ('Omni'), v => set ({ inCh: Number (v) }), 100)}</>)}

      {section ('Send', b.inKind !== 0 ? 'out' : undefined)}
      {b.inKind === 0
        ? row (word ('the same event (pick a "When" event to convert it)'))
        : row (select (String (b.outKind), kinds (true), v => set ({ outKind: Number (v) })))}
      {hasNumber (outType) && b.inKind !== 0 && row (<>
        {hasNumber (b.inKind) && (
          <Checkbox checked={b.outNum === null} label="same number" accent={ACCENT}
            onChange={same => set ({ outNum: same ? null : (b.inNum ?? (outType <= 3 ? 60 : 0)) })} />
        )}
        {b.outNum !== null && one (outType, b.outNum, 127, n => set ({ outNum: n }), n => live ({ outNum: n }))}
      </>)}
      {row (<>{word ('on')}{select (String (b.outCh), chOptions ('Same channel'), v => set ({ outCh: Number (v) }), 120)}</>)}

      {choices.length > 0 && <>
        {section (`Value (${kindShort (outType) === 'Any event' ? 'value' : valueLabelFor (outType)})`)}
        {row (<>
          {select (b.value, choices.map (c => ({ id: c, name: VALUE_LABELS[c] })),
            v => set (v === 'limited' ? { value: 'limited', limLo: 0, limHi: vmax } : { value: v as ValueChoice }), 110)}
          {b.value === 'fixed' && one (0, b.fixed, vmax, n => set ({ fixed: n }), n => live ({ fixed: n }))}
          {b.value === 'limited' && <>
            {one (0, b.limLo, vmax, n => set ({ limLo: n }), n => live ({ limLo: n }))}
            {word ('–')}
            {one (0, b.limHi, vmax, n => set ({ limHi: n }), n => live ({ limHi: n }))}
          </>}
        </>)}
      </>}

      {section ('Other events')}
      {row (select (rule.unmatched, [{ id: 'pass', name: 'Pass through' }, { id: 'block', name: 'Block' }],
        v => onCommit ({ ...rule, unmatched: v === 'block' ? 'block' : 'pass' }), 130))}

      {anyNote && <>
        {section ('Note names')}
        {row (select (conv, [{ id: 'yamaha', name: 'Yamaha (C3 = 60)' }, { id: 'roland', name: 'Roland (C4 = 60)' }],
          v => onCommit ({ ...rule, noteNames: v === 'roland' ? 'roland' : 'yamaha' }), 150))}
      </>}
    </div>
  );
}

function valueLabelFor (k: number): string {
  if (k === 1 || k === 2) return 'velocity';
  if (k === 3 || k === 6) return 'pressure';
  if (k === 7) return 'bend';
  return 'CC value';
}

// ── Settings panel (full rule editor) ────────────────────────────────────────
function MorpherPanel ({ rule, onLive, onRelease, onCommit, onClose, onReset, learn }: {
  learn:     LearnProps;
  rule:      MorpherRule;
  onLive:    (r: MorpherRule) => void;
  onRelease: () => void;
  onCommit:  (r: MorpherRule) => void;
  onClose:  () => void;
  onReset:  () => void;
}) {
  const outType = rule.outMsg === 0 ? rule.inMsg : rule.outMsg;

  const section = (title: string, learnFor?: LearnTarget) => (
    <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase',
                  marginTop: 10, marginBottom: 6, borderTop: '1px solid var(--border)', paddingTop: 6,
                  display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
      <span>{title}</span>
      {learnFor && <LearnButton armed={learn.target === learnFor} onClick={() => learn.start (learnFor)} />}
    </div>
  );
  const label = (t: string) => (
    <div style={{ width: 54, fontSize: 10, color: 'var(--text-dim)', flexShrink: 0 }}>{t}</div>
  );
  const row = (children: React.ReactNode) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 6 }}>{children}</div>
  );
  const select = (value: number, options: string[], onPick: (v: number) => void) => (
    <div style={{ width: 96 }}>
      <NodeSelect value={String (value)} onChange={v => onPick (Number (v))}
        options={options.map ((name, i) => ({ id: String (i), name }))} accent={ACCENT} showEmpty={false} />
    </div>
  );
  const chOptions = (first: string) => [first, ...Array.from ({ length: 16 }, (_, i) => String (i + 1))];

  const rangeRow = (name: string, key: 'inD1' | 'inD2' | 'outD1' | 'outD2', max: number, dim: boolean,
                    pull?: { key: 'outD1Pull' | 'outD2Pull'; label: string }) => {
    const [lo, hi] = rule[key];
    const linked = (v: number) => onLive ({ ...rule, [key]: [v, v >= max ? -1 : v] });   // ⌥ drag: both ends = v
    return row (<>
      {label (name)}
      <RangeValue value={lo} isHi={false} max={max} dim={dim} onChangeLinked={linked}
        onChange={v => onLive ({ ...rule, [key]: [v, hi] })} onRelease={onRelease} onCommit={v => onCommit ({ ...rule, [key]: [v, hi] })} />
      <RangeValue value={hi} isHi={true} max={max} dim={dim} onChangeLinked={linked}
        onChange={v => onLive ({ ...rule, [key]: [lo, v] })} onRelease={onRelease} onCommit={v => onCommit ({ ...rule, [key]: [lo, v] })} />
      {pull && (
        <div style={{ marginLeft: 4 }}>
          <Checkbox checked={rule[pull.key]} label={pull.label} accent={ACCENT}
            onChange={v => onCommit ({ ...rule, [pull.key]: v })} />
        </div>
      )}
    </>);
  };

  return (
    <div className="nodrag"
      onDoubleClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6, width: 290,
        background: 'var(--surface2)', border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000, boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace", userSelect: 'none',
      }}>
      <SettingsPanelHeader title="MIDI Morpher" onReset={onReset} onClose={onClose} />
      <ModeSwitch mode="advanced" onPick={m => onCommit ({ ...rule, mode: m })} />

      {ruleNotes (rule).map ((n, i) => (
        <div key={i} style={{ fontSize: 9, lineHeight: 1.4, marginBottom: 4, color: n.level === 'warn' ? WARN : 'var(--text-muted)' }}>
          {n.level === 'warn' ? '⚠ ' : 'ℹ '}{n.text}
        </div>
      ))}

      {section ('Input', 'in')}
      {row (<>{label ('Channel')}{select (rule.inCh,  chOptions ('Omni'), v => onCommit ({ ...rule, inCh: v }))}</>)}
      {row (<>{label ('Message')}{select (rule.inMsg, MSG_NAMES,         v => onCommit (normalise ({ ...rule, inMsg: v })))}</>)}
      {rangeRow ('Data1', 'inD1', data1Max (rule.inMsg), false)}
      {rangeRow ('Data2', 'inD2', 127, rule.inMsg !== 0 && ! hasData2 (rule.inMsg))}

      {section ('Output', 'out')}
      {row (<>{label ('Channel')}{select (rule.outCh,  chOptions ('Copy'), v => onCommit ({ ...rule, outCh: v }))}</>)}
      {row (<>{label ('Message')}{select (rule.outMsg, ['Copy', ...MSG_NAMES.slice (1)], v => onCommit (normalise ({ ...rule, outMsg: v })))}</>)}
      {rangeRow ('Data1', 'outD1', data1Max (outType), false, { key: 'outD1Pull', label: 'Pull 2' })}
      {rangeRow ('Data2', 'outD2', 127, outType !== 0 && ! hasData2 (outType), { key: 'outD2Pull', label: 'Pull 1' })}

      {section ('Transfer')}
      <div style={{ display: 'flex', gap: 10, marginBottom: 4 }}>
        <TransferCurve m={slotMap (rule, 1)} label="OUT Data1" />
        <TransferCurve m={slotMap (rule, 2)} label="OUT Data2" />
      </div>
      <div style={{ fontSize: 8, color: 'var(--text-muted)', marginBottom: 2 }}>
        Grey zones = IN values outside the rule (not morphed).
      </div>

      {section ('Not matching the rule')}
      {row (<>{label ('Events')}{select (rule.unmatched === 'block' ? 1 : 0, ['Pass through', 'Block'],
                                          v => onCommit ({ ...rule, unmatched: v === 1 ? 'block' : 'pass' }))}</>)}

      <div style={{ fontSize: 9, color: 'var(--text-muted)', lineHeight: 1.5, marginTop: 4 }}>
        ⌥ Option + drag sets a row's low and high together (one single value).
        OUT Min–Max keeps the value. Any other OUT range rescales the IN range onto it
        (high &lt; low inverts, a single IN value gives a fixed one). Pitch uses Data1, 0–16383.
      </div>
    </div>
  );
}

// ── Node ─────────────────────────────────────────────────────────────────────
export default function MidiMorpherNode ({ id, data, selected }: NodeProps) {
  const d = data as MidiMorpherNodeData;
  const { disabled, toggleDisabled }   = useNodeDisabled (id, d.disabled);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, (data as any)._forceCollapsed);
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings (id);
  const { handleDelete } = useNodeDelete (id);
  const { setHint } = useContext (HintContext);
  const customName = d.customName ?? '';

  // Local copy, updated optimistically; resynced whenever the backend's
  // settingsJson changes (undo/redo, project load, fragment import).
  const [rule, setRule] = useState<MorpherRule> (() => parseRule (d.settingsJson));
  useEffect (() => { setRule (parseRule (d.settingsJson)); }, [d.settingsJson]);

  // Other keys that may live in settingsJson are kept as they are.
  const merged = useCallback ((r: MorpherRule) => {
    let base: object = {};
    try { base = d.settingsJson ? JSON.parse (d.settingsJson) : {}; } catch {}
    return { ...base, ...r };
  }, [d.settingsJson]);

  const live = useCallback ((r: MorpherRule) => {
    setRule (r);
    Bridge.setNodeSettings (id, merged (r));
  }, [id, merged]);

  // Discrete change: one message, one undo step.
  const commit = useCallback ((r: MorpherRule) => {
    setRule (r);
    Bridge.commitSettingsChange (id, merged (r));
  }, [id, merged]);

  // End of a value drag: the backend took the pre-drag snapshot on the first
  // setNodeSettings; this turns the whole drag into one undo step (no-op if
  // the value never moved).
  const release = useCallback (() => { Bridge.commitNodeSettings (id); }, [id]);

  // ── Live feedback (v0.0.926): row flashes, funnel flash, last morph ──
  const flash = useRef ({ in: 0, out: 0, pass: 0, block: 0 });
  const rafRef = useRef<number | null> (null);
  const [, rerender] = useState (0);
  const [lastMorph, setLastMorph] = useState<{ in: MorphSide; out: MorphSide } | null> (null);
  const lastPacked = useRef ('');

  useEffect (() => {
    const tick = () => {
      rerender (t => t + 1);
      const now = Date.now(), f = flash.current;
      rafRef.current = Math.max (f.in, f.out, f.pass, f.block) > now ? requestAnimationFrame (tick) : null;
    };
    const unsub = Bridge.onPortActivity (entries => {
      const m = entries.find (e => e.id === id)?.morph;
      if (! m) return;
      const until = Date.now() + FLASH_MS, f = flash.current;
      if (m.m > 0) { f.in = until; f.out = until; }
      if (m.p > 0) f.pass  = until;
      if (m.b > 0) f.block = until;
      if (m.last !== lastPacked.current) {
        lastPacked.current = m.last;
        const dec = decodeMorph (m.last);
        if (dec) setLastMorph (dec);
      }
      if ((m.m || m.p || m.b) && rafRef.current === null)
        rafRef.current = requestAnimationFrame (tick);
    });
    return () => { unsub(); if (rafRef.current !== null) cancelAnimationFrame (rafRef.current); };
  }, [id]);

  const now = Date.now();
  const inLit = flash.current.in > now, outLit = flash.current.out > now;
  const passLit = flash.current.pass > now, blockLit = flash.current.block > now;
  const notes = ruleNotes (rule);
  const warnings = notes.filter (n => n.level === 'warn');

  const outType = rule.outMsg === 0 ? rule.inMsg : rule.outMsg;
  const block   = rule.unmatched === 'block';

  // Basic face: the same rule as a sentence (null = too complex for Basic)
  const isBasic  = rule.mode === 'basic';
  const basic    = isBasic ? fromAdvanced (rule) : null;
  const sentence = basic ? basicSentence (basic, rule.noteNames) : null;
  const resetRule = () => commit ({ ...DEFAULT_RULE, mode: rule.mode, noteNames: rule.noteNames });

  // v0.0.929 — Learn: the engine captures the next event at the MIDI In
  // (input held back meanwhile), applied to When/IN or Send/OUT, one undo step.
  const learn = useMidiLearn<LearnTarget> (id, (t, ev) => commit (applyLearned (rule, t, ev)));

  // Summary table cells
  const range = (r: Range, show: boolean) => (show ? `${fmtLo (r[0])} ${fmtHi (r[1])}` : '—');
  const inHas2  = rule.inMsg === 0 || hasData2 (rule.inMsg);
  const outHas2 = outType === 0 || hasData2 (outType);
  const rows = [
    { tag: 'IN',  ch: rule.inCh  ? String (rule.inCh)  : 'Omni', msg: MSG_NAMES[rule.inMsg] ?? '?',
      d1: range (rule.inD1, true), d2: range (rule.inD2, inHas2) },
    { tag: 'OUT', ch: rule.outCh ? String (rule.outCh) : 'Copy', msg: rule.outMsg ? (MSG_NAMES[rule.outMsg] ?? '?') : 'Copy',
      d1: range (rule.outD1, true) + (rule.outD1Pull ? ' ←2' : ''),
      d2: outHas2 ? range (rule.outD2, true) + (rule.outD2Pull ? ' ←1' : '') : '—' },
  ];
  const cell = (w: number, align: 'left' | 'right' | 'center' = 'left'): React.CSSProperties => ({
    width: w, minWidth: w, textAlign: align, whiteSpace: 'nowrap', padding: '0 4px',
  });

  return (
    <div style={{ ...nodeContainerStyle (ACCENT, !! selected, { bg: 'var(--surface)', glow: 'var(--midi-glow)', disabled }),
                  minWidth: 300 }}>
      <NodeHandle nodeId={id} label="MIDI In"  direction="in"  colour={ACCENT} index={0} total={1} anchor="table" />
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour={ACCENT} index={0} total={1} anchor="table" />

      <NodeHeader title={customName || 'MIDI MORPHER'} accent={ACCENT}
        subtitle={collapsed ? (sentence ? `${sentence.inText} → ${sentence.outText}` : ruleSummary (rule)) : undefined}
        rename={{ value: customName, placeholder: 'MIDI Morpher', onCommit: commitModelName (id) }}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
        disabled={disabled} onToggleDisabled={toggleDisabled}>
        <NodeHeaderButton onClick={() => commit ({ ...rule, mode: isBasic ? 'advanced' : 'basic' })}
          active={false} activeAccent={ACCENT}
          onHint={{ onMouseEnter: () => setHint ({ title: isBasic ? 'Basic mode' : 'Advanced mode',
                      body: isBasic ? 'The rule in plain words. Click to switch this node to Advanced (Data1/Data2 ranges, Pull…).'
                                    : 'Full MIDI rule editor. Click to switch this node to Basic (plain words).' }),
                    onMouseLeave: () => setHint (null) }}>
          <span style={{ fontSize: 10, fontWeight: 700, width: 12, textAlign: 'center', color: ACCENT }}>{isBasic ? 'B' : 'A'}</span>
        </NodeHeaderButton>
        <span style={{ borderRadius: 4, transition: 'box-shadow .08s',
                       boxShadow: blockLit ? '0 0 0 1.5px var(--error), 0 0 8px var(--error-glow)'
                                : passLit  ? '0 0 0 1.5px var(--text-muted)' : 'none' }}>
        <NodeHeaderButton onClick={() => commit ({ ...rule, unmatched: block ? 'pass' : 'block' })}
          active={block} activeAccent={ACCENT}
          onHint={{ onMouseEnter: () => setHint ({ title: block ? 'Unmatched: blocked' : 'Unmatched: pass through',
                      body: block ? 'Events that don\'t match the rule are dropped. Click to let them pass through unchanged.'
                                  : 'Events that don\'t match the rule pass through unchanged. Click to drop them instead.' }),
                    onMouseLeave: () => setHint (null) }}>
          {block ? <Funnel size={14} /> : <FunnelX size={14} />}
        </NodeHeaderButton>
        </span>
      </NodeHeader>

      {! collapsed && (
        <div data-port-anchor="table" onDoubleClick={toggleSettings}
          style={{ padding: '8px 6px 10px', fontSize: 10, cursor: 'pointer' }} title="Double-click to edit the rule">
          {isBasic ? (
            // Basic: the rule as a sentence — IN part and OUT part flash separately
            <div style={{ fontSize: 11, lineHeight: 1.6, padding: '2px 4px', color: 'var(--text)', maxWidth: 360 }}>
              {sentence ? <>
                <span style={{ borderRadius: 3, padding: '1px 3px', transition: 'background .08s',
                               background: inLit ? 'color-mix(in srgb, var(--midi) 28%, transparent)' : 'transparent' }}>{sentence.inText}</span>
                <span style={{ color: ACCENT, margin: '0 6px' }}>→</span>
                <span style={{ borderRadius: 3, padding: '1px 3px', transition: 'background .08s',
                               background: outLit ? 'color-mix(in srgb, var(--midi) 28%, transparent)' : 'transparent' }}>{sentence.outText}</span>
              </> : <>
                <span style={{ background: (inLit || outLit) ? 'color-mix(in srgb, var(--midi) 28%, transparent)' : 'transparent', borderRadius: 3 }}>{ruleSummary (rule)}</span>
                <div style={{ fontSize: 9, color: 'var(--text-muted)' }}>Advanced rule — open the panel to edit it in Advanced</div>
              </>}
            </div>
          ) : <>
          <div style={{ display: 'flex', color: 'var(--text-muted)', fontSize: 9, marginBottom: 3 }}>
            <span style={cell (34)} /><span style={cell (40)}>Chn</span><span style={cell (70)}>Msg</span>
            <span style={cell (80)}>Data1</span><span style={cell (80)}>Data2</span>
          </div>
          {rows.map (r => (
            <div key={r.tag} style={{ display: 'flex', marginBottom: 2, borderRadius: 3, transition: 'background .08s',
                                      background: (r.tag === 'IN' ? inLit : outLit) ? 'color-mix(in srgb, var(--midi) 28%, transparent)' : 'transparent' }}>
              <span style={{ ...cell (34, 'right'), color: 'var(--text-muted)', fontSize: 9 }}>{r.tag}</span>
              <span style={{ ...cell (40), color: 'var(--text)' }}>{r.ch}</span>
              <span style={{ ...cell (70), color: 'var(--text)' }}>{r.msg}</span>
              <span style={{ ...cell (80), color: 'var(--text)' }}>{r.d1}</span>
              <span style={{ ...cell (80), color: 'var(--text)' }}>{r.d2}</span>
            </div>
          ))}
          </>}

          {/* Last morph readout (+ ⚠ when part of the rule can't apply) */}
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 6, paddingTop: 5,
                        borderTop: '1px solid var(--border)', fontSize: 9, minHeight: 12 }}>
            {warnings.length > 0 && (
              <span style={{ color: WARN, cursor: 'help' }}
                onMouseEnter={() => setHint ({ title: 'Rule warning', body: warnings.map (w => w.text).join (' ') })}
                onMouseLeave={() => setHint (null)}>⚠</span>
            )}
            <span style={{ color: lastMorph ? 'var(--text-dim)' : 'var(--text-muted)', whiteSpace: 'nowrap',
                           overflow: 'hidden', textOverflow: 'ellipsis' }}>
              {learn.target
                ? <span style={{ color: ACCENT }}>● Learn {learn.target === 'in' ? (isBasic ? 'When' : 'Input') : (isBasic ? 'Send' : 'Output')}: move a control or play a key…</span>
                : lastMorph ? `${eventText (lastMorph.in)}  →  ${eventText (lastMorph.out)}` : 'waiting for a matching event…'}
            </span>
          </div>
        </div>
      )}

      {showSettings && ! collapsed && (isBasic
        ? <BasicPanel rule={rule} onLive={live} onRelease={release} onCommit={commit} onClose={closeSettings} onReset={resetRule} learn={learn} />
        : <MorpherPanel rule={rule} onLive={live} onRelease={release} onCommit={commit} onClose={closeSettings} onReset={resetRule} learn={learn} />
      )}
    </div>
  );
}
