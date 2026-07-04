import { memo, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { useNodeSettings, useNodeDelete, useNodeCollapsed,
         NodeHeader, NodeHandle } from './NodeUtils';
import {
  DmxMonitorNodeData, DmxNodeSettings, DEFAULT_SETTINGS,
  ACCENT, ACCENT_C, CHANNELS, COL_W, FADER_H,
  formatVal, navBtnStyle, DmxSettingsPanel,
} from './DmxShared';

export type { DmxMonitorNodeData };

// ── Canvas bargraph panel (Monitor — 60fps RAF, reads window.__dmxSnapshots) ──
function DmxCanvasBargraph ({ nodeId, settingsRef, collapsed }: {
  nodeId:      string;
  settingsRef: React.MutableRefObject<DmxNodeSettings>;
  collapsed:   boolean;
}) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const rafRef    = useRef<number>(0);

  useEffect(() => {
    if (collapsed) return;

    const render = () => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;

      const { startChannel, visibleCount, valueFormat } = settingsRef.current;
      const snap: Uint8Array | undefined = (window as any).__dmxSnapshots?.[nodeId];

      const w = canvas.width;
      const h = canvas.height;
      ctx.clearRect(0, 0, w, h);

      const colW   = w / visibleCount;
      const barW   = Math.max(4, colW * 0.4);
      const barX   = (colW - barW) / 2;
      const trackH = FADER_H;
      const labelH = 12;
      const valH   = 12;

      for (let i = 0; i < visibleCount; i++) {
        const ch  = startChannel + i;
        const val = snap ? snap[ch] : 0;
        const pct = val / 255;
        const x   = i * colW;

        ctx.fillStyle = '#6b7280';
        ctx.font = '7px monospace';
        ctx.textAlign = 'center';
        ctx.fillText(String(ch + 1), x + colW / 2, labelH - 1);

        const trackY = labelH;
        ctx.fillStyle = '#1f2937';
        ctx.fillRect(x + barX, trackY, barW, trackH);

        if (val > 0) {
          const fillH2 = Math.round(pct * trackH);
          const alpha  = 0.4 + pct * 0.6;
          ctx.fillStyle = val === 255 ? ACCENT_C : `rgba(251,191,36,${alpha.toFixed(2)})`;
          ctx.fillRect(x + barX, trackY + trackH - fillH2, barW, fillH2);
        }

        ctx.fillStyle = val === 0 ? '#4b5563' : '#9ca3af';
        ctx.font = '7px monospace';
        ctx.textAlign = 'center';
        ctx.fillText(formatVal(val, valueFormat), x + colW / 2, labelH + trackH + valH - 1);
      }

      rafRef.current = requestAnimationFrame(render);
    };

    rafRef.current = requestAnimationFrame(render);
    return () => cancelAnimationFrame(rafRef.current);
  }, [collapsed, nodeId, settingsRef]);

  const { visibleCount } = settingsRef.current;
  const canvasW = visibleCount * COL_W;
  const canvasH = 12 + FADER_H + 12;

  return (
    <canvas
      ref={canvasRef}
      width={canvasW}
      height={canvasH}
      style={{ display: 'block' }}
    />
  );
}

