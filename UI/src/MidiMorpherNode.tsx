// Patchy — MIDI MORPHER node UI (v0.0.925, 2026-10-06)
//
// One rule per node (see Source/MidiMorpherNode.h for the engine and the
// exact semantics). This is the "Advanced" face: the node body is a
// MidiDash-style IN/OUT summary table, the cog opens the full rule editor.
// The Basic face (sentence view, friendly names, Learn) comes later and
// edits the same settingsJson.
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

type Range = [number, number];   // [lo, hi], hi -1 = Max

export interface MorpherRule {
  inCh: number; inMsg: number; inD1: Range; inD2: Range;
  outCh: number; outMsg: number; outD1: Range; outD2: Range;
  outD1Pull: boolean; outD2Pull: boolean;
  unmatched: 'pass' | 'block';
}

export const DEFAULT_RULE: MorpherRule = {
  inCh: 0, inMsg: 0, inD1: [0, -1], inD2: [0, -1],
  outCh: 0, outMsg: 0, outD1: [0, -1], outD2: [0, -1],
  outD1Pull: false, outD2Pull: false,
  unmatched: 'pass',
};

// Short names match MidiDash's menu (node body + Advanced panel).
export const MSG_NAMES = ['Any', 'NoteOff', 'NoteOn', 'Afttouch', 'CC', 'Program', 'ChannelAT', 'Pitch'];
const PITCH = 7;
const hasData2 = (m: number) => m >= 1 && m <= 4;
const data1Max = (m: number) => (m === PITCH ? 16383 : 127);

function parseRange (v: unknown, fallback: Range): Range {
  return Array.isArray (v) && v.length === 2 && typeof v[0] === 'number' && typeof v[1] === 'number'
    ? [v[0], v[1]] : fallback;
}

export function parseRule (json: string | undefined): MorpherRule {
  if (! json) return { ...DEFAULT_RULE };
  try {
    const p = JSON.parse (json);
    const num = (k: string, d: number) => (typeof p[k] === 'number' ? p[k] : d);
    return {
      inCh:  num ('inCh', 0),  inMsg:  num ('inMsg', 0),
      inD1:  parseRange (p.inD1, [0, -1]),  inD2:  parseRange (p.inD2, [0, -1]),
      outCh: num ('outCh', 0), outMsg: num ('outMsg', 0),
      outD1: parseRange (p.outD1, [0, -1]), outD2: parseRange (p.outD2, [0, -1]),
      outD1Pull: !! p.outD1Pull, outD2Pull: !! p.outD2Pull,
      unmatched: p.unmatched === 'block' ? 'block' : 'pass',
    };
  } catch { return { ...DEFAULT_RULE }; }
}

/** Keep explicit values inside a slot's range after a message-type change. */
function clampRange ([lo, hi]: Range, max: number): Range {
  const l = Math.min (lo, max);
  const h = hi < 0 || hi >= max ? -1 : hi;
  return [l, h];
}

/** Re-clamp every data range to the current message types. */
function normalise (r: MorpherRule): MorpherRule {
  const outType = r.outMsg === 0 ? r.inMsg : r.outMsg;
  return {
    ...r,
    inD1:  clampRange (r.inD1,  data1Max (r.inMsg)),
    inD2:  clampRange (r.inD2,  127),
    outD1: clampRange (r.outD1, data1Max (outType)),
    outD2: clampRange (r.outD2, 127),
  };
}

const fmtLo = (v: number) => (v === 0 ? 'Min' : String (v));
const fmtHi = (v: number) => (v < 0 ? 'Max' : String (v));

