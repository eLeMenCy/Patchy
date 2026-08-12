import { memo, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, ArtNetSnapshotEntry } from './Bridge';
import { useNodeSettings, useNodeDelete, useNodeCollapsed,
         NodeHeader, NodeHandle, SettingsPanelHeader } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import {
  DmxMonitorNodeData, DmxNodeSettings, DEFAULT_SETTINGS,
  CHANNELS, COL_W, FADER_H,
  formatVal, navBtnStyle, NameInput,
} from './DmxShared';

// ArtNet uses its own colour
const ACCENT   = 'var(--artnet)';
const ACCENT_C = '#fde68a';  // resolved for canvas

export type { DmxMonitorNodeData };

// ArtNetMonitorNode.tsx — ArtNet's counterpart to DmxMonitorNode.tsx, and
// shares its core rendering approach: same canvas-over-DOM-faders reasoning,
// same RAF self-scheduling loop, same window-global-snapshot pattern (here
// __artNetSnapshots instead of __dmxSnapshots), same 0.4-floor alpha
// convention — see that file for the full reasoning, not repeated here.
// What's genuinely new: ArtNet actually has multiple addressable universes,
// so this node can filter to just one rather than always showing whichever
// universe happens to arrive — see noDataRef and the filter logic below.

// ── ArtNet-specific settings (extends DMX settings with universe filter) ──────
interface ArtNetMonitorSettings extends DmxNodeSettings {
  universe:           number;
  filterUniverse:     boolean;
  filterUniverseValue: number;
}

const DEFAULT_ARTNET_SETTINGS: ArtNetMonitorSettings = {
  ...DEFAULT_SETTINGS,
  universe:            0,
  filterUniverse:      false,
  filterUniverseValue: 0,
};

// ── Canvas bargraph (mirrors DmxMonitorNode but reads __artNetSnapshots) ──────
function ArtNetCanvasBargraph ({ nodeId, settingsRef, collapsed, noDataRef }: {
  nodeId:      string;
  settingsRef: React.MutableRefObject<ArtNetMonitorSettings>;
  collapsed:   boolean;
  noDataRef:   React.MutableRefObject<boolean>;
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

      const { startChannel, visibleCount, valueFormat,
              filterUniverse, filterUniverseValue } = settingsRef.current;
      const snap: Uint8Array | undefined = (window as any).__artNetSnapshots?.[nodeId];
      const noData = noDataRef.current;

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
        const val = (snap && !noData) ? snap[ch] : 0;
        const pct = val / 255;
        const x   = i * colW;

        ctx.fillStyle = '#6b7280';
        ctx.font = '7px monospace';
        ctx.textAlign = 'center';
        ctx.fillText(String(ch + 1), x + colW / 2, labelH - 1);

        const trackY = labelH;
        ctx.fillStyle = '#1f2937';
        ctx.fillRect(x + barX, trackY, barW, trackH);

        if (val > 0 && !noData) {
          const fillH2 = Math.round(pct * trackH);
          const alpha  = 0.4 + pct * 0.6;
          ctx.fillStyle = val === 255 ? ACCENT_C : `rgba(253,230,138,${alpha.toFixed(2)})`;
          ctx.fillRect(x + barX, trackY + trackH - fillH2, barW, fillH2);
        }

        // Show "--" when filter is active but no matching universe arrived
        const displayVal = noData && filterUniverse ? '--' : formatVal(val, valueFormat);
        ctx.fillStyle = (noData && filterUniverse) ? '#6b7280' : (val === 0 ? '#4b5563' : '#9ca3af');
        ctx.font = '7px monospace';
        ctx.textAlign = 'center';
        ctx.fillText(displayVal, x + colW / 2, labelH + trackH + valH - 1);
      }

      rafRef.current = requestAnimationFrame(render);
    };

    rafRef.current = requestAnimationFrame(render);
    return () => cancelAnimationFrame(rafRef.current);
  }, [collapsed, nodeId, settingsRef, noDataRef]);

  const { visibleCount } = settingsRef.current;
  const canvasW = visibleCount * COL_W;
  const canvasH = 12 + FADER_H + 12;

  return (
    <canvas ref={canvasRef} width={canvasW} height={canvasH} style={{ display: 'block' }} />
  );
}

