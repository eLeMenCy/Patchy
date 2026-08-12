import { memo, useCallback, useRef, useState } from 'react';
import { NodeSelect } from './NodeSelect';
import { SettingsPanelHeader } from './NodeUtils';

// DmxShared.tsx — shared types, constants, and components for both DMX and
// ArtNet Monitor/Console nodes (DmxMonitorNode.tsx, DmxConsoleNode.tsx,
// ArtNetMonitorNode.tsx, ArtNetConsoleNode.tsx all import from here). One
// shared file rather than four separate copies, since DMX and ArtNet
// Monitor/Console share an identical UI shape (same fader grid, same
// settings panel, same channel-range navigation) — only the wire protocol
// underneath differs, which lives in each consumer's own file, not here.

// ── Types ─────────────────────────────────────────────────────────────────────
export interface DmxMonitorNodeData {
  label:    string;
  nodeType: 16 | 17 | 18 | 19;   // 16=DMX Monitor, 17=DMX Console, 18=ArtNet Monitor, 19=ArtNet Console
  ports:    { id: string; label: string; type: string; direction: 'input' | 'output' }[];
  settingsJson?: string;
  [key: string]: unknown;   // ReactFlow's NodeProps generic constraint requires this
}

export interface DmxNodeSettings {
  visibleCount: 8 | 16 | 24 | 32;
  startChannel: number;
  valueFormat:  'dec' | 'pct' | 'hex';
  customName:   string;
  blackout:     boolean;
}

export const DEFAULT_SETTINGS: DmxNodeSettings = {
  visibleCount: 8,
  startChannel: 0,
  valueFormat:  'dec',
  customName:   '',
  blackout:     false,
};

// Two colour constants, deliberately not just one: ACCENT is a CSS custom
// property reference for ordinary DOM styling, but Canvas 2D's
// fillStyle/strokeStyle cannot resolve CSS custom properties at all — it
// silently fails and defaults to black rather than throwing (this exact
// bug once made a whole waveform render invisible elsewhere in this
// project — see AudioToDmxNode.tsx's resolveCssColor). ACCENT_C is the
// same colour as a literal hex, for anything drawn on a <canvas>.
export const ACCENT   = 'var(--dmx)';
export const ACCENT_C = '#fbbf24';   // resolved colour for canvas drawing
export const CHANNELS = 512;         // full DMX/ArtNet universe size
export const COL_W    = 24;
export const FADER_H  = 80;

export function formatVal(v: number, fmt: DmxNodeSettings['valueFormat']): string {
  if (fmt === 'pct') return `${Math.round(v / 255 * 100)}`;
  if (fmt === 'hex') return v.toString(16).toUpperCase().padStart(2, '0');
  return String(v);
}

// ── Nav button style ──────────────────────────────────────────────────────────
export function navBtnStyle(disabled: boolean): React.CSSProperties {
  return {
    background: 'transparent', border: '1px solid var(--border)',
    color: disabled ? 'var(--text-muted)' : ACCENT,
    borderRadius: 3, padding: '1px 5px', fontSize: 8,
    cursor: disabled ? 'not-allowed' : 'pointer',
    opacity: disabled ? 0.4 : 1,
    fontFamily: "'JetBrains Mono', monospace",
  };
}

// ── Vertical fader (Console — interactive DOM element) ────────────────────────
// HTML <input type="range"> has no native vertical orientation — the
// writingMode/direction combo below is the standard cross-browser CSS
// trick to fake one (writingMode rotates the whole box, direction:rtl
// flips it back so higher values still end up at the top, not the
// bottom). `ch` here is the 1-based channel number shown to the user
// (matches DMX's own 1-512 convention); onChange subtracts 1 since the
// underlying channel array/API is 0-indexed — easy to misread if you're
// not expecting the shift.
export function DmxFader ({ ch, value, format, onChange, onCommit, readOnly }: {
  ch:       number;
  value:    number;
  format:   DmxNodeSettings['valueFormat'];
  onChange: (ch: number, val: number) => void;
  onCommit: () => void;
  readOnly: boolean;
}) {
  const [editing, setEditing] = useState(false);
  const [editVal, setEditVal] = useState('');
  const inputRef = useRef<HTMLInputElement>(null);
  const isDragging = useRef(false);

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
    onCommit();
    setEditing(false);
  }, [ch, editVal, format, onChange, onCommit]);

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
        // stopPropagation + className="nodrag" below: without both, ReactFlow
        // reads a mousedown/pointerdown on this fader as "start dragging the
        // whole node" rather than "interact with the slider" — nodrag is
        // ReactFlow's own documented opt-out class, stopPropagation covers
        // the mouse-drag path nodrag alone doesn't reach.
        onMouseDown={e => { e.stopPropagation(); isDragging.current = true; }}
        onMouseUp={() => { if (isDragging.current) { isDragging.current = false; onCommit(); } }}
        onKeyUp={() => onCommit()}
        onChange={e => onChange(ch - 1, Number(e.target.value))}
        onPointerDown={e => e.stopPropagation()}
        style={{
          writingMode: 'vertical-lr' as const,
          direction: 'rtl' as const,
          height: FADER_H, width: COL_W,
          cursor: readOnly ? 'default' : 'pointer',
          accentColor: ACCENT,
          opacity: readOnly ? 0.4 : 1,
        }}
        className="nodrag"
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

