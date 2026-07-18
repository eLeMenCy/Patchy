/**
 * OscDeviceUI.tsx
 *
 * Settings panel + summary components for OSC In/Out device nodes.
 * Extracted from GenericNode.tsx (2026-07-19) — see AudioDeviceUI.tsx's
 * header comment for the full rationale and naming convention explanation.
 */

import { useState, useEffect, useRef } from 'react';
import { Bridge } from './Bridge';
import { SettingsPanelHeader, isLikelyCompleteHost } from './NodeUtils';

// ── OSC IN/OUT settings panel ────────────────────────────────────────────────
export function OscDeviceSettingsPanel ({ nodeId, nodeType, port, targetHost, oscAddress, onClose }: {
  nodeId:      string;
  nodeType:    10 | 11;
  port:        number;
  targetHost:  string;
  oscAddress:  string;
  onClose:     () => void;
}) {
  const isOut = nodeType === 11;
  const title = isOut ? 'OSC OUT Settings' : 'OSC IN Settings';
  const accent = 'var(--osc)';

  // Instant local state for display, explicit-action commit for the
  // backend — same pattern established for MQTT Subscribe/Publish/UDP.
  const [localPort, setLocalPort] = useState(port);
  const [localTargetHost, setLocalTargetHost] = useState(targetHost);
  const [localOscAddress, setLocalOscAddress] = useState(oscAddress);

  useEffect(() => { setLocalPort(port); },             [port]);
  useEffect(() => { setLocalTargetHost(targetHost); }, [targetHost]);
  useEffect(() => { setLocalOscAddress(oscAddress); }, [oscAddress]);

  const updateLocal = (next: { port?: number; targetHost?: string; oscAddress?: string }) => {
    if (next.port        !== undefined) setLocalPort(next.port);
    if (next.targetHost  !== undefined) setLocalTargetHost(next.targetHost);
    if (next.oscAddress  !== undefined) setLocalOscAddress(next.oscAddress);
  };

  const commitNow = (overrides: { port?: number; targetHost?: string; oscAddress?: string } = {}) => {
    Bridge.setOscSettings(
      nodeId,
      overrides.port       ?? localPort,
      overrides.targetHost ?? localTargetHost,
      overrides.oscAddress ?? localOscAddress,
    );
  };

  // See UdpDeviceSettingsPanel's comment for why programmatic blur() calls
  // need to suppress the separate onBlur handler from also firing.
  const suppressNextBlurRef = useRef(false);
  const blurSuppressed = (el: HTMLInputElement) => {
    suppressNextBlurRef.current = true;
    el.blur();
  };
  const commitOnBlur = (overrides: { port?: number; targetHost?: string; oscAddress?: string } = {}) => {
    if (suppressNextBlurRef.current) { suppressNextBlurRef.current = false; return; }
    commitNow(overrides);
  };

  const commitOnEnter = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') { e.preventDefault(); commitNow(); blurSuppressed(e.target as HTMLInputElement); }
  };

  // Target Host gets its own Enter handler — see UdpDeviceSettingsPanel's
  // comment for why blur()-then-refocus is used to trigger the beep as
  // intentional "not valid yet" feedback on a malformed address.
  const commitHostOnEnter = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key !== 'Enter') return;
    const candidate = (e.target as HTMLInputElement).value;
    if (!isLikelyCompleteHost(candidate)) {
      const el = e.target as HTMLInputElement;
      suppressNextBlurRef.current = true;
      el.blur();
      requestAnimationFrame(() => el.focus());
      return;
    }
    e.preventDefault();
    commitNow({ targetHost: candidate });
    blurSuppressed(e.target as HTMLInputElement);
  };

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
        width: 200, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title={title}
        onReset={() => {
          updateLocal({ port: 0, targetHost: '', oscAddress: '/patchy' });
          commitNow({ port: 0, targetHost: '', oscAddress: '/patchy' });
        }}
        onClose={onClose}
      />

      <div style={labelStyle}>Port</div>
      <input
        type="number" min={1} max={65535} value={localPort || ''}
        placeholder="e.g. 8000"
        onChange={e => updateLocal({ port: parseInt(e.target.value, 10) || 0 })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      {isOut && (<>
        <div style={labelStyle}>Target Host</div>
        <input
          type="text" value={localTargetHost} placeholder="192.168.1.50"
          autoCapitalize="off" autoCorrect="off" spellCheck={false}
          onChange={e => updateLocal({ targetHost: e.target.value })}
          onKeyDown={commitHostOnEnter}
          onBlur={() => commitOnBlur()}
          style={inputStyle}
        />

        <div style={labelStyle}>OSC Address</div>
        <input
          type="text" value={localOscAddress} placeholder="/patchy"
          autoCapitalize="off" autoCorrect="off" spellCheck={false}
          onChange={e => updateLocal({ oscAddress: e.target.value })}
          onKeyDown={commitOnEnter}
          onBlur={() => commitOnBlur({ oscAddress: localOscAddress || '/patchy' })}
          style={inputStyle}
        />
        <div style={{ fontSize: 9, color: accent, marginTop: 4, opacity: 0.7 }}>
          Must start with /
        </div>
      </>)}

      {!localPort && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set a port to activate
        </div>
      )}
    </div>
  );
}

// ── OSC port summary label ────────────────────────────────────────────────────
export function OscPortSummary ({ port, targetHost, oscAddress, byteRate, onClick }: {
  port:       number;
  targetHost: string;
  oscAddress: string;
  byteRate:   string;
  onClick:    () => void;
}) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  if (!port) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        No port set
      </div>
    );
  }
  const addressTag = oscAddress && oscAddress !== '/patchy' ? ` · ${oscAddress}` : '';
  const hostTag = targetHost ? ` · ${targetHost}` : '';
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>:{port}{hostTag}{addressTag}</span>
      {byteRate && <span style={{ color: 'var(--osc)', opacity: 0.85 }}>{byteRate}</span>}
    </div>
  );
}

