import { memo, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, ArtNetSnapshotEntry } from './Bridge';
import { useNodeSettings, useNodeDelete, useNodeCollapsed,
         NodeHeader, NodeHeaderButton, NodeHandle, SettingsPanelHeader } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import { HintContext } from './HintPanel';
import {
  DmxMonitorNodeData, DmxNodeSettings, DEFAULT_SETTINGS,
  CHANNELS, COL_W,
  navBtnStyle, DmxFader, NameInput,
} from './DmxShared';

const ACCENT = 'var(--artnet)';

export type { DmxMonitorNodeData };

// ArtNetConsoleNode.tsx — ArtNet's counterpart to DmxConsoleNode.tsx, and
// nearly identical to it: same channels/channelsRef dual-tracking, same
// settingsJson-restore race guard, same fullSettingsRef role, same
// blackout behaviour, same base64 universe encoding — see that file for
// the full reasoning behind each, not repeated here. What's genuinely
// different: this node needs its own settings panel (below) rather than
// the shared DmxSettingsPanel, since it has one extra field DMX doesn't
// — the ArtNet universe number — and its own settings type reflects that.

// ArtNet Console settings — same as DMX Console + universe
interface ArtNetConsoleSettings extends DmxNodeSettings {
  universe: number;
}

const DEFAULT_ARTNET_CONSOLE_SETTINGS: ArtNetConsoleSettings = {
  ...DEFAULT_SETTINGS,
  universe: 0,
};

