import { memo, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, DmxSnapshotEntry } from './Bridge';
import { useNodeSettings, useNodeDelete, useNodeCollapsed,
         NodeHeader, NodeHeaderButton, NodeHandle, SettingsPanelHeader } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import { HintContext } from './HintPanel';

// ── Types ─────────────────────────────────────────────────────────────────────
export interface DmxMonitorNodeData {
  label:    string;
  nodeType: 16 | 17;
  ports:    { id: string; label: string; type: string; direction: 'input' | 'output' }[];
  settingsJson?: string;
  [key: string]: unknown;
}

interface DmxNodeSettings {
  visibleCount: 8 | 16 | 24 | 32;
  startChannel: number;
  valueFormat:  'dec' | 'pct' | 'hex';
}

const DEFAULT_SETTINGS: DmxNodeSettings = {
  visibleCount: 8,
  startChannel: 0,
  valueFormat:  'dec',
};

const ACCENT   = 'var(--dmx)';
const ACCENT_C = '#fbbf24';   // resolved colour for canvas drawing
const CHANNELS = 512;
const COL_W    = 24;
const FADER_H  = 80;

function formatVal(v: number, fmt: DmxNodeSettings['valueFormat']): string {
  if (fmt === 'pct') return `${Math.round(v / 255 * 100)}`;
  if (fmt === 'hex') return v.toString(16).toUpperCase().padStart(2, '0');
  return String(v);
}

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
      // Read directly from global — set by Bridge.onDmxSnapshot, no React overhead
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

// ── Vertical fader (Console — interactive DOM element) ────────────────────────
function DmxFader ({ ch, value, format, onChange, readOnly }: {
  ch:       number;
  value:    number;
  format:   DmxNodeSettings['valueFormat'];
  onChange: (ch: number, val: number) => void;
  readOnly: boolean;
}) {
  const [editing, setEditing] = useState(false);
  const [editVal, setEditVal] = useState('');
  const inputRef = useRef<HTMLInputElement>(null);

  const startEdit = useCallback(() => {
    if (readOnly) return;
    setEditVal(String(value));
    setEditing(true);
    setTimeout(() => inputRef.current?.select(), 10);
  }, [readOnly, value]);

  const commitEdit = useCallback(() => {
    let v: number;
    if (format === 'hex') v = parseInt(editVal, 16);
    else if (format === 'pct') v = Math.round(parseInt(editVal, 10) / 100 * 255);
    else v = parseInt(editVal, 10);
    if (!isNaN(v)) onChange(ch - 1, Math.max(0, Math.min(255, v)));
    setEditing(false);
  }, [ch, editVal, format, onChange]);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center',
                  gap: 2, width: COL_W }}>
      <div style={{ fontSize: 7, color: 'var(--text-muted)', textAlign: 'center',
                    fontFamily: "'JetBrains Mono', monospace", lineHeight: 1 }}>
        {ch}
      </div>
      <input
        type="range" min={0} max={255} value={value}
        disabled={readOnly}
        onChange={e => onChange(ch - 1, Number(e.target.value))}
        style={{
          writingMode: 'vertical-lr' as const,
          direction: 'rtl' as const,
          height: FADER_H, width: COL_W,
          cursor: readOnly ? 'default' : 'pointer',
          accentColor: ACCENT,
          opacity: readOnly ? 0.4 : 1,
        }}
        className="nodrag"
        onMouseDown={e => e.stopPropagation()}
        onPointerDown={e => e.stopPropagation()}
      />
      {editing ? (
        <input
          ref={inputRef}
          value={editVal}
          onChange={e => setEditVal(e.target.value)}
          onBlur={commitEdit}
          onKeyDown={e => { if (e.key === 'Enter') commitEdit(); if (e.key === 'Escape') setEditing(false); }}
          className="nodrag"
          style={{
            width: COL_W, fontSize: 7, padding: '1px 1px',
            background: 'var(--surface2)', border: `1px solid ${ACCENT}`,
            borderRadius: 2, color: ACCENT, textAlign: 'center',
            fontFamily: "'JetBrains Mono', monospace",
          }}
        />
      ) : (
        <div
          onClick={startEdit}
          className="nodrag"
          style={{
            fontSize: 7, color: value === 0 ? 'var(--text-muted)' : 'var(--text-dim)',
            textAlign: 'center', fontFamily: "'JetBrains Mono', monospace",
            lineHeight: 1, minWidth: COL_W,
            cursor: readOnly ? 'default' : 'text', userSelect: 'none',
          }}
        >{formatVal(value, format)}</div>
      )}
    </div>
  );
}

