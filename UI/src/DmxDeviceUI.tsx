/**
 * DmxDeviceUI.tsx
 *
 * Settings panel + summary components for DMX In/Out device nodes.
 * Extracted from GenericNode.tsx (2026-07-19) — see AudioDeviceUI.tsx's
 * header comment for the full rationale and naming convention explanation.
 */

import { useContext } from 'react';
import { Bridge } from './Bridge';
import { SettingsPanelHeader, resolveCssColor } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import { HintContext } from './HintPanel';

// ── DMX settings panel ────────────────────────────────────────────────────────
export function DmxDeviceSettingsPanel ({ nodeId, nodeType, devicePath, serialPorts, onClose }: {
  nodeId:      string;
  nodeType:    14 | 15;
  devicePath:  string;
  serialPorts: string[];
  onClose:     () => void;
}) {
  const isOut  = nodeType === 15;
  const title  = isOut ? 'DMX OUT Settings' : 'DMX IN Settings';
  const accent = 'var(--dmx)';

  const commit = (path: string) => Bridge.setDmxSettings(nodeId, path);

  const inputStyle: React.CSSProperties = {
    width: '100%', fontSize: 10, padding: '3px 6px', marginTop: 2,
    background: 'var(--surface)', border: '1px solid var(--border)',
    borderRadius: 3, color: 'var(--text-dim)',
    fontFamily: "'JetBrains Mono', monospace",
  };

  const labelStyle: React.CSSProperties = {
    fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
    textTransform: 'uppercase', marginTop: 8, marginBottom: 4,
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
        width: 220, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title={title}
        onReset={() => commit('')}
        onClose={onClose}
      />

      <div style={labelStyle}>Serial Port</div>

      {serialPorts.length === 0 ? (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 4 }}>
          No serial ports found.<br />
          Plug in your Enttec Pro and{' '}
          <span
            className="nodrag"
            onClick={() => Bridge.listSerialPorts()}
            style={{ color: accent, cursor: 'pointer', textDecoration: 'underline' }}>
            refresh
          </span>
        </div>
      ) : (
        <select
          value={devicePath}
          onChange={e => commit(e.target.value)}
          className="nodrag"
          style={{ ...inputStyle, cursor: 'pointer' }}>
          <option value="">— select port —</option>
          {serialPorts.map(p => (
            <option key={p} value={p}>{p.replace('/dev/cu.', '').replace('/dev/', '')}</option>
          ))}
        </select>
      )}

      <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 6, opacity: 0.7 }}>
        Enttec DMX USB Pro · 57600 8N2
      </div>

      <div
        onClick={() => Bridge.listSerialPorts()}
        className="nodrag"
        style={{
          fontSize: 9, color: accent, marginTop: 6,
          cursor: 'pointer', opacity: 0.8,
        }}>
        ↺ Refresh port list
      </div>

      {!devicePath && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Select a port to activate
        </div>
      )}
    </div>
  );
}

// ── DMX port summary label ────────────────────────────────────────────────────
export function DmxPortSummary ({ devicePath, byteRate, onClick }: {
  devicePath: string;
  byteRate:   string;
  onClick:    () => void;
}) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  if (!devicePath) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        No port selected
      </div>
    );
  }
  // Show just the device name without the full /dev/cu. prefix
  const shortName = devicePath.replace('/dev/cu.', '').replace('/dev/', '').replace('COM', 'COM');
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>{shortName}</span>
      {byteRate && <span style={{ color: 'var(--dmx)', opacity: 0.85 }}>{byteRate}</span>}
    </div>
  );
}

// ── DMX device selector — inline on node face, same pattern as DeviceSelector ──
export function DmxDeviceSelector ({ nodeId, devicePath, universe, serialPorts }: {
  nodeId:      string;
  devicePath:  string;
  universe:    number;
  serialPorts: string[];
}) {
  const { setHint } = useContext(HintContext);
  const accent = 'var(--dmx)';

  const opts = serialPorts.map(p => ({
    id:   p,
    name: p.replace('/dev/cu.', '').replace('/dev/', ''),
  }));

  return (
    <NodeSelect
      value={devicePath}
      onChange={v => Bridge.setDmxSettings(nodeId, v, universe)}
      options={opts}
      disabled={serialPorts.length === 0}
      accent={accent}
      onOptionHover={h => setHint(h)}
    />
  );
}