// ── ArtNet Console (nodeType 19) ──────────────────────────────────────────────
export const ArtNetConsoleNode = memo(function ArtNetConsoleNode ({ id, data, selected }: NodeProps) {
  const nodeData = data as DmxMonitorNodeData;
  const { setHint }                              = useContext(HintContext);
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete }                         = useNodeDelete(id);
  const { collapsed, toggleCollapsed }           = useNodeCollapsed(id, (data as any)._forceCollapsed);
  // channels/channelsRef: see DmxConsoleNode.tsx — same dual-tracking
  // reasoning (state for rendering, ref for synchronous reads in callbacks).
  const [channels, setChannels]                  = useState<number[]>(new Array(512).fill(0));
  const [blackout, setBlackoutState]             = useState(false);
  const channelsRef                              = useRef<number[]>(new Array(512).fill(0));
  const portBodyRef                              = useRef<HTMLDivElement>(null);

  const [settings, setSettings] = useState<ArtNetConsoleSettings>(() => ({
    ...DEFAULT_ARTNET_CONSOLE_SETTINGS,
    ...(nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) as Partial<ArtNetConsoleSettings> : {}),
  }));
  const settingsRef = useRef(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  const patchSettings = useCallback((patch: Partial<ArtNetConsoleSettings>) => {
    setSettings(prev => ({ ...prev, ...patch }));
  }, []);

  const commitPatch = useCallback((patch: Partial<ArtNetConsoleSettings>) => {
    setSettings(prev => {
      const next = { ...prev, ...patch };
      const merged = { ...fullSettingsRef.current, ...next };
      fullSettingsRef.current = merged;
      Bridge.commitSettingsChange(id, merged);
      if ('customName' in patch) Bridge.setNodeLabel(id, patch.customName ?? '');
      return next;
    });
  }, [id]);

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
        ...(parsed.universe     !== undefined && { universe:     parsed.universe     }),
      }));
    } catch {}
  }, [nodeData.settingsJson]);

  // Restore channel values and blackout state from settingsJson (undo/redo)
  useEffect(() => {
    try {
      const raw = nodeData.settingsJson as string | undefined;
      const parsed = raw ? JSON.parse(raw) : null;
      if (!parsed) return;
      if (typeof parsed.blackout === 'boolean') {
        setBlackoutState(parsed.blackout);
      }
      const b64: string | undefined = parsed?.artNetChannels;
      if (!b64) {
        channelsRef.current = new Array<number>(512).fill(0);
        setChannels(new Array<number>(512).fill(0));
        (window as any).__artNetSnapshots = (window as any).__artNetSnapshots ?? {};
        (window as any).__artNetSnapshots[id] = new Uint8Array(512);
        return;
      }
      const isBlank = channelsRef.current.every(v => v === 0);
      // See DmxConsoleNode.tsx — same race-prevention reasoning: only
      // restore if a live snapshot hasn't already populated this.
      if (!isBlank) return;
      const bin = atob(b64);
      const ch = new Array<number>(512).fill(0);
      for (let i = 0; i < Math.min(512, bin.length); i++) ch[i] = bin.charCodeAt(i);
      channelsRef.current = ch;
      setChannels([...ch]);
      (window as any).__artNetSnapshots = (window as any).__artNetSnapshots ?? {};
      (window as any).__artNetSnapshots[id] = new Uint8Array(ch);
    } catch {}
  }, [id, nodeData.settingsJson]);

  // Subscribe to ArtNet snapshots for fader sync
  useEffect(() => {
    const unsub = Bridge.onArtNetSnapshot((snaps: ArtNetSnapshotEntry[]) => {
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

  // fullSettingsRef: always holds latest merged settingsJson
  const fullSettingsRef = useRef<Record<string, unknown>>((() => {
    try {
      const base = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : {};
      return { ...DEFAULT_ARTNET_CONSOLE_SETTINGS, ...base };
    } catch { return { ...(DEFAULT_ARTNET_CONSOLE_SETTINGS as unknown as Record<string, unknown>) }; }
  })());
  useEffect(() => {
    try {
      const base = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : {};
      fullSettingsRef.current = { ...DEFAULT_ARTNET_CONSOLE_SETTINGS, ...base, ...settings, blackout };
    } catch {}
  }, [nodeData.settingsJson, settings]);

  const handleChange = useCallback((ch: number, val: number) => {
    const next = [...channelsRef.current];
    next[ch] = val;
    channelsRef.current = next;
    setChannels([...next]);

    // Encode 512 channels to base64 in React for setNodeSettings (undo snapshot)
    const bytes = new Uint8Array(512);
    for (let i = 0; i < 512; i++) bytes[i] = next[i] ?? 0;
    let b64 = '';
    const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
    for (let i = 0; i < 512; i += 3) {
      const n = (bytes[i] << 16) | (bytes[i+1] << 8) | bytes[i+2];
      b64 += chars[(n >> 18) & 63] + chars[(n >> 12) & 63]
           + (i+1 < 512 ? chars[(n >> 6) & 63] : '=')
           + (i+2 < 512 ? chars[n & 63] : '=');
    }
    const merged = { ...fullSettingsRef.current, artNetChannels: b64 };
    fullSettingsRef.current = merged;
    // Two calls, two jobs — see DmxConsoleNode.tsx's handleChange for the
    // full reasoning (persistence vs. real-time transmission).
    Bridge.setNodeSettings(id, merged);
    Bridge.setArtNetConsoleChannel(id, ch, val);
  }, [id]);

  const handleCommit = useCallback(() => {
    Bridge.commitNodeSettings(id);
  }, [id]);

  const handleBlackout = useCallback(() => {
    const next = !blackout;
    setBlackoutState(next);
    Bridge.setArtNetBlackout(id, next);
    commitPatch({ blackout: next });
  }, [id, blackout, commitPatch]);

  const { visibleCount, startChannel, valueFormat, universe } = settings;
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
      boxShadow: selected ? `0 0 0 1px ${ACCENT}, 0 8px 32px var(--artnet-glow)` : '0 4px 16px rgba(0,0,0,.5)',
      minWidth: nodeW, fontFamily: "'JetBrains Mono', monospace", position: 'relative',
    }}>
      <NodeHeader title={settings.customName || 'ARTNET CONSOLE'} accent={ACCENT}
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}>
        <NodeHeaderButton
          onClick={handleBlackout}
          active={blackout}
          activeAccent="#ef5350"
          onHint={{
            onMouseEnter: () => setHint({ title: 'Blackout', body: 'Set all ArtNet channels to 0. Click again to restore.' }),
            onMouseLeave: () => setHint(null),
          }}>
          <span style={{ fontSize: 8, fontWeight: 700, letterSpacing: '0.05em' }}>BO</span>
        </NodeHeaderButton>
      </NodeHeader>
      {showSettings && (
        <ArtNetConsoleSettingsPanel
          settings={settings}
          onChange={patchSettings}
          onCommit={commitPatch}
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
              <span style={{ marginLeft: 4, color: ACCENT, opacity: 0.8 }}>[Uni {universe}]</span>
              {blackout && <span style={{ color: '#ef5350', marginLeft: 4 }}>● BO</span>}
            </div>
            <button className="nodrag"
              onClick={() => commitPatch({ startChannel: Math.min(CHANNELS - visibleCount, startChannel + visibleCount) })}
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
                onCommit={handleCommit}
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

// ── ArtNet Console Settings Panel ─────────────────────────────────────────────
// Not DmxShared's DmxSettingsPanel — that component has no Universe field
// and no way to add one without affecting every other DMX/ArtNet node that
// shares it, so ArtNet Console (and ArtNet Monitor) each get their own
// panel instead, otherwise matching DmxSettingsPanel's layout row-for-row.
function ArtNetConsoleSettingsPanel ({ settings, onChange, onCommit, onClose }: {
  settings: ArtNetConsoleSettings;
  onChange: (s: Partial<ArtNetConsoleSettings>) => void;
  onCommit: (s: Partial<ArtNetConsoleSettings>) => void;
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
      <SettingsPanelHeader
        title="ARTNET CONSOLE"
        onReset={() => onCommit({ ...DEFAULT_ARTNET_CONSOLE_SETTINGS })}
        onClose={onClose}
      />
      {row('Name', (
        <NameInput
          value={settings.customName}
          placeholder="ArtNet Console"
          onChange={v => onChange({ customName: v })}
          onCommit={v => onCommit({ customName: v })}
        />
      ))}
      {row('Universe', (
        // 0-32767: ArtNet 4's Port-Address is a 15-bit value (Net × 7 bits
        // + Sub-Net × 4 bits + Universe × 4 bits) — not an arbitrary limit,
        // it's the actual addressable range the protocol allows.
        <input type="number" min={0} max={32767}
          value={settings.universe}
          onChange={e => onChange({ universe: Number(e.target.value) })}
          onBlur={e => onCommit({ universe: Number(e.target.value) })}
          onKeyDown={e => { if (e.key === 'Enter') onCommit({ universe: Number((e.target as HTMLInputElement).value) }); }}
          style={{
            width: '100%', background: 'var(--surface)', border: `1px solid ${ACCENT}`,
            color: 'var(--text)', fontSize: 10, borderRadius: 3,
            padding: '2px 6px', fontFamily: "'JetBrains Mono', monospace", outline: 'none',
          }}
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
    </div>
  );
}
