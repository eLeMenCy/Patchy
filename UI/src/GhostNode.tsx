// GhostNode.tsx — v0.0.941 (user's idea, 2026-10-16)
//
// A node whose Pax isn't installed (project, fragment or library wrap made
// elsewhere). It keeps its place, its saved ports and its wires, and the
// backend keeps its whole data untouched (settings included) — the processing
// graph just has no processor for it, so it passes nothing. Installing the Pax
// and restarting Patchy brings the real node back.
import { useContext, useRef } from 'react';
import { NodeProps } from '@xyflow/react';
import { X, Ghost } from 'lucide-react';
import { HintContext } from './HintPanel';
import { useNodeDelete, useNodeCollapsed, NodeHeaderButton, NodeHandle, nodeContainerStyle, portColour } from './NodeUtils';
import { FoldTriangle } from './WrapNode';

interface GhostPort { id: string; label: string; type: string; direction: 'input' | 'output' }
export interface GhostNodeData {
  label?:      string;
  paxName?:    string;
  customName?: string;
  ports?:      GhostPort[];
  [key: string]: unknown;
}

const ROW = 14;   // NodeHandle's own spacing between ports

export default function GhostNode ({ id, data, selected }: NodeProps) {
  const d = data as GhostNodeData;
  const { setHint } = useContext(HintContext);
  const { handleDelete } = useNodeDelete (id);
  const bodyRef = useRef<HTMLDivElement>(null);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, d._forceCollapsed as boolean | undefined);   // fold to the header, like any node

  const ports   = d.ports ?? [];
  const inputs  = ports.filter (p => p.direction === 'input');
  const outputs = ports.filter (p => p.direction === 'output');
  const rows    = Math.max (inputs.length, outputs.length, 1);
  const name    = d.paxName || d.label || 'Pax';
  const accent  = 'var(--text-muted)';

  const hint = {
    title: `Missing: ${name}`,
    body:  `This Pax isn't installed, so the node passes nothing. Its settings and wires are kept and saved with the project. `
         + `Install "${name}" in the Pax folder and restart Patchy to bring it back.`,
  };

  const portLabel = (p: GhostPort, i: number, side: 'left' | 'right') => (
    <span key={p.id} style={{
      position: 'absolute', top: 12 + i * ROW - 6, [side]: 8,
      fontSize: 9, lineHeight: '12px', color: 'var(--text-muted)', opacity: 0.8, whiteSpace: 'nowrap',
    }}>{p.label}</span>
  );

  return (
    <div style={{
      minWidth: 200, userSelect: 'none',
      ...nodeContainerStyle (accent, !!selected, { bg: 'var(--surface)' }),
      // full shorthands (not borderStyle on top of nodeContainerStyle's solid
      // ones): React re-sets `border` when the colour changes (selection) and
      // the separate dashed style was lost
      // 2px dashes, readable; selection = an outline set off from the border
      // (the usual 1px ring right against it filled the gaps and the dashes faded)
      border:    `2px dashed ${selected ? 'var(--text-dim)' : 'var(--text-muted)'}`,
      borderTop: `3px dashed ${selected ? 'var(--text-dim)' : 'var(--text-muted)'}`,
      boxShadow: '0 4px 16px rgba(0,0,0,.5)',
      outline:   selected ? '1px solid var(--text-dim)' : 'none', outlineOffset: 3,
      opacity: 0.85,
    }}>
      <div
        onMouseEnter={() => setHint (hint)} onMouseLeave={() => setHint (null)}
        onDoubleClick={toggleCollapsed}
        style={{ padding: '8px 10px 6px', borderBottom: collapsed ? 'none' : '1px dashed var(--border)', cursor: 'pointer',
                 display: 'flex', alignItems: 'center', gap: 6 }}>
        <FoldTriangle collapsed={collapsed} colour={accent} />
        <Ghost size={12} color={accent} />
        <div style={{ flex: 1, minWidth: 0, fontSize: 11, fontWeight: 700, letterSpacing: '0.1em',
                      fontFamily: "'Syne', sans-serif", color: 'var(--text-muted)',
                      whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
          MISSING: {name.toUpperCase()}
        </div>
        <NodeHeaderButton onClick={handleDelete} danger
          onHint={{ onMouseEnter: () => setHint ({ title: 'Delete node', body: 'Deletes this ghost node and its wires. Undoable (⌘Z).' }),
                    onMouseLeave: () => setHint (hint) }}>
          <X size={14} />
        </NodeHeaderButton>
      </div>

      {! collapsed && <div ref={bodyRef} style={{ position: 'relative', minHeight: 24 + (rows - 1) * ROW + 8 }}>
        {d.customName && (
          <div style={{ position: 'absolute', left: 0, right: 0, bottom: 4, textAlign: 'center',
                        fontSize: 9, color: 'var(--text-muted)', opacity: 0.6 }}>{d.customName}</div>
        )}
        {inputs.map  ((p, i) => portLabel (p, i, 'left'))}
        {outputs.map ((p, i) => portLabel (p, i, 'right'))}
      </div>}

      {inputs.map ((p, i) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in" colour={portColour (p.type)}
          index={i} total={inputs.length} portBodyRef={bodyRef} portId={p.id} />
      ))}
      {outputs.map ((p, i) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out" colour={portColour (p.type)}
          index={i} total={outputs.length} portBodyRef={bodyRef} portId={p.id} />
      ))}
    </div>
  );
}