// ── Range value: drag up/down to scrub, double-click to type ─────────────────
function RangeValue ({ value, isHi, max, onChange, onRelease, onCommit, dim }: {
  value:     number;          // hi: -1 = Max
  isHi:      boolean;
  max:       number;
  onChange:  (v: number) => void;   // live, while dragging
  onRelease: () => void;            // drag ended (closes the drag's undo step)
  onCommit:  (v: number) => void;   // typed value (one undo step)
  dim?:      boolean;
}) {
  const [editing, setEditing] = useState (false);
  const [draft,   setDraft]   = useState ('');
  const drag = useRef<{ y: number; v: number; last: number } | null> (null);
  const done = useRef (false);   // Enter/Escape already handled — ignore the blur that follows the unmount

  const shown  = isHi ? fmtHi (value) : fmtLo (value);
  const actual = isHi && value < 0 ? max : value;
  const store  = (v: number) => (isHi && v >= max ? -1 : Math.max (0, Math.min (max, v)));
  const step   = max > 127 ? 128 : 1;   // pitch bend: one MIDI-ish step per 3 px

  const commitDraft = () => {
    if (done.current) return;
    done.current = true;
    const t = draft.trim().toLowerCase();
    const n = t === 'max' ? max : t === 'min' ? 0 : parseInt (t, 10);
    if (! isNaN (n)) onCommit (store (n));
    setEditing (false);
  };

  const onMouseDown = (e: React.MouseEvent) => {
    // No stopPropagation here (nor on the panel): React's stop also stops the
    // NATIVE event at the root, so window/document listeners — this drag's
    // mouseup, NodeSelect's outside-click — never fire and controls stay
    // "stuck". The `nodrag` class is what keeps ReactFlow from moving the node.
    e.preventDefault();
    drag.current = { y: e.clientY, v: actual, last: value };
    const move = (ev: MouseEvent) => {
      if (! drag.current) return;
      const delta = Math.round ((drag.current.y - ev.clientY) / 3) * step;
      const next  = store (drag.current.v + delta);
      if (next !== drag.current.last) { drag.current.last = next; onChange (next); }
    };
    const up = () => {
      drag.current = null;
      window.removeEventListener ('mousemove', move, true);
      window.removeEventListener ('mouseup',   up,   true);
      onRelease();
    };
    // Capture phase: runs before anything below window can stop the event.
    window.addEventListener ('mousemove', move, true);
    window.addEventListener ('mouseup',   up,   true);
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
    <span className="nodrag" title="Drag up/down • double-click to type (a number, min or max)"
      onMouseDown={onMouseDown}
      onDoubleClick={e => { e.stopPropagation(); done.current = false; setDraft (String (actual)); setEditing (true); }}
      style={{ ...box, cursor: 'ns-resize', userSelect: 'none', display: 'inline-block' }}>
      {shown}
    </span>
  );
}