// ── Shared settings panel ─────────────────────────────────────────────────────
function DmxSettingsPanel ({ settings, isConsole, onChange, onClose }: {
  settings:  DmxNodeSettings;
  isConsole: boolean;
  onChange:  (s: Partial<DmxNodeSettings>) => void;
  onClose:   () => void;
}) {
  const accent = ACCENT;

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
        width: 220, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title={isConsole ? 'DMX CONSOLE' : 'DMX MONITOR'}
        onReset={() => onChange({ ...DEFAULT_SETTINGS })}
        onClose={onClose}
      />
      {row('Channels', (
        <NodeSelect value={String(settings.visibleCount)} showEmpty={false} accent={accent}
          onChange={v => onChange({ visibleCount: Number(v) as 8|16|24|32, startChannel: 0 })}
          options={[
            { id: '8',  name: '8 channels'  },
            { id: '16', name: '16 channels' },
            { id: '24', name: '24 channels' },
            { id: '32', name: '32 channels' },
          ]}
          onOptionHover={() => {}} />
      ))}
      {row('Start at', (
        <NodeSelect value={String(settings.startChannel)} showEmpty={false} accent={accent}
          onChange={v => onChange({ startChannel: Number(v) })}
          options={startOptions}
          onOptionHover={() => {}} />
      ))}
      {row('Format', (
        <NodeSelect value={settings.valueFormat} showEmpty={false} accent={accent}
          onChange={v => onChange({ valueFormat: v as DmxNodeSettings['valueFormat'] })}
          options={[
            { id: 'dec', name: '0–255 (decimal)' },
            { id: 'pct', name: '0–100 (percent)' },
            { id: 'hex', name: '00–FF (hex)'     },
          ]}
          onOptionHover={() => {}} />
      ))}
    </div>
  );
}

