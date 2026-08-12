import { memo, useCallback, useContext, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, DmxSnapshotEntry } from './Bridge';
import { useNodeSettings, useNodeDelete, useNodeCollapsed,
         NodeHeader, NodeHeaderButton, NodeHandle } from './NodeUtils';
import { HintContext } from './HintPanel';
import {
  DmxMonitorNodeData, DmxNodeSettings, DEFAULT_SETTINGS,
  ACCENT, CHANNELS, COL_W,
  navBtnStyle, DmxFader, DmxSettingsPanel,
} from './DmxShared';

export type { DmxMonitorNodeData };

// DmxConsoleNode.tsx — the interactive, read-write counterpart to
// DmxMonitorNode.tsx (which only displays incoming values). Shares its
// fader/settings-panel UI with DmxMonitorNode and both ArtNet variants via
// DmxShared.tsx; what's unique here is everything to do with actually
// setting values: dragging a fader, blackout, and persisting 512 channels'
// worth of state into settingsJson so it survives undo/redo and reload.

// ── DMX Console (nodeType 17) ─────────────────────────────────────────────────
export const DmxConsoleNode = memo(function DmxConsoleNode ({ id, data, selected }: NodeProps) {
  const nodeData = data as DmxMonitorNodeData;
  const { setHint }                              = useContext(HintContext);
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete }                         = useNodeDelete(id);
  const { collapsed, toggleCollapsed }           = useNodeCollapsed(id, (data as any)._forceCollapsed);
  // channels (state) drives what's rendered; channelsRef mirrors the same
  // 512 values for reading synchronously inside callbacks — React state
  // updates are async/batched, so a callback closing over `channels`
  // directly could read a stale array from before its own most recent
  // update. Every write below updates both, in that order.
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

  const commitPatch = useCallback((patch: Partial<DmxNodeSettings>) => {
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
      const b64: string | undefined = parsed?.dmxChannels;
      if (!b64) {
        channelsRef.current = new Array<number>(512).fill(0);
        setChannels(new Array<number>(512).fill(0));
        (window as any).__dmxSnapshots = (window as any).__dmxSnapshots ?? {};
        (window as any).__dmxSnapshots[id] = new Uint8Array(512);
        return;
      }
      const isBlank = channelsRef.current.every(v => v === 0);
      // Only restore from settingsJson if nothing's arrived yet. Two
      // sources can populate channels — this restore effect, and the live
      // Bridge.onDmxSnapshot subscription below — and a live snapshot can
      // easily land first. Restoring unconditionally here would let a
      // slower settingsJson parse clobber fresher live data that's
      // already on screen; once anything real is showing, this effect
      // backs off and leaves it alone.
      if (!isBlank) return;
      const bin = atob(b64);
      const ch = new Array<number>(512).fill(0);
      for (let i = 0; i < Math.min(512, bin.length); i++) ch[i] = bin.charCodeAt(i);
      channelsRef.current = ch;
      setChannels([...ch]);
      (window as any).__dmxSnapshots = (window as any).__dmxSnapshots ?? {};
      (window as any).__dmxSnapshots[id] = new Uint8Array(ch);
    } catch {}
  }, [id, nodeData.settingsJson]);

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

  // fullSettingsRef: always holds latest merged settingsJson, initialized
  // eagerly. `settings` only covers the UI-editable fields (visibleCount,
  // startChannel, valueFormat, customName) — the settings panel doesn't
  // need to know about dmxChannels or blackout, so they're not part of its
  // type. This ref holds the complete picture, everything actually
  // persisted, so a channel update (handleChange below) can merge in just
  // the one field it's changing without needing to reconstruct or guess
  // at the rest of the saved state.
  const fullSettingsRef = useRef<Record<string, unknown>>((() => {
    try {
      const base = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : {};
      return { ...DEFAULT_SETTINGS, ...base };
    } catch { return { ...(DEFAULT_SETTINGS as unknown as Record<string, unknown>) }; }
  })());
  useEffect(() => {
    try {
      const base = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : {};
      fullSettingsRef.current = { ...DEFAULT_SETTINGS, ...base, ...settings, blackout };
    } catch {}
  }, [nodeData.settingsJson, settings, blackout]);

  const handleChange = useCallback((ch: number, val: number) => {
    const next = [...channelsRef.current];
    next[ch] = val;
    channelsRef.current = next;
    setChannels([...next]);

    // Encode all 512 channels to base64 so the full universe survives in
    // settingsJson — needed for undo/redo and reload to restore exactly
    // where the fader grid was left, not just the one channel that moved.
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
    const merged = { ...fullSettingsRef.current, dmxChannels: b64 };
    fullSettingsRef.current = merged;
    // Two separate calls doing two separate jobs: setNodeSettings persists
    // the settingsJson snapshot above (live update, no Undo-history entry
    // — handleCommit below pushes the actual history entry on release).
    // setDmxConsoleChannel is the real-time path, sent straight to the
    // backend so the channel's actual DMX output changes as the fader
    // moves, independent of whatever the UI persists.
    Bridge.setNodeSettings(id, merged);
    Bridge.setDmxConsoleChannel(id, ch, val);
  }, [id]);

  const handleCommit = useCallback(() => {
    Bridge.commitNodeSettings(id);
  }, [id]);

  const handleBlackout = useCallback(() => {
    const next = !blackout;
    setBlackoutState(next);
    Bridge.setDmxBlackout(id, next);
    commitPatch({ blackout: next });
  }, [id, blackout, commitPatch]);

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
      <NodeHeader title={settings.customName || 'DMX CONSOLE'} accent={ACCENT}
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
                // Blackout forces the displayed (and transmitted) value to
                // 0 without touching the underlying stored channel values
                // — `val` itself is untouched. Turning blackout back off
                // restores whatever the faders were actually set to,
                // rather than every channel having been reset to 0.
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
