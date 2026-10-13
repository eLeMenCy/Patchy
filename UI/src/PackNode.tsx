// Patchy — Packs (v0.0.930): several nodes shown as one box.
//
// A pack is UI / project state only (backend: GraphModel::PackData). The
// processing graph never sees it: members stay ordinary nodes with ordinary
// connections. App.tsx turns the graph into what ReactFlow draws:
//   folded pack → members hidden, one "pack" node; edges crossing its border
//                 redrawn to it with their own ids, and its ports reuse the
//                 inner ports' ids — so the activity flashes (keyed by edge
//                 id / handle id) light the pack with no extra code.
//   open pack   → members shown, a "packFrame" node drawn behind them.
//
//   PackNode  — the folded box: name, buttons (settings, open in place,
//               enter — coming next, unpack, delete), one row per face port,
//               a one-line summary of the Pax values inside and (⚙) a panel
//               with every Pax parameter inside, grouped by node (v0.0.931).
//   PackFrame — the open frame: dashed outline + title bar (fold, unpack);
//               dragging the title bar moves the pack's nodes (v0.0.930).

import { useContext, useEffect, useState } from 'react';
import { NodeProps, EdgeProps, useUpdateNodeInternals, Handle, Position, BaseEdge, getBezierPath } from '@xyflow/react';
import { Package, Maximize2, Minimize2, LogIn, Ungroup, X, Settings, Pin } from 'lucide-react';
import { Bridge } from './Bridge';
import type { RawPack, RawPackPin, PaxParamInfo } from './Bridge';
import { HintContext } from './HintPanel';
import { NodeHandle, NodeHeaderButton, EditableTitle, freqLabel } from './NodeUtils';

export const PACK_ACCENT = 'var(--generic)';

export interface PackPort {
  handleId: string;          // the inner port's own id
  label:    string;          // "MIDI In"
  nodeName: string;          // the inner node it belongs to
  dir:      'in' | 'out';
  colour:   string;
}

export interface PackActions {
  open:   (packId: string) => void;
  close:  (packId: string) => void;
  unpack: (packId: string) => void;
  remove: (packId: string) => void;
  rename: (packId: string, name: string) => void;
  // v0.0.931 — a Pax parameter edited from the pack's panel: same messages as
  // the node's own controls (setNodeSettings + setPaxParameter while
  // dragging, commitNodeSettings at the end = one undo step)
  setParam:    (nodeId: string, index: number, value: number) => void;
  commitParam: (nodeId: string) => void;
  // v0.0.932 — the whole pin list (pin / unpin / rename = one undo step)
  setPins:     (packId: string, pins: RawPackPin[]) => void;
}

// v0.0.931 — a generic Pax inside the pack, with its current values
export interface PackPax {
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
}

