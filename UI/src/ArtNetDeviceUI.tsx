/**
 * ArtNetDeviceUI.tsx
 *
 * Settings panel + summary components for ArtNet In/Out device nodes.
 * Extracted from GenericNode.tsx (2026-07-19) — see AudioDeviceUI.tsx's
 * header comment for the full rationale and naming convention explanation.
 */

import { useState, useEffect, useRef } from 'react';
import { Bridge } from './Bridge';
import { SettingsPanelHeader, isLikelyCompleteHost } from './NodeUtils';

// ── ArtNet settings panel ─────────────────────────────────────────────────────
export function ArtNetDeviceSettingsPanel ({ nodeId, nodeType, universe, targetHost, onClose }: {
  nodeId:     string;
  nodeType:   12 | 13;
  universe:   number;
  targetHost: string;
  onClose:    () => void;
}) {
  const isOut  = nodeType === 13;
  const title  = isOut ? 'ARTNET OUT Settings' : 'ARTNET IN Settings';
  const accent = 'var(--artnet)';

  // Instant local state for display, explicit-action commit for the
  // backend — same pattern established for MQTT Subscribe/Publish/UDP/OSC.
  const [localUniverse, setLocalUniverse] = useState(universe);
  const [localTargetHost, setLocalTargetHost] = useState(targetHost);

  useEffect(() => { setLocalUniverse(universe); },     [universe]);
  useEffect(() => { setLocalTargetHost(targetHost); }, [targetHost]);

  const updateLocal = (next: { universe?: number; targetHost?: string }) => {
    if (next.universe   !== undefined) setLocalUniverse(next.universe);
    if (next.targetHost !== undefined) setLocalTargetHost(next.targetHost);
  };

  const commitNow = (overrides: { universe?: number; targetHost?: string } = {}) => {
    Bridge.setArtNetSettings(
      nodeId,
      overrides.universe   ?? localUniverse,
      overrides.targetHost ?? localTargetHost,
    );
  };

  // See UdpDeviceSettingsPanel's comment for why programmatic blur() calls
  // need to suppress the separate onBlur handler from also firing.
  const suppressNextBlurRef = useRef(false);
  const blurSuppressed = (el: HTMLInputElement) => {
    suppressNextBlurRef.current = true;
    el.blur();
  };
  const commitOnBlur = (overrides: { universe?: number; targetHost?: string } = {}) => {
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
          updateLocal({ universe: 0, targetHost: '' });
          commitNow({ universe: 0, targetHost: '' });
        }}
        onClose={onClose}
      />

      <div style={labelStyle}>Universe</div>
      <input
        type="number" min={0} max={32767} value={localUniverse || ''}
        placeholder="0"
        onChange={e => updateLocal({ universe: parseInt(e.target.value, 10) || 0 })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />
      <div style={{ fontSize: 9, color: accent, marginTop: 4, opacity: 0.7 }}>
        Port fixed at 6454 (Art-Net spec)
      </div>

      {isOut && (<>
        <div style={labelStyle}>Target Host</div>
        <input
          type="text" value={localTargetHost} placeholder="192.168.1.255"
          autoCapitalize="off" autoCorrect="off" spellCheck={false}
          onChange={e => updateLocal({ targetHost: e.target.value })}
          onKeyDown={commitHostOnEnter}
          onBlur={() => commitOnBlur()}
          style={inputStyle}
        />
        <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 4, opacity: 0.7 }}>
          Use 255.255.255.255 for broadcast
        </div>
      </>)}

      {isOut && !localTargetHost && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set a target host to activate
        </div>
      )}
    </div>
  );
}

// ── ArtNet port summary label ─────────────────────────────────────────────────
export function ArtNetPortSummary ({ universe, targetHost, byteRate, onClick }: {
  universe:   number;
  targetHost: string;
  byteRate:   string;
  onClick:    () => void;
}) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  // For Out nodes targetHost is the indicator; for In nodes universe alone is enough
  const isConfigured = universe >= 0;
  if (!isConfigured) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        Not configured
      </div>
    );
  }
  const label = `uni ${universe}${targetHost ? ` · ${targetHost}` : ''}`;
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>{label}</span>
      {byteRate && <span style={{ color: 'var(--artnet)', opacity: 0.85 }}>{byteRate}</span>}
    </div>
  );
}