// ── Settings panel (full rule editor) ────────────────────────────────────────
function MorpherPanel ({ rule, onLive, onRelease, onCommit, onClose, onReset }: {
  rule:      MorpherRule;
  onLive:    (r: MorpherRule) => void;
  onRelease: () => void;
  onCommit:  (r: MorpherRule) => void;
  onClose:  () => void;
  onReset:  () => void;
}) {
  const outType = rule.outMsg === 0 ? rule.inMsg : rule.outMsg;

  const section = (title: string) => (
    <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase',
                  marginTop: 10, marginBottom: 6, borderTop: '1px solid var(--border)', paddingTop: 6 }}>{title}</div>
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
    return row (<>
      {label (name)}
      <RangeValue value={lo} isHi={false} max={max} dim={dim}
        onChange={v => onLive ({ ...rule, [key]: [v, hi] })} onRelease={onRelease} onCommit={v => onCommit ({ ...rule, [key]: [v, hi] })} />
      <RangeValue value={hi} isHi={true} max={max} dim={dim}
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
    <div className="nodrag nowheel"
      onDoubleClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6, width: 290,
        background: 'var(--surface2)', border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000, boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace", userSelect: 'none',
      }}>
      <SettingsPanelHeader title="MIDI Morpher" onReset={onReset} onClose={onClose} />

      {section ('Input')}
      {row (<>{label ('Channel')}{select (rule.inCh,  chOptions ('Omni'), v => onCommit ({ ...rule, inCh: v }))}</>)}
      {row (<>{label ('Message')}{select (rule.inMsg, MSG_NAMES,         v => onCommit (normalise ({ ...rule, inMsg: v })))}</>)}
      {rangeRow ('Data1', 'inD1', data1Max (rule.inMsg), false)}
      {rangeRow ('Data2', 'inD2', 127, rule.inMsg !== 0 && ! hasData2 (rule.inMsg))}

      {section ('Output')}
      {row (<>{label ('Channel')}{select (rule.outCh,  chOptions ('Copy'), v => onCommit ({ ...rule, outCh: v }))}</>)}
      {row (<>{label ('Message')}{select (rule.outMsg, ['Copy', ...MSG_NAMES.slice (1)], v => onCommit (normalise ({ ...rule, outMsg: v })))}</>)}
      {rangeRow ('Data1', 'outD1', data1Max (outType), false, { key: 'outD1Pull', label: 'Pull 2' })}
      {rangeRow ('Data2', 'outD2', 127, outType !== 0 && ! hasData2 (outType), { key: 'outD2Pull', label: 'Pull 1' })}

      {section ('Not matching the rule')}
      {row (<>{label ('Events')}{select (rule.unmatched === 'block' ? 1 : 0, ['Pass through', 'Block'],
                                          v => onCommit ({ ...rule, unmatched: v === 1 ? 'block' : 'pass' }))}</>)}

      <div style={{ fontSize: 9, color: 'var(--text-muted)', lineHeight: 1.5, marginTop: 4 }}>
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

  const outType = rule.outMsg === 0 ? rule.inMsg : rule.outMsg;
  const block   = rule.unmatched === 'block';

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
        rename={{ value: customName, placeholder: 'MIDI Morpher', onCommit: commitModelName (id) }}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
        disabled={disabled} onToggleDisabled={toggleDisabled}>
        <NodeHeaderButton onClick={() => commit ({ ...rule, unmatched: block ? 'pass' : 'block' })}
          active={block} activeAccent={ACCENT}
          onHint={{ onMouseEnter: () => setHint ({ title: block ? 'Unmatched: blocked' : 'Unmatched: pass through',
                      body: block ? 'Events that don\'t match the rule are dropped. Click to let them pass through unchanged.'
                                  : 'Events that don\'t match the rule pass through unchanged. Click to drop them instead.' }),
                    onMouseLeave: () => setHint (null) }}>
          {block ? <Funnel size={14} /> : <FunnelX size={14} />}
        </NodeHeaderButton>
      </NodeHeader>

      {! collapsed && (
        <div data-port-anchor="table" onDoubleClick={toggleSettings}
          style={{ padding: '8px 6px 10px', fontSize: 10, cursor: 'pointer' }} title="Double-click to edit the rule">
          <div style={{ display: 'flex', color: 'var(--text-muted)', fontSize: 9, marginBottom: 3 }}>
            <span style={cell (34)} /><span style={cell (40)}>Chn</span><span style={cell (70)}>Msg</span>
            <span style={cell (80)}>Data1</span><span style={cell (80)}>Data2</span>
          </div>
          {rows.map (r => (
            <div key={r.tag} style={{ display: 'flex', marginBottom: 2 }}>
              <span style={{ ...cell (34, 'right'), color: 'var(--text-muted)', fontSize: 9 }}>{r.tag}</span>
              <span style={{ ...cell (40), color: 'var(--text)' }}>{r.ch}</span>
              <span style={{ ...cell (70), color: 'var(--text)' }}>{r.msg}</span>
              <span style={{ ...cell (80), color: 'var(--text)' }}>{r.d1}</span>
              <span style={{ ...cell (80), color: 'var(--text)' }}>{r.d2}</span>
            </div>
          ))}
        </div>
      )}

      {showSettings && ! collapsed && (
        <MorpherPanel rule={rule} onLive={live} onRelease={release} onCommit={commit} onClose={closeSettings}
          onReset={() => commit ({ ...DEFAULT_RULE })} />
      )}
    </div>
  );
}
