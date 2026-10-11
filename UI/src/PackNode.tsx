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
import { NodeProps, useUpdateNodeInternals } from '@xyflow/react';
import { Package, Maximize2, Minimize2, LogIn, Ungroup, X, Settings } from 'lucide-react';
import { Bridge } from './Bridge';
import type { RawPack, PaxParamInfo } from './Bridge';
import { HintContext } from './HintPanel';
import { NodeHandle, NodeHeaderButton, EditableTitle } from './NodeUtils';

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
}

// v0.0.931 — a generic Pax inside the pack, with its current values
export interface PackPax {
  nodeId: string;
  name:   string;            // custom name or label
  params: PaxParamInfo[];
  values: number[];          // one per param (settingsJson, else default)
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

// Summary for the face: live (read-only) values first, each in its Pax's
// colour, then the settable ones in grey — "● Current Value 0.786 · Min 0.00
// · …" (up to 2 lines). A parameter name found in more than one Pax of the
// pack is prefixed with its node's name; a Pax's only live value then shows
// the node's name alone ("● OSC TO VALUE 0.5"). Live values take the colour
// of the Pax's output wires (what the value IS once sent), settings grey.
interface SummaryPart { text: string; colour?: string }
function paxSummary (pax: PackPax[], live: Map<string, number>) {
  const count = new Map<string, number>();
  for (const x of pax) for (const p of x.params) count.set (p.name, (count.get (p.name) ?? 0) + 1);
  const liveParts: SummaryPart[] = [], setParts: SummaryPart[] = [];
  for (const x of pax)
    x.params.forEach ((p, i) => {
      const v    = p.readOnly ? (live.get (`${x.nodeId}:${i}`) ?? x.values[i] ?? p.defaultValue) : (x.values[i] ?? p.defaultValue);
      const shared   = (count.get (p.name) ?? 0) > 1;
      const onlyLive = p.readOnly && x.params.filter (q => q.readOnly).length === 1;
      const name = ! shared ? p.name : onlyLive ? x.name.toUpperCase() : `${x.name.toUpperCase()} ${p.name}`;
      if (p.readOnly) liveParts.push ({ text: `● ${name} ${fmtValue (p, v)}`, colour: x.liveColour });
      else            setParts.push  ({ text: `${name} ${fmtValue (p, v)}` });
    });
  const parts = [...liveParts, ...setParts];
  return { parts, full: parts.map (q => q.text).join (' · ') };
}

// ── One parameter in the pack's panel (same kinds of control as the node) ───
function PackParamControl ({ x, p, i, live, actions }: {
  x: PackPax; p: PaxParamInfo; i: number; live: number | undefined; actions: PackActions;
}) {
  const { setHint } = useContext (HintContext);
  const v = x.values[i] ?? p.defaultValue;
  const row: React.CSSProperties = { display: 'flex', justifyContent: 'space-between', alignItems: 'center', gap: 8 };
  const label = <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{p.name}</span>;

  if (p.readOnly)
    return (
      <div style={row}>{label}
        <span style={{ fontSize: 9, color: x.liveColour, fontVariantNumeric: 'tabular-nums' }}>
          {fmtValue (p, live ?? v)}
        </span>
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
  const summary = paxSummary (pax, live);

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
              {x.params.map ((p, i) => (
                <div key={i} style={{ paddingTop: 4 }}>
                  <PackParamControl x={x} p={p} i={i} live={live.get (`${x.nodeId}:${i}`)} actions={actions} />
                </div>
              ))}
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
  [key: string]: unknown;
}

export const FRAME_PAD  = 18;
export const FRAME_HEAD = 26;

export function PackFrame ({ data }: NodeProps) {
  const { pack, actions } = data as PackFrameData;
  const { setHint } = useContext (HintContext);
  return (
    <div style={{
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
    </div>
  );
}