// ── DMX settings panel (gear) — universe selection ───────────────────────────
export function DmxSettingsPanel ({ nodeId, nodeType, devicePath, universe, isMk2, status = 0, onClose }: {
  status?:    number;   // v0.0.916 — see dmxStatusText()
  nodeId:     string;
  nodeType:   14 | 15;
  devicePath: string;
  universe:   number;
  isMk2:      boolean;
  onClose:    () => void;
}) {
  const isOut = nodeType === 15;
  const accent = 'var(--dmx)';
  // FIXED (2026-08-12): resolved once, used only where a hex-alpha suffix
  // gets concatenated below — same bug class as NodeSelect.tsx.
  const accentHex = resolveCssColor(accent);

  const commit = (u: number) => Bridge.setDmxSettings(nodeId, devicePath, u);

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
        title={isOut ? 'DMX OUT Settings' : 'DMX IN Settings'}
        onReset={() => commit(0)}
        onClose={onClose}
      />

      <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 6 }}>
        Universe
      </div>

      <div style={{ display: 'flex', gap: 6 }}>
        {[0, 1].map(u => {
          const noDevice = !devicePath;
          const disabled = noDevice || (u === 1 && !isMk2);
          const active   = universe === u && !disabled;
          return (
            <div
              key={u}
              className="nodrag"
              onClick={() => { if (!disabled) commit(u); }}
              title={noDevice ? 'Select a port first' : disabled ? 'Requires Enttec Pro Mk2' : undefined}
              style={{
                flex: 1, textAlign: 'center', padding: '4px 0',
                fontSize: 10, borderRadius: 3,
                cursor: disabled ? 'not-allowed' : 'pointer',
                border: `1px solid ${active ? accent : 'var(--border)'}`,
                background: active ? `${accentHex}22` : 'transparent',
                color: disabled ? 'var(--text-muted)' : active ? accent : 'var(--text-muted)',
                opacity: disabled ? 0.35 : 1,
                transition: 'all .12s',
              }}>
              {u === 0 ? 'Uni 0' : 'Uni 1'}
            </div>
          );
        })}
      </div>

      <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 6, opacity: 0.7 }}>
        {!devicePath ? 'Select a port to activate'
          : isMk2 ? 'Enttec Pro Mk2 detected ✓'
          : 'Uni 1 requires Pro Mk2'}
      </div>

      {!!status && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          {dmxStatusText(status, true)}
        </div>
      )}

      <div
        onClick={() => Bridge.listSerialPorts()}
        className="nodrag"
        style={{ fontSize: 9, color: accent, marginTop: 8, cursor: 'pointer', opacity: 0.8 }}>
        ↺ Refresh port list
      </div>
    </div>
  );
}

// ── DMX port summary label (byte-rate only, shown above selector) ─────────────
export function DmxByteRateLabel ({ byteRate }: { byteRate: string }) {
  if (!byteRate) return null;
  return (
    <div style={{
      fontSize: 9, color: 'var(--dmx)', textAlign: 'right',
      marginBottom: 2, letterSpacing: '0.05em', opacity: 0.85,
    }}>
      {byteRate}
    </div>
  );
}

// ── DMX interface status — v0.0.916 (2026-09-28) ─────────────────────────────
// From the backend's shared-port state (DmxSharedPort): 1 = held by another
// application (opened exclusively), 2 = can't be opened, 3 = In + Out on the
// same classic DMX USB Pro, which is half-duplex. 1/2 are only reported after
// 1 s of failing; the backend retries quietly and clears it on its own.
export function dmxStatusText (status: number, long = false): string {
  switch (status) {
    case 1: return long ? 'Interface in use by another application — retrying…' : 'Interface in use';
    case 2: return long ? "Interface can't be opened — retrying…" : "Can't open interface";
    case 3: return long ? 'Classic DMX USB Pro is half-duplex: a DMX In and a DMX Out cannot share it. Use a second interface (or a Pro Mk2).'
                        : 'Half-duplex: In + Out on one Pro';
    default: return '';
  }
}

export function DmxStatusLine ({ status }: { status: number }) {
  if (!status) return null;
  return (
    <div style={{ fontSize: 9, color: '#ef5350', textAlign: 'center', marginTop: 3, letterSpacing: '0.05em' }}>
      {dmxStatusText(status)}
    </div>
  );
}
