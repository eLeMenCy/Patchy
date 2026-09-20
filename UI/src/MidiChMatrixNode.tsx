// Patchy — MIDI CH. MATRIX node UI
//
// Stage 3a (2026-09-19): component shell + header only. The grid itself
// (crosshair hover, click-to-toggle cells) is Stage 3b, not yet built —
// this stage renders a placeholder body so the header/stepper/resize
// interaction can be verified on its own first.

import { useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import type { RawPort } from './Bridge';
import { NodeHandle, useNodeDisabled, useNodeCollapsed, NodeHeaderButton, NodeCollapseArrow } from './NodeUtils';
import { Power, X, Funnel, FunnelX } from 'lucide-react';

export interface MidiChMatrixNodeData {
  label:         string;
  nodeType:      27;
  ports:         RawPort[];
  disabled?:     boolean;
  settingsJson?: string;
  [key: string]: unknown;
}

const ACCENT   = 'var(--midi)';
const MIN_GRID = 4;
const MAX_GRID = 16;

interface MidiChMatrixSettings {
  gridSize:     number;
  dropUnmapped: boolean;
  cells:        [number, number][];
}

function parseSettings (json: string | undefined): MidiChMatrixSettings {
  const fallback: MidiChMatrixSettings = { gridSize: MIN_GRID, dropUnmapped: true, cells: [] };
  if (! json) return fallback;
  try {
    const parsed = JSON.parse (json);
    return {
      gridSize:     typeof parsed.gridSize === 'number' ? parsed.gridSize : fallback.gridSize,
      dropUnmapped: typeof parsed.dropUnmapped === 'boolean' ? parsed.dropUnmapped : fallback.dropUnmapped,
      cells:        Array.isArray (parsed.cells) ? parsed.cells : fallback.cells,
    };
  } catch { return fallback; }
}

export default function MidiChMatrixNode ({ id, data, selected }: NodeProps) {
  const { disabled, toggleDisabled }    = useNodeDisabled (id, (data as any).disabled);
  const { collapsed, toggleCollapsed }  = useNodeCollapsed (id, (data as any)._forceCollapsed);

  // Local state, optimistically updated on every interaction — matches
  // DmxConsoleNode.tsx's own established pattern. data.settingsJson only
  // refreshes on a structural graph change or undo/redo, never on a live,
  // per-cell dispatch like this node's own — deriving settings fresh from
  // it on every render left the visual genuinely stale until something
  // else triggered a full graph push (real bug, found by the user).
  const [settings, setSettings] = useState<MidiChMatrixSettings> (
    () => parseSettings ((data as any).settingsJson)
  );
  const { gridSize, dropUnmapped } = settings;

  // Resync from the backend whenever settingsJson genuinely changes (undo/
  // redo, project reload, fragment import) — every live interaction below
  // already updates the local state instantly on its own.
  useEffect (() => {
    setSettings (parseSettings ((data as any).settingsJson));
  }, [(data as any).settingsJson]);

  const handleResize = useCallback ((delta: number) => {
    const next = Math.max (MIN_GRID, Math.min (MAX_GRID, gridSize + delta));
    if (next === gridSize) return;
    setSettings (s => ({
      ...s,
      gridSize: next,
      // Resize preserves in-bounds cells, matching the backend's own
      // setGridSize() — only cells outside the new, smaller grid drop.
      cells: s.cells.filter (([r, c]) => r < next && c < next),
    }));
    Bridge.setNodeParam (id, 'midiChMatrixGridSize', String (next));
  }, [id, gridSize]);

  const handleToggleMode = useCallback(() => {
    const next = ! dropUnmapped;
    setSettings (s => ({ ...s, dropUnmapped: next }));
    Bridge.setNodeParam (id, 'midiChMatrixDropUnmapped', next ? '1' : '0');
  }, [id, dropUnmapped]);

  const handleReset = useCallback(() => {
    setSettings ({ gridSize: MIN_GRID, dropUnmapped: true, cells: [] });
    Bridge.setNodeParam (id, 'midiChMatrixReset', '1');
  }, [id]);

  const [hoveredCell, setHoveredCell] = useState<{ r: number; c: number } | null> (null);

  const handleToggleCell = useCallback ((r: number, c: number) => {
    setSettings (s => {
      const exists = s.cells.some (([cr, cc]) => cr === r && cc === c);
      return {
        ...s,
        cells: exists
          ? s.cells.filter (([cr, cc]) => ! (cr === r && cc === c))
          : [...s.cells, [r, c] as [number, number]],
      };
    });
    Bridge.setNodeParam (id, 'midiChMatrixCell', JSON.stringify ({ r, c }));
  }, [id]);

  // Channel-flash feature — Option B (self-contained, agreed with the
  // user over mirroring the existing edge-flash CSS-injection mechanism
  // in App.tsx, since this is "only a visual FX"): subscribes directly
  // to the same Bridge.onPortActivity() feed App.tsx itself uses,
  // filtered to this node's own id. Timestamps live in a ref (not
  // state) so a poll with no activity for this node never triggers a
  // render; a requestAnimationFrame loop runs only while at least one
  // channel is still within its own 80ms window, driving the periodic
  // re-renders needed to actually clear a label afterward.
  const flashRef = useRef<{ in: Map<number, number>; out: Map<number, number> }> ({ in: new Map(), out: new Map() });
  const rafRef   = useRef<number | null> (null);
  const [, forceFlashRerender] = useState (0);

  useEffect(() => {
    const stillFlashing = () => {
      const now = Date.now();
      for (const exp of flashRef.current.in.values())  if (exp > now) return true;
      for (const exp of flashRef.current.out.values()) if (exp > now) return true;
      return false;
    };
    const tick = () => {
      forceFlashRerender (t => t + 1);
      rafRef.current = stillFlashing() ? requestAnimationFrame (tick) : null;
    };

    return Bridge.onPortActivity ((entries) => {
      const entry = entries.find (e => e.id === id);
      if (! entry || (! entry.inChMask && ! entry.outChMask)) return;

      const now = Date.now();
      for (let ch = 0; ch < 16; ch++)
      {
        if (entry.inChMask  & (1 << ch)) flashRef.current.in.set  (ch, now + 80);
        if (entry.outChMask & (1 << ch)) flashRef.current.out.set (ch, now + 80);
      }
      if (rafRef.current === null)
        rafRef.current = requestAnimationFrame (tick);
    });
  }, [id]);

  return (
    <div style={{
      display: 'inline-block',
      width: 'fit-content',
      background: 'var(--surface)',
      border: `1px solid ${selected ? ACCENT : 'var(--border)'}`,
      borderTop: `3px solid ${ACCENT}`,
      borderRadius: 'var(--radius)',
      boxShadow: selected ? `0 0 0 1px ${ACCENT}, 0 8px 32px var(--midi-glow)` : '0 4px 16px rgba(0,0,0,.5)',
      fontFamily: "'JetBrains Mono', monospace",
      filter: disabled ? 'grayscale(0.8) opacity(0.55)' : 'none',
      transition: 'filter .15s',
      position: 'relative',
    }}>
      {/* MIDI In / Out handles — single in, single out, matching every other Pax/built-in transform */}
      <NodeHandle nodeId={id} label="MIDI In"  direction="in"  colour={ACCENT} index={0} total={1} offset={0} />
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour={ACCENT} index={0} total={1} offset={0} />

      {/* Header */}
      <div style={{
        display: 'flex', alignItems: 'center', padding: '4px 8px', gap: 4,
        background: 'color-mix(in srgb, var(--midi) 10%, transparent)',
        borderBottom: collapsed ? 'none' : `1px solid color-mix(in srgb, var(--midi) 27%, transparent)`,
        cursor: 'pointer',
      }} onDoubleClick={toggleCollapsed}>

        {/* Fold arrow + title + stepper, grouped together with no flex
            growth between them — kept visually anchored to the name
            rather than drifting apart as the grid grows the node wider */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <NodeCollapseArrow collapsed={collapsed} accent={ACCENT} />

          <div style={{
            fontSize: '11px', fontWeight: 700, color: ACCENT,
            letterSpacing: '0.1em', fontFamily: "'Syne', sans-serif",
            textTransform: 'uppercase', userSelect: 'none', whiteSpace: 'nowrap',
          }}>
            MIDI CH. MATRIX
          </div>

          {/* Stepper — deliberately inline (not a settings-panel slider), so
              the node itself visibly grows/shrinks with the grid */}
          <div className="nodrag" onDoubleClick={e => e.stopPropagation()}
            style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
            <button onClick={() => handleResize (-1)} disabled={gridSize <= MIN_GRID}
              style={{
                width: 16, height: 16, background: 'var(--surface2)', border: '1px solid var(--border)',
                borderRadius: 3, color: ACCENT, fontSize: 10, lineHeight: 1, padding: 0,
                cursor: gridSize <= MIN_GRID ? 'default' : 'pointer',
                opacity: gridSize <= MIN_GRID ? 0.4 : 1,
              }}>-</button>
            <span style={{ fontSize: 10, color: ACCENT, minWidth: 42, textAlign: 'center', userSelect: 'none' }}>
              {gridSize} x {gridSize}
            </span>
            <button onClick={() => handleResize (1)} disabled={gridSize >= MAX_GRID}
              style={{
                width: 16, height: 16, background: 'var(--surface2)', border: '1px solid var(--border)',
                borderRadius: 3, color: ACCENT, fontSize: 10, lineHeight: 1, padding: 0,
                cursor: gridSize >= MAX_GRID ? 'default' : 'pointer',
                opacity: gridSize >= MAX_GRID ? 0.4 : 1,
              }}>+</button>
          </div>
        </div>

        {/* Spacer — pushes the action buttons below to the right edge,
            instead of the title doing that job (which used to drag the
            stepper along with it as the node grew wider) */}
        <div style={{ flex: 1 }} />

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={toggleDisabled} active={! disabled} activeAccent={ACCENT}>
            <Power size={11} />
          </NodeHeaderButton>
        </div>

        {/* Drop/pass-through-unmapped mode toggle */}
        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={handleToggleMode} active={dropUnmapped} activeAccent={ACCENT}
            title={dropUnmapped ? 'Unmapped channels: dropped (click to pass through)' : 'Unmapped channels: pass through (click to drop)'}>
            {dropUnmapped ? <Funnel size={11} /> : <FunnelX size={11} />}
          </NodeHeaderButton>
        </div>

        {/* Reset — plain "R", matching this project's own established
            reset-button convention (not an icon) */}
        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={handleReset} title="Reset grid to 4x4 and clear all cells">
            <span style={{ fontSize: 11, fontWeight: 700 }}>R</span>
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={() => Bridge.removeNode (id)}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {! collapsed && (
        <div style={{ padding: '14px 14px 0px', display: 'flex', justifyContent: 'center' }}>
          <div className="nodrag" style={{
            display: 'inline-grid',
            gridTemplateColumns: `32px repeat(${gridSize}, 22px)`,
            gridTemplateRows: `repeat(${gridSize}, 22px) 44px`,
            gap: 2,
          }}>
            {/* Row labels */}
            {Array.from ({ length: gridSize }, (_, r) => {
              const isFlashing     = (flashRef.current.in.get (r) ?? 0) > Date.now();
              const isHoverAligned = !! hoveredCell && hoveredCell.r === r;
              return (
                <div key={`rl${r}`} style={{
                  gridColumn: 1, gridRow: r + 1,
                  fontSize: 8, lineHeight: '22px', userSelect: 'none', whiteSpace: 'nowrap',
                  color: isFlashing ? ACCENT : isHoverAligned ? 'color-mix(in srgb, var(--midi) 75%, var(--text-muted))' : 'var(--text-muted)',
                  fontWeight: isFlashing ? 700 : 400,
                }}>Ch. {r + 1}</div>
              );
            })}

            {/* Cells */}
            {Array.from ({ length: gridSize }, (_, r) =>
              Array.from ({ length: gridSize }, (_, c) => {
                const isOn           = settings.cells.some (([cr, cc]) => cr === r && cc === c);
                const isIntersection = !! hoveredCell && hoveredCell.r === r && hoveredCell.c === c;
                const isInBand       = !! hoveredCell && ! isIntersection && (hoveredCell.r === r || hoveredCell.c === c);
                return (
                  <div key={`${r}_${c}`}
                    onMouseEnter={() => setHoveredCell ({ r, c })}
                    onMouseLeave={() => setHoveredCell (h => (h && h.r === r && h.c === c) ? null : h)}
                    onClick={() => handleToggleCell (r, c)}
                    style={{
                      gridColumn: c + 2, gridRow: r + 1,
                      width: 22, height: 22, borderRadius: 2, cursor: 'pointer',
                      background: isOn
                        ? ACCENT
                        : isIntersection
                          ? 'color-mix(in srgb, var(--midi) 25%, var(--surface2))'
                          : 'var(--surface2)',
                      boxShadow: isIntersection
                        ? 'inset 0 0 0 1.5px color-mix(in srgb, var(--midi) 45%, var(--border-hi))'
                        : isInBand
                          ? 'inset 0 0 0 1px var(--border-hi)'
                          : 'none',
                    }} />
                );
              })
            )}

            {/* Column labels — rotated 180° so the number reads above "Ch." */}
            {Array.from ({ length: gridSize }, (_, c) => {
              const isFlashing     = (flashRef.current.out.get (c) ?? 0) > Date.now();
              const isHoverAligned = !! hoveredCell && hoveredCell.c === c;
              return (
                <div key={`cl${c}`} style={{
                  gridColumn: c + 2, gridRow: gridSize + 1,
                  fontSize: 8, textAlign: 'center', userSelect: 'none',
                  writingMode: 'vertical-rl', transform: 'rotate(180deg)', whiteSpace: 'nowrap',
                  marginLeft: 6,
                  color: isFlashing ? ACCENT : isHoverAligned ? 'color-mix(in srgb, var(--midi) 75%, var(--text-muted))' : 'var(--text-muted)',
                  fontWeight: isFlashing ? 700 : 400,
                }}>Ch. {c + 1}</div>
              );
            })}
          </div>
        </div>
      )}
    </div>
  );
}