// ── Nav button style ──────────────────────────────────────────────────────────
function navBtnStyle(disabled: boolean): React.CSSProperties {
  return {
    background: 'transparent', border: '1px solid var(--border)',
    color: disabled ? 'var(--text-muted)' : ACCENT,
    borderRadius: 3, padding: '1px 5px', fontSize: 8,
    cursor: disabled ? 'not-allowed' : 'pointer',
    opacity: disabled ? 0.4 : 1,
    fontFamily: "'JetBrains Mono', monospace",
  };
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

  // Canvas RAF reads window.__dmxSnapshots[id] directly — no state needed here

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
      <NodeHeader title="DMX MONITOR" accent={ACCENT}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
      />
      {showSettings && (
        <DmxSettingsPanel settings={settings} isConsole={false}
          onChange={patchSettings} onClose={closeSettings} />
      )}
      {!collapsed && (
        <div ref={portBodyRef} className="nodrag"
          onMouseDown={e => e.stopPropagation()}
          onPointerDown={e => e.stopPropagation()}
          style={{ padding: '6px 8px' }}>
          {/* Page nav */}
          <div style={{ display: 'flex', alignItems: 'center', marginBottom: 4, gap: 4 }}>
            <button className="nodrag"
              onClick={() => patchSettings({ startChannel: Math.max(0, startChannel - visibleCount) })}
              disabled={startChannel === 0}
              style={navBtnStyle(startChannel === 0)}>◀</button>
            <div style={{ flex: 1, textAlign: 'center', fontSize: 8, color: 'var(--text-muted)' }}>
              Ch {startChannel + 1}–{Math.min(startChannel + visibleCount, CHANNELS)}
            </div>
            <button className="nodrag"
              onClick={() => patchSettings({ startChannel: Math.min(CHANNELS - visibleCount, startChannel + visibleCount) })}
              disabled={startChannel + visibleCount >= CHANNELS}
              style={navBtnStyle(startChannel + visibleCount >= CHANNELS)}>▶</button>
          </div>
          {/* Canvas bargraph — 60fps, reads window.__dmxSnapshots directly */}
          <DmxCanvasBargraph
            nodeId={id}
            settingsRef={settingsRef}
            collapsed={collapsed}
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

// ── DMX Console (nodeType 17) ─────────────────────────────────────────────────
export const DmxConsoleNode = memo(function DmxConsoleNode ({ id, data, selected }: NodeProps) {
  const nodeData = data as DmxMonitorNodeData;
  const { setHint }                              = useContext(HintContext);
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete }                         = useNodeDelete(id);
  const { collapsed, toggleCollapsed }           = useNodeCollapsed(id);
  const [channels, setChannels]                  = useState<number[]>(new Array(512).fill(0));
  const [blackout, setBlackoutState]             = useState(false);
  const channelsRef                              = useRef<number[]>(new Array(512).fill(0));
  const portBodyRef                              = useRef<HTMLDivElement>(null);

  const [settings, setSettings] = useState<DmxNodeSettings>(() => ({
    ...DEFAULT_SETTINGS,
    ...(nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) as Partial<DmxNodeSettings> : {}),
  }));
  const settingsRef = useRef(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  const patchSettings = useCallback((patch: Partial<DmxNodeSettings>) => {
    setSettings(prev => ({ ...prev, ...patch }));
  }, []);

  useEffect(() => {
    const unsub = Bridge.onDmxSnapshot((snaps: DmxSnapshotEntry[]) => {
      const snap = snaps.find(s => s.id === id);
      if (!snap) return;
      const { startChannel, visibleCount } = settingsRef.current;
      let changed = false;
      for (let i = startChannel; i < startChannel + visibleCount && i < 512; i++) {
        if (channelsRef.current[i] !== snap.ch[i]) { changed = true; break; }
      }
      channelsRef.current = snap.ch;
      if (changed) setChannels([...snap.ch]);
    });
    return unsub;
  }, [id]);

  const handleChange = useCallback((ch: number, val: number) => {
    setChannels(prev => { const next = [...prev]; next[ch] = val; return next; });
    Bridge.setDmxConsoleChannel(id, ch, val);
  }, [id]);

  const handleBlackout = useCallback(() => {
    const next = !blackout;
    setBlackoutState(next);
    Bridge.setDmxBlackout(id, next);
  }, [id, blackout]);

  const { visibleCount, startChannel, valueFormat } = settings;
  const visibleChannels = channels.slice(startChannel, startChannel + visibleCount);
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
      <NodeHeader title="DMX CONSOLE" accent={ACCENT}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}>
        <NodeHeaderButton
          onClick={handleBlackout}
          active={blackout}
          activeAccent="#ef5350"
          onHint={{
            onMouseEnter: () => setHint({ title: 'Blackout', body: 'Set all DMX channels to 0. Click again to restore.' }),
            onMouseLeave: () => setHint(null),
          }}>
          <span style={{ fontSize: 8, fontWeight: 700, letterSpacing: '0.05em' }}>BO</span>
        </NodeHeaderButton>
      </NodeHeader>
      {showSettings && (
        <DmxSettingsPanel settings={settings} isConsole={true}
          onChange={patchSettings} onClose={closeSettings} />
      )}
      {!collapsed && (
        <div ref={portBodyRef} className="nodrag"
          onMouseDown={e => e.stopPropagation()}
          onPointerDown={e => e.stopPropagation()}
          style={{ padding: '6px 8px' }}>
          <div style={{ display: 'flex', alignItems: 'center', marginBottom: 4, gap: 4 }}>
            <button className="nodrag"
              onClick={() => patchSettings({ startChannel: Math.max(0, startChannel - visibleCount) })}
              disabled={startChannel === 0}
              style={navBtnStyle(startChannel === 0)}>◀</button>
            <div style={{ flex: 1, textAlign: 'center', fontSize: 8, color: 'var(--text-muted)' }}>
              Ch {startChannel + 1}–{Math.min(startChannel + visibleCount, CHANNELS)}
              {blackout && <span style={{ color: '#ef5350', marginLeft: 4 }}>● BO</span>}
            </div>
            <button className="nodrag"
              onClick={() => patchSettings({ startChannel: Math.min(CHANNELS - visibleCount, startChannel + visibleCount) })}
              disabled={startChannel + visibleCount >= CHANNELS}
              style={navBtnStyle(startChannel + visibleCount >= CHANNELS)}>▶</button>
          </div>
          <div style={{ display: 'flex', gap: 0 }}>
            {visibleChannels.map((val, i) => (
              <DmxFader
                key={startChannel + i}
                ch={startChannel + i + 1}
                value={blackout ? 0 : val}
                format={valueFormat}
                onChange={handleChange}
                readOnly={blackout}
              />
            ))}
          </div>
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