export interface PackNodeData {
  pack:    RawPack;
  ports:   PackPort[];
  pax:     PackPax[];
  actions: PackActions;
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
function pairRole (x: PackPax, i: number): { role: 'lo' | 'hi'; lo: number; hi: number } | null {
  for (const [lo, hi] of x.pairs) {
    if (i === lo) return { role: 'lo', lo, hi };
    if (i === hi) return { role: 'hi', lo, hi };
  }
  return null;
}
const pairLabel = (p: PaxParamInfo) => p.name.replace (/\s*Low\b.*$/i, '') || 'Band';
const isHz      = (p: PaxParamInfo) => /hz/i.test (p.name);
const fmtPairEnd = (p: PaxParamInfo, v: number) => isHz (p) ? freqLabel (v) : fmtValue (p, v);
const fmtChoice  = (x: PackPax, i: number, p: PaxParamInfo, v: number) =>
  x.choices[i]?.[Math.round (v)] ?? fmtValue (p, v);

// Summary for the face: live (read-only) values first, each in its Pax's
// colour, then the settable ones in grey — "● Current Value 0.786 · Min 0.00
// · …" (up to 2 lines). A parameter name found in more than one Pax of the
// pack is prefixed with its node's name; a Pax's only live value then shows
// the node's name alone ("● OSC TO VALUE 0.5"). Live values take the colour
// of the Pax's output wires (what the value IS once sent), settings grey.
interface SummaryPart { text: string; colour?: string }
function paxSummary (pax: PackPax[], live: Map<string, number>, pinned: Set<string>) {
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

// ── One parameter in the pack's panel (same kinds of control as the node) ───
// `labelNode` replaces the parameter name (the pinned controls on the box
// show their own, renamable label); `big` = a pinned live value, larger.
function PackParamControl ({ x, p, i, live, actions, labelNode, big = false }: {
  x: PackPax; p: PaxParamInfo; i: number; live: number | undefined; actions: PackActions;
  labelNode?: React.ReactNode; big?: boolean;
}) {
  const { setHint } = useContext (HintContext);
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
    return (
      <div>
        <div style={row}>{labelNode ?? <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{pairLabel (p)}</span>}
          <span style={{ fontSize: 9, color: 'var(--text-dim)', fontVariantNumeric: 'tabular-nums' }}>
            {fmtPairEnd (p, lo)} – {fmtPairEnd (hp, hi)}{isHz (p) ? ' Hz' : ''}
          </span>
        </div>
        <div className="dual-range nodrag"
          onMouseEnter={() => setHint ({ title: `${pairLabel (p)} — ${x.name}`, body: `Two heads: Low and High (they push each other). Range shown: the node's own zoom (${fmtPairEnd (p, zMin)}–${fmtPairEnd (hp, zMax)}), changed on the node. Double-click a head to reset it.` })}
          onMouseLeave={() => setHint (null)}
          style={{ position: 'relative', height: 16, marginTop: 2 }}>
          <input type="range" min={zMin} max={zMax} step={step} value={Math.min (zMax, Math.max (zMin, lo))}
            onChange={e => setLo (Number (e.target.value))} onMouseUp={commit} onKeyUp={commit}
            onDoubleClick={() => { setLo (p.defaultValue); commit(); }}
            style={{ width: '100%', top: 5, left: 0, ['--thumb-color' as any]: x.accent }} />
          <input type="range" min={zMin} max={zMax} step={step} value={Math.min (zMax, Math.max (zMin, hi))}
            onChange={e => setHi (Number (e.target.value))} onMouseUp={commit} onKeyUp={commit}
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
                         border: `1px solid ${on ? PACK_ACCENT : 'var(--border)'}`,
                         color: on ? PACK_ACCENT : 'var(--text-muted)',
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
        <input type="number" className="nodrag" min={p.min} max={p.max} step={1} value={Math.round (v)}
          onChange={e => { const r = parseInt (e.target.value, 10); if (! isNaN (r)) actions.setParam (x.nodeId, i, Math.min (p.max, Math.max (p.min, r))); }}
          onBlur={() => actions.commitParam (x.nodeId)}
          onKeyDown={e => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }}
          style={{ width: 44, fontSize: 9, textAlign: 'right', background: 'var(--surface)', color: PACK_ACCENT,
                   border: '1px solid var(--border)', borderRadius: 3, padding: '2px 4px' }} />
      </div>
    );

  return (
    <div>
      <div style={row}>{label}
        <span style={{ fontSize: 9, color: 'var(--text-dim)', fontVariantNumeric: 'tabular-nums' }}>{fmtValue (p, v)}</span>
      </div>
      <input type="range" className="nodrag" min={p.min} max={p.max} step={(p.max - p.min) / 1000} value={v}
        onChange={e => { let nv = parseFloat (e.target.value); if (p.step >= 1) nv = Math.round (nv); actions.setParam (x.nodeId, i, nv); }}
        onMouseUp={() => actions.commitParam (x.nodeId)}
        onKeyUp={() => actions.commitParam (x.nodeId)}
        onDoubleClick={() => { actions.setParam (x.nodeId, i, p.defaultValue); actions.commitParam (x.nodeId); }}
        onMouseEnter={() => setHint ({ title: `${p.name} — ${x.name}`, body: `Range: ${p.min} to ${p.max}${p.step >= 1 ? ' (integer)' : ''}.\nDouble-click to reset to default (${p.defaultValue}). Same control as on the node inside the pack.` })}
        onMouseLeave={() => setHint (null)}
        style={{ width: '100%', display: 'block', ['--thumb-color' as any]: PACK_ACCENT }} />
    </div>
  );
}

const titleStyle: React.CSSProperties = {
  fontSize: 11, fontWeight: 700, color: 'var(--text)', letterSpacing: '0.1em',
  fontFamily: "'Syne', sans-serif",
};

// ── Folded pack ──────────────────────────────────────────────────────────────
export function PackNode ({ id, data, selected }: NodeProps) {
  const { pack, ports, pax, actions } = data as PackNodeData;
  const { setHint } = useContext (HintContext);
  const updateNodeInternals = useUpdateNodeInternals();
  const [showSettings, setShowSettings] = useState (false);

  // Live values of read-only Pax parameters (summary + panel); followed only
  // when the pack holds at least one
  const [live, setLive] = useState<Map<string, number>>(new Map());
  const paxKey  = pax.map (x => x.nodeId).join ('|');
  const hasLive = pax.some (x => x.params.some (p => p.readOnly));
  // v0.0.932 — pinned controls, in pin order (pins of nodes no longer in the
  // pack, or of parameters that no longer exist, are skipped)
  const pins: RawPackPin[] = pack.pins ?? [];
  const pinKey    = (nodeId: string, index: number) => `${nodeId}:${index}`;
  const pinnedSet = new Set (pins.map (q => pinKey (q.nodeId, q.index)));
  const pinned = pins.flatMap (q => {
    const x = pax.find (y => y.nodeId === q.nodeId);
    const p = x?.params[q.index];
    return x && p && x.shown[q.index] !== false ? [{ q, x, p }] : [];
  });
  const togglePin = (nodeId: string, index: number) =>
    actions.setPins (pack.id, pinnedSet.has (pinKey (nodeId, index))
      ? pins.filter (q => pinKey (q.nodeId, q.index) !== pinKey (nodeId, index))
      : [...pins, { nodeId, index, label: '' }]);
  const renamePin = (nodeId: string, index: number, label: string) =>
    actions.setPins (pack.id, pins.map (q => q.nodeId === nodeId && q.index === index ? { ...q, label } : q));

  const summary = paxSummary (pax, live, pinnedSet);

  // Hovering the summary shows it in full in the hint panel, refreshed with
  // the live values (a native title tooltip is a frozen snapshot)
  const [summaryHover, setSummaryHover] = useState (false);
  useEffect (() => {
    if (summaryHover) setHint ({ title: `${pack.name || 'PACK'} — values`, body: summary.parts.map (q => q.text).join ('\n') });
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

  const portLabel = (p: PackPort | undefined, align: 'left' | 'right') => p ? (
    <div style={{ textAlign: align, minWidth: 0 }}>
      <div style={{ fontSize: 10, color: 'var(--text)', whiteSpace: 'nowrap' }}>{p.label}</div>
      <div style={{ fontSize: 8, color: 'var(--text-muted)', whiteSpace: 'nowrap', overflow: 'hidden',
                    textOverflow: 'ellipsis', maxWidth: 120 }}>{p.nodeName}</div>
    </div>
  ) : <div />;

  return (
    <div onDoubleClick={() => actions.open (pack.id)} style={{
      minWidth: 220, background: 'var(--surface)',
      border: `1px solid ${selected ? PACK_ACCENT : 'var(--border-hi)'}`, borderTop: `3px solid ${PACK_ACCENT}`,
      borderRadius: 'var(--radius)', position: 'relative', fontFamily: "'JetBrains Mono', monospace",
      boxShadow: selected ? `0 0 0 1px ${PACK_ACCENT}, 0 8px 32px var(--generic-glow)`
                          : '0 4px 16px rgba(0,0,0,.5), 4px 4px 0 -1px var(--surface2), 4px 4px 0 0 var(--border)',
    }}>
      {ports.map ((p, i) => (
        <NodeHandle key={p.handleId} nodeId={id} portId={p.handleId} label={p.label} direction={p.dir}
          colour={p.colour} index={0} total={1}
          anchor={`row${(p.dir === 'in' ? ins : outs).indexOf (p)}`} />
      ))}

      {/* Header */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 4, padding: '6px 8px',
                    borderBottom: '1px solid var(--border)', userSelect: 'none' }}
        onMouseEnter={e => { if (! e.currentTarget.contains (e.relatedTarget as Node))
          setHint ({ title: 'Pack', body: 'Several nodes shown as one box. Its ports are the connections that cross its border. Double-click (or ⤢) to open it in place. Nothing changes in the processing: packing only tidies the canvas.' }); }}
        onMouseLeave={e => { if (! e.currentTarget.contains (e.relatedTarget as Node)) setHint (null); }}>
        <Package size={12} color={PACK_ACCENT} />
        <EditableTitle display={pack.name || 'PACK'} value={pack.name === 'PACK' ? '' : pack.name} placeholder="Pack"
          onCommit={name => actions.rename (pack.id, name || 'PACK')} textStyle={titleStyle} />
        <span style={{ fontSize: 9, color: 'var(--text-muted)', marginLeft: 2 }}>{pack.nodeIds.length} nodes</span>
        <div style={{ flex: 1 }} />
        <div style={{ display: 'flex', gap: 4 }} onDoubleClick={e => e.stopPropagation()}>
          {pax.length > 0 && (
            <NodeHeaderButton onClick={() => setShowSettings (v => ! v)} active={showSettings} activeAccent={PACK_ACCENT}
              onHint={hint ('Pack settings', 'Every Pax parameter inside this pack, grouped by node — the same controls as on the nodes themselves (one undo step per change).')}>
              <Settings size={11} color={showSettings ? PACK_ACCENT : 'var(--text-muted)'} />
            </NodeHeaderButton>
          )}
          <NodeHeaderButton onClick={() => actions.open (pack.id)}
            onHint={hint ('Open in place', 'Shows the nodes inside this pack, where they are, in a frame. Fold it back with the frame\'s ⤡ button.')}>
            <Maximize2 size={13} />
          </NodeHeaderButton>
          <span style={{ opacity: 0.35, cursor: 'default' }}
            {...hint ('Enter (coming next)', 'Will open the pack in its own view, with a way back to the main graph. Not available yet.')}>
            <NodeHeaderButton onClick={() => {}}><LogIn size={13} /></NodeHeaderButton>
          </span>
          <NodeHeaderButton onClick={() => actions.unpack (pack.id)}
            onHint={hint ('Unpack', 'Removes the pack and puts its nodes back on the canvas as they are (⌘⇧G). Undoable.')}>
            <Ungroup size={13} />
          </NodeHeaderButton>
          <NodeHeaderButton onClick={() => actions.remove (pack.id)} danger
            onHint={hint ('Delete pack', 'Deletes the pack AND the nodes inside it. Undoable (⌘Z).')}>
            <X size={13} />
          </NodeHeaderButton>
        </div>
      </div>

      {/* Face ports: inputs left, outputs right */}
      <div style={{ padding: '6px 10px 8px' }}>
        {ports.length === 0 && (
          <div style={{ fontSize: 9, color: 'var(--text-muted)', padding: '4px 0' }}>no connections across the pack's border</div>
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
        {pinned.length > 0 && ! showSettings && (
          <div className="nodrag" onDoubleClick={e => e.stopPropagation()}
            style={{ marginTop: ports.length > 0 ? 6 : 0, paddingTop: 4, borderTop: '1px dashed var(--border)', minWidth: 220 }}>
            {pinned.map (({ q, x, p }) => {
              const def = `${pairRole (x, q.index) ? pairLabel (p) : p.name} · ${x.name}`;
              return (
                <div key={pinKey (q.nodeId, q.index)} style={{ paddingTop: 4, display: 'flex', alignItems: 'flex-start', gap: 6 }}>
                  <span onClick={() => togglePin (q.nodeId, q.index)}
                    {...hint ('Unpin', 'Takes this control off the box (it stays in the ⚙ panel). Undoable.')}
                    style={{ cursor: 'pointer', display: 'inline-flex', paddingTop: 1, flexShrink: 0, color: PACK_ACCENT }}>
                    <Pin size={10} fill="currentColor" />
                  </span>
                  <div style={{ flex: 1, minWidth: 0 }}>
                  <PackParamControl x={x} p={p} i={q.index} live={live.get (pinKey (x.nodeId, q.index))} actions={actions}
                    big={p.readOnly}
                    labelNode={
                      <EditableTitle display={q.label || def} value={q.label} placeholder={def}
                        onCommit={name => renamePin (q.nodeId, q.index, name)}
                        textStyle={{ fontSize: 9, color: p.readOnly ? x.liveColour : 'var(--text-muted)', letterSpacing: '0.04em' }} />
                    } />
                  </div>
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
              <div style={{ fontSize: 9, fontWeight: 700, letterSpacing: '0.08em', color: x.accent,
                            textTransform: 'uppercase', marginBottom: 2 }}>{x.name}</div>
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
                               color: on ? PACK_ACCENT : 'var(--text-muted)', opacity: on ? 1 : 0.45 }}>
                      <Pin size={10} fill={on ? 'currentColor' : 'none'} />
                    </span>
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <PackParamControl x={x} p={p} i={i} live={live.get (pinKey (x.nodeId, i))} actions={actions} />
                    </div>
                  </div>
                );
              })}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}

// ── Open pack: frame drawn behind its members ────────────────────────────────
export interface PackFrameData {
  pack:    RawPack;
  actions: PackActions;
  ports:   PackPort[];   // v0.0.934 — border ports, ordered top to bottom
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
function FramePort ({ q, top }: { q: PackPort; top: number }) {
  const { setHint } = useContext (HintContext);
  const isIn = q.dir === 'in';
  const dot: React.CSSProperties = { background: q.colour, width: 10, height: 10, border: '2px solid var(--bg)', top, pointerEvents: 'all' };
  return (
    <>
      {/* outer: inputs take wires from the left, outputs give them to the right */}
      <Handle type={isIn ? 'target' : 'source'} position={isIn ? Position.Left : Position.Right}
        id={q.handleId} style={dot}
        onMouseEnter={() => setHint ({ title: `${q.label} — ${q.nodeName}`, body: `Pack ${isIn ? 'input' : 'output'}: wires from outside connect here; inside, it leads to ${q.nodeName}'s ${q.label}.` })}
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

export function PackFrame ({ id, data }: NodeProps) {
  const { pack, actions, ports = [] } = data as PackFrameData;
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
      border: `1.5px dashed color-mix(in srgb, var(--generic) 70%, transparent)`, borderRadius: 10,
      background: 'color-mix(in srgb, var(--generic) 4%, transparent)',
    }}>
      <div className="pack-frame-drag" style={{
        pointerEvents: 'auto', cursor: 'grab', display: 'flex', alignItems: 'center', gap: 6, height: FRAME_HEAD,
        padding: '0 8px', borderBottom: `1px dashed color-mix(in srgb, var(--generic) 45%, transparent)`,
        background: 'color-mix(in srgb, var(--generic) 10%, var(--bg))', borderRadius: '9px 9px 0 0',
        fontFamily: "'JetBrains Mono', monospace", userSelect: 'none',
      }}>
        <Package size={12} color={PACK_ACCENT} />
        <EditableTitle display={pack.name || 'PACK'} value={pack.name === 'PACK' ? '' : pack.name} placeholder="Pack"
          onCommit={name => actions.rename (pack.id, name || 'PACK')} textStyle={titleStyle} />
        <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>open</span>
        <div style={{ flex: 1 }} />
        <NodeHeaderButton onClick={() => actions.close (pack.id)}
          onHint={{ onMouseEnter: () => setHint ({ title: 'Fold the pack', body: 'Folds these nodes back into one box.' }), onMouseLeave: () => setHint (null) }}>
          <Minimize2 size={13} />
        </NodeHeaderButton>
        <NodeHeaderButton onClick={() => actions.unpack (pack.id)}
          onHint={{ onMouseEnter: () => setHint ({ title: 'Unpack', body: 'Removes the pack; its nodes stay where they are. Undoable.' }), onMouseLeave: () => setHint (null) }}>
          <Ungroup size={13} />
        </NodeHeaderButton>
      </div>
      {ins.map  ((q, k) => <FramePort key={q.handleId} q={q} top={topOf (k)} />)}
      {outs.map ((q, k) => <FramePort key={q.handleId} q={q} top={topOf (k)} />)}
    </div>
  );
}

// ── A wire crossing open-pack frames (v0.0.934) ──────────────────────────────
// Still the one real connection (real id + class → resting colour, activity
// flash, selection, delete all as for any wire), drawn through the frame
// port(s) it crosses: a bezier into each port from the left, out of it to the
// right, then on to its end.
export function PackRouteEdge ({ id, sourceX, sourceY, targetX, targetY, sourcePosition, targetPosition,
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