// ── Name input with debounced commit (500ms after last keystroke) ─────────────
// onChange fires (and pushes to local state) on every keystroke for a
// responsive-feeling input, but onCommit — which pushes an Undo-history
// entry — is deliberately debounced rather than firing per-keystroke, or
// the Undo stack would fill with one step per character typed. handleBlur
// commits immediately regardless of the timer, so navigating away mid-type
// doesn't lose the in-progress edit.
export function NameInput ({ value, placeholder, onChange, onCommit }: {
  value:       string;
  placeholder: string;
  onChange:    (v: string) => void;
  onCommit:    (v: string) => void;
}) {
  const timerRef  = useRef<ReturnType<typeof setTimeout> | null>(null);
  const latestRef = useRef(value);

  const handleChange = (v: string) => {
    latestRef.current = v;
    onChange(v);
    if (timerRef.current) clearTimeout(timerRef.current);
    timerRef.current = setTimeout(() => { onCommit(latestRef.current); }, 500);
  };

  const handleBlur = () => {
    if (timerRef.current) { clearTimeout(timerRef.current); timerRef.current = null; }
    onCommit(latestRef.current);
  };

  return (
    <input
      type="text"
      value={value}
      placeholder={placeholder}
      onChange={e => handleChange(e.target.value)}
      onBlur={handleBlur}
      onKeyDown={e => { if (e.key === 'Enter') handleBlur(); }}
      style={{
        flex: 1, width: '100%', background: 'var(--surface)',
        border: '1px solid var(--border)', color: 'var(--text)',
        fontSize: 10, borderRadius: 3, padding: '2px 6px',
        fontFamily: "'JetBrains Mono', monospace", outline: 'none',
      }}
    />
  );
}

// ── Shared settings panel ─────────────────────────────────────────────────────
// One panel serves both roles — Console (read-write, user sets values) and
// Monitor (read-only, just displays incoming values) — driven entirely by
// the `isConsole` flag, which only changes the title text here; whether
// the faders themselves are actually editable is decided by each
// consumer's own `readOnly` prop passed to DmxFader, not by this panel.
export function DmxSettingsPanel ({ settings, isConsole, onChange, onCommit, onClose }: {
  settings:  DmxNodeSettings;
  isConsole: boolean;
  onChange:  (s: Partial<DmxNodeSettings>) => void;
  onCommit:  (s: Partial<DmxNodeSettings>) => void;
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
        onReset={() => onCommit({ ...DEFAULT_SETTINGS })}
        onClose={onClose}
      />
      {row('Name', (
        <NameInput
          value={settings.customName}
          placeholder={isConsole ? 'DMX Console' : 'DMX Monitor'}
          onChange={v => onChange({ customName: v })}
          onCommit={v => onCommit({ customName: v })}
        />
      ))}
      {row('Channels', (
        <NodeSelect value={String(settings.visibleCount)} showEmpty={false} accent={accent}
          onChange={v => onCommit({ visibleCount: Number(v) as 8|16|24|32, startChannel: 0 })}
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
          onChange={v => onCommit({ startChannel: Number(v) })}
          options={startOptions}
          onOptionHover={() => {}} />
      ))}
      {row('Format', (
        <NodeSelect value={settings.valueFormat} showEmpty={false} accent={accent}
          onChange={v => onCommit({ valueFormat: v as DmxNodeSettings['valueFormat'] })}
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

// suppress unused import warning — memo is used by consumers
export { memo };
