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
//   PackNode  — the folded box: name, buttons (open in place, enter — coming
//               next, unpack, delete), one row per face port.
//   PackFrame — the open frame: dashed outline + title bar (fold, unpack);
//               dragging the title bar moves the pack's nodes (v0.0.930).

import { useContext, useEffect } from 'react';
import { NodeProps, useUpdateNodeInternals } from '@xyflow/react';
import { Package, Maximize2, Minimize2, LogIn, Ungroup, X } from 'lucide-react';
import type { RawPack } from './Bridge';
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
}

export interface PackNodeData {
  pack:    RawPack;
  ports:   PackPort[];
  actions: PackActions;
  [key: string]: unknown;
}

const titleStyle: React.CSSProperties = {
  fontSize: 11, fontWeight: 700, color: 'var(--text)', letterSpacing: '0.1em',
  fontFamily: "'Syne', sans-serif",
};

// ── Folded pack ──────────────────────────────────────────────────────────────
export function PackNode ({ id, data, selected }: NodeProps) {
  const { pack, ports, actions } = data as PackNodeData;
  const { setHint } = useContext (HintContext);
  const updateNodeInternals = useUpdateNodeInternals();

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
      </div>
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
