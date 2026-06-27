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
  startChannel: number;     // 0-based, multiple of visibleCount
  valueFormat:  'dec' | 'pct' | 'hex';
}

const DEFAULT_SETTINGS: DmxNodeSettings = {
  visibleCount: 8,
  startChannel: 0,
  valueFormat:  'dec',
};

// ── Constants ─────────────────────────────────────────────────────────────────
const ACCENT      = 'var(--dmx)';
const FADER_H     = 80;    // px — fader/bargraph height, matching WheelSlider feel
const COL_W       = 24;    // px — channel column width
const CHANNELS    = 512;

function formatVal(v: number, fmt: DmxNodeSettings['valueFormat']): string {
  if (fmt === 'pct') return `${Math.round(v / 255 * 100)}`;
  if (fmt === 'hex') return v.toString(16).toUpperCase().padStart(2, '0');
  return String(v);
}

// ── Vertical bargraph (Monitor — read-only) ───────────────────────────────────
const DmxBargraph = memo(function DmxBargraph ({ ch, value, format }: {
  ch:     number;
  value:  number;
  format: DmxNodeSettings['valueFormat'];
}) {
  const pct = value / 255;
  const fillH = Math.round(pct * FADER_H);
  const color = value === 0   ? 'var(--border)'
              : value === 255 ? ACCENT
              : `color-mix(in srgb, ${ACCENT} ${Math.round(pct * 100)}%, var(--border))`;

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center',
                  gap: 2, width: COL_W }}>
      {/* Channel number */}
      <div style={{ fontSize: 7, color: 'var(--text-muted)', textAlign: 'center',
                    fontFamily: "'JetBrains Mono', monospace", lineHeight: 1 }}>
        {ch}
      </div>
      {/* Bargraph track */}
      <div style={{ width: 10, height: FADER_H, background: 'var(--surface)',
                    borderRadius: 3, position: 'relative', overflow: 'hidden' }}>
        <div style={{
          position: 'absolute', bottom: 0, left: 0, right: 0,
          height: fillH, background: color,
          borderRadius: 3,
        }} />
      </div>
      {/* Value */}
      <div style={{ fontSize: 7, color: value === 0 ? 'var(--text-muted)' : 'var(--text-dim)',
                    textAlign: 'center', fontFamily: "'JetBrains Mono', monospace",
                    lineHeight: 1, minWidth: COL_W }}>
        {formatVal(value, format)}
      </div>
    </div>
  );
});

// ── Vertical fader (Console — interactive) ────────────────────────────────────
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
      {/* Channel number */}
      <div style={{ fontSize: 7, color: 'var(--text-muted)', textAlign: 'center',
                    fontFamily: "'JetBrains Mono', monospace", lineHeight: 1 }}>
        {ch}
      </div>

      {/* Vertical range slider — same pattern as WheelSlider */}
      <input
        type="range" min={0} max={255} value={value}
        disabled={readOnly}
        onChange={e => onChange(ch - 1, Number(e.target.value))}
        style={{
          writingMode: 'vertical-lr' as const,
          direction: 'rtl' as const,
          height: FADER_H,
          width: COL_W,
          cursor: readOnly ? 'default' : 'pointer',
          accentColor: ACCENT,
          opacity: readOnly ? 0.4 : 1,
        }}
        className="nodrag"
        onMouseDown={e => e.stopPropagation()}
        onPointerDown={e => e.stopPropagation()}
      />

      {/* Value — click to edit */}
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
  const { setHint } = useContext(HintContext);
  const accent = ACCENT;

  const row = (label: string, child: React.ReactNode) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
      <div style={{ width: 80, fontSize: 10, color: 'var(--text-dim)', flexShrink: 0 }}>{label}</div>
      <div style={{ flex: 1, minWidth: 0 }}>{child}</div>
    </div>
  );

  // Channel start options — multiples of visibleCount up to 512
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
        <NodeSelect
          value={String(settings.visibleCount)} showEmpty={false} accent={accent}
          onChange={v => onChange({ visibleCount: Number(v) as 8|16|24|32, startChannel: 0 })}
          options={[
            { id: '8',  name: '8 channels'  },
            { id: '16', name: '16 channels' },
            { id: '24', name: '24 channels' },
            { id: '32', name: '32 channels' },
          ]}
          onOptionHover={() => {}}
        />
      ))}

      {row('Start at', (
        <NodeSelect
          value={String(settings.startChannel)} showEmpty={false} accent={accent}
          onChange={v => onChange({ startChannel: Number(v) })}
          options={startOptions}
          onOptionHover={() => {}}
        />
      ))}

      {row('Format', (
        <NodeSelect
          value={settings.valueFormat} showEmpty={false} accent={accent}
          onChange={v => onChange({ valueFormat: v as DmxNodeSettings['valueFormat'] })}
          options={[
            { id: 'dec', name: '0–255 (decimal)' },
            { id: 'pct', name: '0–100 (percent)' },
            { id: 'hex', name: '00–FF (hex)'     },
          ]}
          onOptionHover={() => {}}
        />
      ))}
    </div>
  );
}

