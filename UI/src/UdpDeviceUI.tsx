/**
 * UdpDeviceUI.tsx
 *
 * Settings panel + summary components for UDP In/Out device nodes.
 * Extracted from GenericNode.tsx (2026-07-19) — see AudioDeviceUI.tsx's
 * header comment for the full rationale and naming convention explanation.
 */

import { useState, useEffect, useRef } from 'react';
import { Bridge } from './Bridge';
import { SettingsPanelHeader, isLikelyCompleteHost } from './NodeUtils';

// ── UDP port summary label ────────────────────────────────────────────────────
export function UdpPortSummary ({ port, mode, targetHost, multicastAddr, byteRate, onClick }: { port: number; mode: 0 | 1 | 2; targetHost: string; multicastAddr: string; byteRate: string; onClick: () => void }) {
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
  const modeTag = mode === 1 ? ` · multicast${multicastAddr ? ` · ${multicastAddr}` : ''}`
                : mode === 2 ? ' · broadcast'
                : (targetHost ? ` · ${targetHost}` : '');
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>:{port}{modeTag}</span>
      {byteRate && <span style={{ color: 'var(--udp)', opacity: 0.85 }}>{byteRate}</span>}
    </div>
  );
}

// ── UDP IN/OUT settings panel ────────────────────────────────────────────────
export function UdpDeviceSettingsPanel ({ nodeId, nodeType, port, mode, targetHost, multicastAddr, onClose }: {
  nodeId:         string;
  nodeType:       8 | 9;
  port:           number;
  mode:           0 | 1 | 2;
  targetHost:     string;
  multicastAddr:  string;
  onClose:        () => void;
}) {
  const isOut  = nodeType === 9;
  const title  = isOut ? 'UDP OUT Settings' : 'UDP IN Settings';
  const accent = 'var(--udp)';

  // Instant local state for display, explicit-action commit for the
  // backend — same pattern established for MQTT Subscribe/Publish (see
  // their locked decisions in Architecture.md for the full rationale).
  const [localPort, setLocalPort] = useState(port);
  const [localMode, setLocalMode] = useState(mode);
  const [localTargetHost, setLocalTargetHost] = useState(targetHost);
  const [localMulticastAddr, setLocalMulticastAddr] = useState(multicastAddr);

  useEffect(() => { setLocalPort(port); },                   [port]);
  useEffect(() => { setLocalMode(mode); },                   [mode]);
  useEffect(() => { setLocalTargetHost(targetHost); },       [targetHost]);
  useEffect(() => { setLocalMulticastAddr(multicastAddr); }, [multicastAddr]);

  const updateLocal = (next: { port?: number; mode?: 0 | 1 | 2; targetHost?: string; multicastAddr?: string }) => {
    if (next.port          !== undefined) setLocalPort(next.port);
    if (next.mode           !== undefined) setLocalMode(next.mode);
    if (next.targetHost     !== undefined) setLocalTargetHost(next.targetHost);
    if (next.multicastAddr  !== undefined) setLocalMulticastAddr(next.multicastAddr);
  };

  const commitNow = (overrides: { port?: number; mode?: 0 | 1 | 2; targetHost?: string; multicastAddr?: string } = {}) => {
    Bridge.setUdpSettings(
      nodeId,
      overrides.port          ?? localPort,
      overrides.mode          ?? localMode,
      overrides.targetHost    ?? localTargetHost,
      overrides.multicastAddr ?? localMulticastAddr,
    );
  };

  // Enter handlers call .blur() programmatically (to trigger the beep, or
  // just to leave the field after a commit) — but a programmatic .blur()
  // fires the same native blur event as a user click-away, which would
  // otherwise make the onBlur handler below fire commitNow() a second,
  // unintended time (redundant on a valid host, and on a malformed host it
  // would defeat the whole point of isLikelyCompleteHost by committing the
  // bad value anyway). This ref suppresses exactly one such blur.
  const suppressNextBlurRef = useRef(false);
  const blurSuppressed = (el: HTMLInputElement) => {
    suppressNextBlurRef.current = true;
    el.blur();
  };

  const commitOnEnter = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') { e.preventDefault(); commitNow(); blurSuppressed(e.target as HTMLInputElement); }
  };

  // Host-shaped fields (Target Host, Multicast Group) get their own Enter
  // handler: a malformed/incomplete address deliberately does NOT call
  // preventDefault(), and blurs-then-refocuses (rather than skipping blur
  // entirely) — this combination is what triggers the WebView's default
  // beep as intentional "not valid yet" feedback, confirmed empirically
  // via MQTT's settings panels. Re-focusing immediately means the user can
  // keep typing right away even though the beep fires.
  const commitHostOnEnter = (field: 'targetHost' | 'multicastAddr') => (e: React.KeyboardEvent<HTMLInputElement>) => {
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
    commitNow({ [field]: candidate });
    blurSuppressed(e.target as HTMLInputElement);
  };

  const commitOnBlur = (overrides: { port?: number; mode?: 0 | 1 | 2; targetHost?: string; multicastAddr?: string } = {}) => {
    if (suppressNextBlurRef.current) { suppressNextBlurRef.current = false; return; }
    commitNow(overrides);
  };

  const inputStyle: React.CSSProperties = {
    width: '100%', fontSize: 10, padding: '3px 6px', marginTop: 2,
    background: 'var(--surface)', border: '1px solid var(--border)',
    borderRadius: 3, color: 'var(--text-dim)',
    fontFamily: "'JetBrains Mono', monospace",
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
      <SettingsPanelHeader title={title} onReset={() => {
        updateLocal({ port: 0, mode: 0, targetHost: '', multicastAddr: '' });
        commitNow({ port: 0, mode: 0, targetHost: '', multicastAddr: '' });
      }} onClose={onClose} />

      <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
        Port
      </div>
      <input
        type="number" min={1} max={65535} value={localPort || ''}
        placeholder="e.g. 9000"
        onChange={e => updateLocal({ port: parseInt(e.target.value, 10) || 0 })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
        Mode
      </div>
      <div style={{ display: 'flex', gap: 4 }}>
        {(['Unicast', 'Multicast', 'Broadcast'] as const).map((label, i) => (
          <button key={label}
            onClick={() => { const m = i as 0 | 1 | 2; updateLocal({ mode: m }); commitNow({ mode: m }); }}
            style={{
              flex: 1, fontSize: 9, padding: '4px 2px', borderRadius: 3,
              border: '1px solid ' + (localMode === i ? accent : 'var(--border)'),
              background: localMode === i ? 'var(--surface)' : 'transparent',
              color: localMode === i ? accent : 'var(--text-muted)',
              cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            }}>
            {label}
          </button>
        ))}
      </div>

      {isOut && localMode === 0 && (
        <>
          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
            Target Host
          </div>
          <input
            type="text" value={localTargetHost} placeholder="192.168.1.50"
            autoCapitalize="off" autoCorrect="off" spellCheck={false}
            onChange={e => updateLocal({ targetHost: e.target.value })}
            onKeyDown={commitHostOnEnter('targetHost')}
            onBlur={() => commitOnBlur()}
            style={inputStyle}
          />
        </>
      )}

      {localMode === 1 && (
        <>
          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
            Multicast Group
          </div>
          <input
            type="text" value={localMulticastAddr} placeholder="239.0.0.1"
            autoCapitalize="off" autoCorrect="off" spellCheck={false}
            onChange={e => updateLocal({ multicastAddr: e.target.value })}
            onKeyDown={commitHostOnEnter('multicastAddr')}
            onBlur={() => commitOnBlur()}
            style={inputStyle}
          />
        </>
      )}

      {!localPort && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set a port to activate
        </div>
      )}
    </div>
  );
}

