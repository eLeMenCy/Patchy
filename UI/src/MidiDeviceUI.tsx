/**
 * MidiDeviceUI.tsx
 *
 * Settings panel + summary label for MidiOutDeviceNode's own channel
 * filter (2026-09-21) — the physical output side only, since
 * MidiInDeviceNode has nothing to filter (it has no channel-scoped
 * concept of its own, just relays whatever a device sends).
 *
 * Named "*DeviceUI.tsx" not "*NodeUI.tsx" deliberately, matching
 * AudioDeviceUI.tsx's own established convention — an internal helper
 * GenericNode.tsx imports, not its own registered node type. Kept
 * separate from AudioDeviceUI.tsx itself (rather than added there)
 * since these two components are genuinely MIDI-specific, not shared
 * with Audio the way DeviceSelector already is.
 *
 * The actual channel toggles deliberately mirror MidiMonitorNode.tsx's
 * own small numbered-button style (its `chBtn`), not
 * AudioDeviceSettingsPanel's own checkbox-based one — the user's own
 * explicit request ("like in MIDI Monitor"), and a better fit for 16
 * channels than a checkbox list. Panel positioning/header/stop-
 * propagation handlers otherwise mirror AudioDeviceSettingsPanel's own
 * established structure directly.
 */

import { SettingsPanelHeader } from './NodeUtils';

// ── Channel summary label ─────────────────────────────────────────────────────
// Unlike AudioDeviceUI's own ChannelSummary (which returns null on an
// empty array — right for Audio's "not configured yet" default), MIDI's
// own empty-selection state is a real, meaningful one worth naming
// explicitly: Omni. Format per the user's own request: "Ch. 1", "Ch. 1,
// 2, 5", "Ch. Omni".
export function MidiChannelSummary ({ channels }: { channels: number[] }) {
  const label = channels.length === 0
    ? 'Ch. Omni'
    : `Ch. ${channels.slice().sort((a, b) => a - b).join(', ')}`;
  return (
    <div style={{
      fontSize: 9, color: 'var(--text-muted)', textAlign: 'center',
      marginBottom: 3, letterSpacing: '0.05em',
    }}>
      {label}
    </div>
  );
}

// ── Channel filter settings panel ─────────────────────────────────────────────
export function MidiOutChannelFilterPanel ({ channels, onChannelsChange, onClose }: {
  channels:         number[];
  onChannelsChange: (next: number[]) => void;
  onClose:          () => void;
}) {
  const accent = 'var(--midi)';

  const chBtn = (ch: number) => {
    const active = channels.includes (ch);
    return (
      <div key={ch} onClick={() => {
          const next = active
            ? channels.filter (c => c !== ch)
            : [...channels, ch].sort ((a, b) => a - b);
          onChannelsChange (next);
        }}
        style={{
          width: 22, height: 18, display: 'flex', alignItems: 'center',
          justifyContent: 'center', fontSize: 9, borderRadius: 3, cursor: 'pointer',
          background: active ? accent : 'var(--surface)',
          color: active ? '#000' : 'var(--text-muted)',
          border: `1px solid ${active ? accent : 'var(--border)'}`,
          userSelect: 'none',
        }}>
        {ch}
      </div>
    );
  };

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
        width: 200, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title="MIDI Out Channels"
        onReset={() => onChannelsChange ([])}
        onClose={onClose}
      />
      <div style={{ fontSize: 9, color: 'var(--text-muted)', marginBottom: 4 }}>
        All channels when none selected
      </div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 3 }}>
        {Array.from ({ length: 16 }, (_, i) => chBtn (i + 1))}
      </div>
    </div>
  );
}