// ── DMX Monitor (nodeType 16) — canvas-based, 60fps RAF ──────────────────────
export const DmxMonitorNode = memo(function DmxMonitorNode ({ id, data, selected }: NodeProps) {
  const nodeData = data as DmxMonitorNodeData;
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete }               = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id);
  const portBodyRef                    = useRef<HTMLDivElement>(null);

  const [settings, setSettings] = useState<DmxNodeSettings>(() => ({
    ...DEFAULT_SETTINGS,
    ...(nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) as Partial<DmxNodeSettings> : {}),
  }));
  const settingsRef = useRef(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  const patchSettings = useCallback((patch: Partial<DmxNodeSettings>) => {
    setSettings(prev => ({ ...prev, ...patch }));
  }, []);

  const commitPatch = useCallback((patch: Partial<DmxNodeSettings>) => {
    setSettings(prev => {
      const next = { ...prev, ...patch };
      const existing = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : {};
      Bridge.commitSettingsChange(id, { ...existing, ...next });
      if ('customName' in patch) Bridge.setNodeLabel(id, patch.customName ?? '');
      return next;
    });
  }, [id, nodeData.settingsJson]);

  // Restore UI settings from settingsJson on undo/redo
  useEffect(() => {
    try {
      const raw = nodeData.settingsJson as string | undefined;
      const parsed = raw ? JSON.parse(raw) : null;
      if (!parsed) return;
      setSettings(prev => ({
        ...prev,
        ...(parsed.visibleCount !== undefined && { visibleCount: parsed.visibleCount }),
        ...(parsed.startChannel !== undefined && { startChannel: parsed.startChannel }),
        ...(parsed.valueFormat  !== undefined && { valueFormat:  parsed.valueFormat  }),
        ...(parsed.customName   !== undefined && { customName:   parsed.customName   }),
      }));
    } catch {}
  }, [nodeData.settingsJson]);

  const { visibleCount, startChannel } = settings;
  const nodeW = visibleCount * COL_W + 16;

  const inputs  = nodeData.ports.filter(p => p.direction === 'input');
  const outputs = nodeData.ports.filter(p => p.direction === 'output');

  return (
    <div style={{
      background: 'var(--surface)',
      border: `1px solid ${selected ? ACCENT : 'var(--border)'}`,
      borderTop: `3px solid ${ACCENT}`,
      borderRadius: 'var(--radius)',
      boxShadow: selected ? `0 0 0 1px ${ACCENT}, 0 8px 32px var(--dmx-glow)` : '0 4px 16px rgba(0,0,0,.5)',
      minWidth: nodeW, fontFamily: "'JetBrains Mono', monospace", position: 'relative',
    }}>
      <NodeHeader title={settings.customName || 'DMX MONITOR'} accent={ACCENT}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
      />
      {showSettings && (
        <DmxSettingsPanel settings={settings} isConsole={false}
          onChange={patchSettings} onCommit={commitPatch}
          onClose={closeSettings} />
      )}
      {!collapsed && (
        <div ref={portBodyRef} className="nodrag"
          onMouseDown={e => e.stopPropagation()}
          onPointerDown={e => e.stopPropagation()}
          style={{ padding: '6px 8px' }}>
          <div style={{ display: 'flex', alignItems: 'center', marginBottom: 4, gap: 4 }}>
            <button className="nodrag"
              onClick={() => commitPatch({ startChannel: Math.max(0, startChannel - visibleCount) })}
              disabled={startChannel === 0}
              style={navBtnStyle(startChannel === 0)}>◀</button>
            <div style={{ flex: 1, textAlign: 'center', fontSize: 8, color: 'var(--text-muted)' }}>
              Ch {startChannel + 1}–{Math.min(startChannel + visibleCount, CHANNELS)}
            </div>
            <button className="nodrag"
              onClick={() => commitPatch({ startChannel: Math.min(CHANNELS - visibleCount, startChannel + visibleCount) })}
              disabled={startChannel + visibleCount >= CHANNELS}
              style={navBtnStyle(startChannel + visibleCount >= CHANNELS)}>▶</button>
          </div>
          <DmxCanvasBargraph nodeId={id} settingsRef={settingsRef} collapsed={collapsed} />
        </div>
      )}
      {inputs.map((p, i) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="in"
          colour={ACCENT} index={i} total={inputs.length} offset={8}
          portBodyRef={portBodyRef} portId={p.id} />
      ))}
      {outputs.map((p, i) => (
        <NodeHandle key={p.id} nodeId={id} label={p.label} direction="out"
          colour={ACCENT} index={i} total={outputs.length} offset={8}
          portBodyRef={portBodyRef} portId={p.id} />
      ))}
    </div>
  );
});
