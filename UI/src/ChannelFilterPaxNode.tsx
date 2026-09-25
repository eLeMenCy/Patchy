// Patchy — Channel Filter Pax node UI
//
// Bespoke component for ChannelFilterPax.cpp — a study example, deliberately
// modeled on SpectrumyserNode.tsx's own header/settings-panel structure (the
// simplest existing bespoke Pax component to learn from), with MIDI port
// rendering reused from MidiChMatrixNode.tsx's own already-proven convention.
//
// The two button rows below both dispatch via the same onParamChange +
// Bridge.commitNodeSettings(id) pattern SpectrumyserNode.tsx's own "band
// count" row already established — a discrete click should commit
// immediately, not wait for a drag to end the way the generic slider does.

import { useState, useEffect, useCallback, useContext } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { NodeHandle, useNodeCollapsed, useNodeDisabled, NodeHeaderButton, NodeCollapseArrow, settingsPanelStyle, EditableTitle, commitModelName } from './NodeUtils';
import { HintContext } from './HintPanel';
import { Settings, X, Power } from 'lucide-react';

// Persist settings panel open state across graph updates (survives undo/redo)
const _settingsOpen = new Map<string, boolean>();

const ACCENT = 'var(--midi)';

export default function ChannelFilterPaxNode ({ id, data, selected }: NodeProps) {
  const { setHint } = useContext (HintContext);
  const { collapsed, toggleCollapsed } = useNodeCollapsed (id, (data as any)._forceCollapsed);
  const { disabled, toggleDisabled }   = useNodeDisabled (id, (data as any).disabled);
  const [showSettings, setShowSettings] = useState (() => _settingsOpen.get (id) ?? false);

  // paramValues[0] = channel (1-16), paramValues[1] = mode (0=Filter, 1=Force)
  // — same flat-array shape as SpectrumyserNode.tsx's own paramValues,
  // matching Bridge.setNodeSettings()'s own expected payload.
  const [paramValues, setParamValues] = useState<number[]>([1, 0]);

  // Restore state from settingsJson on undo/redo — same pattern as
  // SpectrumyserNode.tsx's own restoration effect.
  useEffect (() => {
    const sj = (data as any)?.settingsJson;
    if (! sj) return;
    try {
      const vals = JSON.parse (sj) as number[];
      if (vals.length >= 2) {
        setParamValues (vals);
        Bridge.setPaxParameter (id, 0, vals[0]);
        Bridge.setPaxParameter (id, 1, vals[1]);
      }
    } catch {}
  }, [(data as any)?.settingsJson]);

  const channel = paramValues[0] ?? 1;
  const mode    = paramValues[1] ?? 0;   // 0 = Filter, 1 = Force

  const handleParam = useCallback ((idx: number, val: number) => {
    setParamValues (prev => {
      const next = [...prev]; next[idx] = val;
      Bridge.setNodeSettings (id, next);
      return next;
    });
    Bridge.setPaxParameter (id, idx, val);
    // Both parameters here are discrete clicks (a channel button, a mode
    // toggle), never a drag — commit immediately, matching
    // SpectrumyserNode.tsx's own "band count is a discrete action" convention.
    Bridge.commitNodeSettings (id);
  }, [id]);

  // Phase 6 persistent rename, 2026-09-25 — was a standalone useState('')
  // (lost on every reload); now read from the graph model's own customName.
  const customName: string = (data as any)?.customName ?? '';

  const resetParams = useCallback (() => {
    setParamValues ([1, 0]);
    Bridge.setNodeSettings (id, [1, 0]);
    Bridge.setPaxParameter (id, 0, 1);
    Bridge.setPaxParameter (id, 1, 0);
    Bridge.commitNodeSettings (id);
  }, [id]);

  return (
    <div style={{
      display: 'inline-block', width: 'fit-content',
      background: 'var(--surface)',
      border: `1px solid ${selected ? ACCENT : 'var(--border)'}`,
      borderTop: `3px solid ${ACCENT}`,
      borderRadius: 'var(--radius)',
      fontFamily: "'JetBrains Mono', monospace",
      filter: disabled ? 'grayscale(0.8) opacity(0.55)' : 'none',
      transition: 'filter .15s',
      position: 'relative',
    }}>
      <NodeHandle nodeId={id} label="MIDI In"  direction="in"  colour={ACCENT} index={0} total={1} offset={0} />
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour={ACCENT} index={0} total={1} offset={0} />

      {/* Header — same structure as SpectrumyserNode.tsx's own */}
      <div style={{
        display: 'flex', alignItems: 'center', padding: '4px 8px', gap: 4,
        background: 'transparent',
        borderBottom: collapsed ? 'none' : '1px solid color-mix(in srgb, var(--midi) 27%, transparent)',
        cursor: 'pointer',
      }} onDoubleClick={toggleCollapsed}>

        <NodeCollapseArrow collapsed={collapsed} accent={ACCENT} />

        <div style={{ flex: 1, minWidth: 0 }}>
          {/* Phase 6 in-place rename, 2026-09-25 — pencil on hover */}
          <EditableTitle display={customName || 'Channel Filter'} value={customName} placeholder={'Channel Filter'}
            onCommit={commitModelName (id)}
            textStyle={{ fontSize: '11px', fontWeight: 700, color: ACCENT,
          letterSpacing: '0.1em', fontFamily: "'Syne', sans-serif",
          whiteSpace: 'nowrap', textTransform: 'uppercase', userSelect: 'none' }} />
        </div>


        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={toggleDisabled} active={! disabled} activeAccent={ACCENT}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Disable / Enable Node', body: 'Disables or re-enables this node. A disabled node passes every channel through unchanged.' }), onMouseLeave: () => setHint (null) }}>
            <Power size={11} />
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton
            onClick={() => setShowSettings (v => { const next = ! v; _settingsOpen.set (id, next); return next; })}
            active={showSettings} activeAccent={ACCENT}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Channel Settings', body: 'Choose which channel to keep, and Filter/Force mode.' }), onMouseLeave: () => setHint (null) }}>
            <Settings size={11} />
          </NodeHeaderButton>
        </div>

        <div onDoubleClick={e => e.stopPropagation()}>
          <NodeHeaderButton onClick={() => Bridge.removeNode (id)}
            onHint={{ onMouseEnter: () => setHint ({ title: 'Delete node', body: 'Remove this node.' }), onMouseLeave: () => setHint (null) }}>
            <X size={12} color="var(--text-muted)" />
          </NodeHeaderButton>
        </div>
      </div>

      {! collapsed && (
        <div style={{ padding: '10px 12px', display: 'flex', justifyContent: 'center' }}>
          <div style={{ fontSize: 9, color: 'var(--text-muted)' }}>
            Ch. {channel} — {mode === 0 ? 'Filter' : 'Force'}
          </div>
        </div>
      )}

      {/* Settings panel */}
      {showSettings && ! collapsed && (
        <div style={settingsPanelStyle}>
          {/* Channel — 16 buttons, same style as SpectrumyserNode.tsx's own
              5-button "band count" row, just scaled up */}
          <div className="nodrag" style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 8 }}>
            <span style={{ color: 'var(--text-muted)', minWidth: 40 }}>Channel</span>
          </div>
          <div className="nodrag" style={{ display: 'flex', flexWrap: 'wrap', gap: 3, marginBottom: 10 }}>
            {Array.from ({ length: 16 }, (_, i) => i + 1).map (ch => (
              <div key={ch} onClick={() => handleParam (0, ch)} style={{
                width: 18, height: 18, display: 'flex', alignItems: 'center', justifyContent: 'center',
                cursor: 'pointer', borderRadius: 2, fontSize: 9,
                background: ch === channel ? ACCENT : 'var(--surface)',
                color: ch === channel ? '#000' : 'var(--text-muted)',
                fontWeight: ch === channel ? 700 : 400,
              }}>{ch}</div>
            ))}
          </div>

          {/* Mode — 2 buttons, same shape as the channel row above, just
              with words instead of numbers */}
          <div className="nodrag" style={{ display: 'flex', alignItems: 'center', gap: 4, marginBottom: 4 }}>
            <span style={{ color: 'var(--text-muted)', minWidth: 40 }}>Mode</span>
          </div>
          <div className="nodrag" style={{ display: 'flex', gap: 3, marginBottom: 8 }}>
            {(['Filter', 'Force'] as const).map ((label, m) => (
              <div key={label} onClick={() => handleParam (1, m)} style={{
                padding: '2px 8px', display: 'flex', alignItems: 'center', justifyContent: 'center',
                cursor: 'pointer', borderRadius: 2, fontSize: 9,
                background: m === mode ? ACCENT : 'var(--surface)',
                color: m === mode ? '#000' : 'var(--text-muted)',
                fontWeight: m === mode ? 700 : 400,
              }}>{label}</div>
            ))}
          </div>

          <div onDoubleClick={e => e.stopPropagation()}>
            <NodeHeaderButton onClick={resetParams}
              onHint={{ onMouseEnter: () => setHint ({ title: 'Reset', body: 'Reset to Channel 1, Filter mode.' }), onMouseLeave: () => setHint (null) }}>
              <span style={{ fontSize: 11, fontWeight: 700 }}>R</span>
            </NodeHeaderButton>
          </div>
        </div>
      )}
    </div>
  );
}