// ── ArtNet Monitor (nodeType 18) ──────────────────────────────────────────────
export const ArtNetMonitorNode = memo(function ArtNetMonitorNode ({ id, data, selected }: NodeProps) {
  const nodeData = data as DmxMonitorNodeData;
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete }               = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const portBodyRef                    = useRef<HTMLDivElement>(null);
  // A ref rather than state for the same reason channels tracking uses one
  // elsewhere in this file family — read synchronously inside the RAF
  // render loop, no re-render needed on every incoming snapshot. "No data"
  // is its own meaningful state, distinct from "received zeros": with a
  // filter active, nothing at all may have arrived yet for that specific
  // universe, and the canvas shows "--" for that (unknown) rather than "0"
  // (a real, known reading), which noDataRef is what makes possible.
  const noDataRef                      = useRef<boolean>(false);

  const [settings, setSettings] = useState<ArtNetMonitorSettings>(() => ({
    ...DEFAULT_ARTNET_SETTINGS,
    ...(nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) as Partial<ArtNetMonitorSettings> : {}),
  }));
  const settingsRef = useRef(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  const patchSettings = useCallback((patch: Partial<ArtNetMonitorSettings>) => {
    setSettings(prev => ({ ...prev, ...patch }));
  }, []);

  const commitPatch = useCallback((patch: Partial<ArtNetMonitorSettings>) => {
    setSettings(prev => {
      const next = { ...prev, ...patch };
      const existing = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : {};
      Bridge.commitSettingsChange(id, { ...existing, ...next });
      if ('customName' in patch) Bridge.setNodeLabel(id, patch.customName ?? '');
      // Update C++ universe filter
      if ('filterUniverse' in patch || 'filterUniverseValue' in patch) {
        // -1 is the backend's own sentinel for "show all universes" — see
        // handleSetNodeParam_ArtNetUniverseFilter in WebBridge_Dispatch.cpp.
        const filter = next.filterUniverse ? next.filterUniverseValue : -1;
        Bridge.setNodeParam(id, 'artNetUniverseFilter', String(filter));
      }
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
        ...(parsed.visibleCount        !== undefined && { visibleCount:        parsed.visibleCount        }),
        ...(parsed.startChannel        !== undefined && { startChannel:        parsed.startChannel        }),
        ...(parsed.valueFormat         !== undefined && { valueFormat:         parsed.valueFormat         }),
        ...(parsed.customName          !== undefined && { customName:          parsed.customName          }),
        ...(parsed.universe            !== undefined && { universe:            parsed.universe            }),
        ...(parsed.filterUniverse      !== undefined && { filterUniverse:      parsed.filterUniverse      }),
        ...(parsed.filterUniverseValue !== undefined && { filterUniverseValue: parsed.filterUniverseValue }),
      }));
    } catch {}
  }, [nodeData.settingsJson]);

  // Subscribe to ArtNet snapshots — update canvas data and noDataRef
  useEffect(() => {
    const unsub = Bridge.onArtNetSnapshot((snaps: ArtNetSnapshotEntry[]) => {
      const snap = snaps.find(s => s.id === id);
      if (!snap) return;
      const { filterUniverse, filterUniverseValue } = settingsRef.current;
      if (filterUniverse && snap.universe !== filterUniverseValue) {
        // Wrong universe — mark as no-data (show "--")
        noDataRef.current = true;
        return;
      }
      noDataRef.current = false;
      // Canvas reads from __artNetSnapshots directly — no React state needed
    });
    return unsub;
  }, [id]);

  const { visibleCount, startChannel, universe, filterUniverse, filterUniverseValue } = settings;
  const nodeW = visibleCount * COL_W + 16;

  const inputs  = nodeData.ports.filter(p => p.direction === 'input');
  const outputs = nodeData.ports.filter(p => p.direction === 'output');

  const headerTitle = settings.customName || 'ARTNET MONITOR';
  const universeLabel = filterUniverse ? `Uni ${filterUniverseValue}` : 'All Uni';

  return (
    <div style={{
      background: 'var(--surface)',
      border: `1px solid ${selected ? ACCENT : 'var(--border)'}`,
      borderTop: `3px solid ${ACCENT}`,
      borderRadius: 'var(--radius)',
      boxShadow: selected ? `0 0 0 1px ${ACCENT}, 0 8px 32px var(--artnet-glow)` : '0 4px 16px rgba(0,0,0,.5)',
      minWidth: nodeW, fontFamily: "'JetBrains Mono', monospace", position: 'relative',
    }}>
      <NodeHeader title={headerTitle} accent={ACCENT}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
      />
      {showSettings && (
        <ArtNetMonitorSettingsPanel
          settings={settings}
          onChange={s => patchSettings(s)}
          onCommit={s => commitPatch(s)}
          onClose={closeSettings}
        />
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
              <span style={{ marginLeft: 6, color: ACCENT, opacity: 0.8 }}>
                [{universeLabel}]
              </span>
            </div>
            <button className="nodrag"
              onClick={() => commitPatch({ startChannel: Math.min(CHANNELS - visibleCount, startChannel + visibleCount) })}
              disabled={startChannel + visibleCount >= CHANNELS}
              style={navBtnStyle(startChannel + visibleCount >= CHANNELS)}>▶</button>
          </div>
          <ArtNetCanvasBargraph
            nodeId={id}
            settingsRef={settingsRef}
            collapsed={collapsed}
            noDataRef={noDataRef}
          />
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

// ── ArtNet Monitor Settings Panel ─────────────────────────────────────────────
function ArtNetMonitorSettingsPanel ({ settings, onChange, onCommit, onClose }: {
  settings: ArtNetMonitorSettings;
  onChange: (s: Partial<ArtNetMonitorSettings>) => void;
  onCommit: (s: Partial<ArtNetMonitorSettings>) => void;
  onClose:  () => void;
}) {
  const row = (label: string, child: React.ReactNode) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
      <div style={{ width: 80, fontSize: 10, color: 'var(--text-dim)', flexShrink: 0 }}>{label}</div>
      <div style={{ flex: 1, minWidth: 0 }}>{child}</div>
    </div>
  );

  const startOptions = [];
  for (let i = 0; i < CHANNELS; i += settings.visibleCount)
    startOptions.push({ id: String(i), name: `Ch ${i + 1}–${Math.min(i + settings.visibleCount, CHANNELS)}` });

  return (
    <div
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onPointerUp={e => e.stopPropagation()}
      onClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6,
        width: 230, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader title="ARTNET MONITOR" onReset={() => onCommit({ ...DEFAULT_ARTNET_SETTINGS })} onClose={onClose} />
      {row('Name', (
        <NameInput
          value={settings.customName}
          placeholder="ArtNet Monitor"
          onChange={v => onChange({ customName: v })}
          onCommit={v => onCommit({ customName: v })}
        />
      ))}
      {row('Channels', (
        <NodeSelect value={String(settings.visibleCount)} showEmpty={false} accent={ACCENT}
          onChange={(v: string) => onCommit({ visibleCount: Number(v) as 8|16|24|32, startChannel: 0 })}
          options={[
            { id: '8',  name: '8 channels'  },
            { id: '16', name: '16 channels' },
            { id: '24', name: '24 channels' },
            { id: '32', name: '32 channels' },
          ]}
          onOptionHover={() => {}} />
      ))}
      {row('Start at', (
        <NodeSelect value={String(settings.startChannel)} showEmpty={false} accent={ACCENT}
          onChange={(v: string) => onCommit({ startChannel: Number(v) })}
          options={startOptions}
          onOptionHover={() => {}} />
      ))}
      {row('Format', (
        <NodeSelect value={settings.valueFormat} showEmpty={false} accent={ACCENT}
          onChange={(v: string) => onCommit({ valueFormat: v as DmxNodeSettings['valueFormat'] })}
          options={[
            { id: 'dec', name: '0–255 (decimal)' },
            { id: 'pct', name: '0–100 (percent)' },
            { id: 'hex', name: '00–FF (hex)'     },
          ]}
          onOptionHover={() => {}} />
      ))}
      {row('Filter Uni', (
        <div style={{ display: 'flex', alignItems: 'center', gap: 6 }}>
          <input type="checkbox" checked={settings.filterUniverse}
            onChange={e => onCommit({ filterUniverse: e.target.checked })}
            style={{ accentColor: ACCENT }} />
          {settings.filterUniverse && (
            <input type="number" min={0} max={32767}
              value={settings.filterUniverseValue}
              onChange={e => onChange({ filterUniverseValue: Number(e.target.value) })}
              onBlur={e => onCommit({ filterUniverseValue: Number(e.target.value) })}
              onKeyDown={e => { if (e.key === 'Enter') onCommit({ filterUniverseValue: Number((e.target as HTMLInputElement).value) }); }}
              style={{
                width: 60, background: 'var(--surface)', border: `1px solid ${ACCENT}`,
                color: 'var(--text)', fontSize: 10, borderRadius: 3,
                padding: '2px 4px', fontFamily: "'JetBrains Mono', monospace",
                outline: 'none',
              }}
            />
          )}
        </div>
      ))}
    </div>
  );
}