// ── DMX Monitor (nodeType 16) ─────────────────────────────────────────────────
export const DmxMonitorNode = memo(function DmxMonitorNode ({ id, data, selected }: NodeProps) {
  const nodeData = data as DmxMonitorNodeData;
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete }                 = useNodeDelete(id);
  const { collapsed, toggleCollapsed }   = useNodeCollapsed(id);
  const [channels, setChannels]          = useState<number[]>(new Array(512).fill(0));
  const channelsRef                      = useRef<number[]>(new Array(512).fill(0));
  const portBodyRef                      = useRef<HTMLDivElement>(null);

  const [settings, setSettings] = useState<DmxNodeSettings>(() => ({
    ...DEFAULT_SETTINGS,
    ...(nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) as Partial<DmxNodeSettings> : {}),
  }));
  const settingsRef = useRef(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  const patchSettings = useCallback((patch: Partial<DmxNodeSettings>) => {
    setSettings(prev => ({ ...prev, ...patch }));
  }, []);

  const lastRender = useRef(0);

  useEffect(() => {
    const unsub = Bridge.onDmxSnapshot((snaps: DmxSnapshotEntry[]) => {
      const snap = snaps.find(s => s.id === id);
      if (!snap) return;
      const now = performance.now();
      if (now - lastRender.current < 66) return;  // ~15Hz max render rate
      const { startChannel, visibleCount } = settingsRef.current;
      let changed = false;
      for (let i = startChannel; i < startChannel + visibleCount && i < 512; i++) {
        if (channelsRef.current[i] !== snap.ch[i]) { changed = true; break; }
      }
      channelsRef.current = snap.ch;
      if (changed) { lastRender.current = now; setChannels([...snap.ch]); }
    });
    return unsub;
  }, [id]);

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
      <NodeHeader title="DMX MONITOR" accent={ACCENT}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}
      />

      {showSettings && (
        <DmxSettingsPanel
          settings={settings} isConsole={false}
          onChange={patchSettings} onClose={closeSettings}
        />
      )}

      {!collapsed && (
        <div ref={portBodyRef} className="nodrag"
          onMouseDown={e => e.stopPropagation()}
          onPointerDown={e => e.stopPropagation()}
          style={{ padding: '6px 8px' }}>

          {/* Page navigation */}
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

          {/* Bargraphs */}
          <div style={{ display: 'flex', gap: 0 }}>
            {visibleChannels.map((val, i) => (
              <DmxBargraph
                key={startChannel + i}
                ch={startChannel + i + 1}
                value={val}
                format={valueFormat}
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
        <DmxSettingsPanel
          settings={settings} isConsole={true}
          onChange={patchSettings} onClose={closeSettings}
        />
      )}

      {!collapsed && (
        <div ref={portBodyRef} className="nodrag"
          onMouseDown={e => e.stopPropagation()}
          onPointerDown={e => e.stopPropagation()}
          style={{ padding: '6px 8px' }}>

          {/* Page navigation */}
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

          {/* Faders */}
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
