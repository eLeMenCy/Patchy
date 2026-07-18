/**
 * MqttDeviceUI.tsx
 *
 * Settings panel + summary components for MQTT Subscribe/Publish device
 * nodes. Extracted from GenericNode.tsx (2026-07-19) — see AudioDeviceUI.tsx's
 * header comment for the full rationale and naming convention explanation.
 * Subscribe and Publish share one file since they're closely related
 * (mirror each other's topic/QoS/username/password shape).
 */

import { useState, useEffect, useRef } from 'react';
import { Bridge } from './Bridge';
import { Checkbox, SettingsPanelHeader, isLikelyCompleteHost } from './NodeUtils';
import { NodeSelect } from './NodeSelect';

// ── MQTT Subscribe settings panel ───────────────────────────────────────────
export function MqttSubscribeSettingsPanel ({ nodeId, host, port, topic, qos, username, password, onClose }: {
  nodeId:   string;
  host:     string;
  port:     number;
  topic:    string;
  qos:      0 | 1 | 2;
  username: string;
  password: string;
  onClose:  () => void;
}) {
  const accent = 'var(--mqtt)';

  // Local state, seeded from props, updated instantly on every keystroke —
  // this is what the inputs actually display, so typing feels normal.
  // The backend commit (which tears down and rebuilds a real TCP
  // connection, unlike UDP/OSC's cheap socket open) only fires on Enter or
  // blur, not on every keystroke or after a timer — typing an address
  // character-by-character should never itself attempt a connection.
  // Local state re-syncs from props if the node's settingsJson changes
  // from elsewhere (e.g. undo/redo).
  const [localHost, setLocalHost] = useState(host);
  const [localPort, setLocalPort] = useState(port);
  const [localTopic, setLocalTopic] = useState(topic);
  const [localQos, setLocalQos] = useState(qos);
  const [localUsername, setLocalUsername] = useState(username);
  const [localPassword, setLocalPassword] = useState(password);

  useEffect(() => { setLocalHost(host); },         [host]);
  useEffect(() => { setLocalPort(port); },         [port]);
  useEffect(() => { setLocalTopic(topic); },       [topic]);
  useEffect(() => { setLocalQos(qos); },           [qos]);
  useEffect(() => { setLocalUsername(username); }, [username]);
  useEffect(() => { setLocalPassword(password); }, [password]);

  // Pure local-state update — called on every keystroke, no backend call.
  const updateLocal = (next: { host?: string; port?: number; topic?: string; qos?: 0 | 1 | 2;
                               username?: string; password?: string }) => {
    if (next.host      !== undefined) setLocalHost(next.host);
    if (next.port      !== undefined) setLocalPort(next.port);
    if (next.topic     !== undefined) setLocalTopic(next.topic);
    if (next.qos       !== undefined) setLocalQos(next.qos);
    if (next.username  !== undefined) setLocalUsername(next.username);
    if (next.password  !== undefined) setLocalPassword(next.password);
  };

  // Fires the actual backend commit immediately, using current local state
  // plus any overrides. Called on Enter/blur for text fields, and directly
  // on change for QoS (an atomic complete-value change, no typing risk).
  const commitNow = (overrides: { host?: string; port?: number; topic?: string; qos?: 0 | 1 | 2;
                                  username?: string; password?: string } = {}) => {
    const resolvedHost = overrides.host ?? localHost;
    // Don't attempt a connection against a host that doesn't look complete
    // yet (e.g. "127." typed mid-IP) — mosquitto_connect_async()'s DNS
    // resolution step blocks the message thread, freezing the whole graph.
    if (!isLikelyCompleteHost(resolvedHost)) return;

    Bridge.setMqttSubscribeSettings(
      nodeId,
      resolvedHost,
      overrides.port     ?? localPort,
      overrides.topic    ?? localTopic,
      overrides.qos      ?? localQos,
      overrides.username ?? localUsername,
      overrides.password ?? localPassword,
    );
  };

  // See UdpDeviceSettingsPanel's comment for why programmatic blur() calls
  // need to suppress the separate onBlur handler from also firing.
  const suppressNextBlurRef = useRef(false);
  const blurSuppressed = (el: HTMLInputElement) => {
    suppressNextBlurRef.current = true;
    el.blur();
  };
  const commitOnBlur = (overrides: { host?: string; port?: number; topic?: string; qos?: 0 | 1 | 2;
                                     username?: string; password?: string } = {}) => {
    if (suppressNextBlurRef.current) { suppressNextBlurRef.current = false; return; }
    commitNow(overrides);
  };

  // Enter commits immediately; Escape reverts the field to the last
  // committed value (blurs so the user sees the reset take effect).
  const commitOnEnter = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') { e.preventDefault(); commitNow(); blurSuppressed(e.target as HTMLInputElement); }
  };

  // Host field gets its own Enter handler: a malformed/incomplete address
  // deliberately does NOT call preventDefault(), and blurs-then-refocuses
  // the field (rather than skipping blur entirely) — this combination is
  // what actually triggers the WebView's default beep as intentional
  // "not valid yet" feedback. Re-focusing immediately means the user can
  // keep typing right away even though the beep fires.
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
    commitNow();
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
        width: 220, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title="MQTT SUBSCRIBE Settings"
        onReset={() => { updateLocal({ host: '', port: 1883, topic: '', qos: 1, username: '', password: '' });
                         commitNow({ host: '', port: 1883, topic: '', qos: 1, username: '', password: '' }); }}
        onClose={onClose}
      />

      <div style={labelStyle}>Broker Host</div>
      <input
        type="text" value={localHost} placeholder="127.0.0.1"
        autoCapitalize="off" autoCorrect="off" spellCheck={false}
        onChange={e => updateLocal({ host: e.target.value })}
        onKeyDown={commitHostOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={labelStyle}>Broker Port</div>
      <input
        type="number" min={1} max={65535} value={localPort || ''}
        placeholder="1883"
        onChange={e => updateLocal({ port: parseInt(e.target.value, 10) || 1883 })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={labelStyle}>Topic</div>
      <input
        type="text" value={localTopic} placeholder="e.g. sensors/+/temperature"
        autoCapitalize="off" autoCorrect="off" spellCheck={false}
        onChange={e => updateLocal({ topic: e.target.value })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />
      <div style={{ fontSize: 9, color: accent, marginTop: 4, opacity: 0.7 }}>
        Wildcards: + (one level), # (all remaining)
      </div>

      <div style={labelStyle}>QoS</div>
      <NodeSelect
        value={String(localQos)}
        onChange={v => { const qos = parseInt(v, 10) as 0 | 1 | 2; updateLocal({ qos }); commitNow({ qos }); }}
        options={[
          { id: '0', name: '0 — At most once' },
          { id: '1', name: '1 — At least once' },
          { id: '2', name: '2 — Exactly once' },
        ]}
        accent={accent}
        showEmpty={false}
      />

      <div style={labelStyle}>Username (optional)</div>
      <input
        type="text" value={localUsername} placeholder=""
        autoCapitalize="off" autoCorrect="off" spellCheck={false}
        onChange={e => updateLocal({ username: e.target.value })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={labelStyle}>Password (optional)</div>
      <input
        type="password" value={localPassword} placeholder=""
        autoCapitalize="off" autoCorrect="off"
        onChange={e => updateLocal({ password: e.target.value })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      {(!localHost || !localTopic) && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set broker host and topic to activate
        </div>
      )}
    </div>
  );
}

// ── MQTT Subscribe summary label ────────────────────────────────────────────
export function MqttSubscribeSummary ({ host, port, topic, onClick }: {
  host:    string;
  port:    number;
  topic:   string;
  onClick: () => void;
}) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  if (!host || !topic) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        Set host and topic
      </div>
    );
  }
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>{host}:{port} · {topic}</span>
    </div>
  );
}

// ── MQTT Publish settings panel ─────────────────────────────────────────────
export function MqttPublishSettingsPanel ({ nodeId, host, port, topic, qos, retain, username, password, onClose }: {
  nodeId:   string;
  host:     string;
  port:     number;
  topic:    string;
  qos:      0 | 1 | 2;
  retain:   boolean;
  username: string;
  password: string;
  onClose:  () => void;
}) {
  const accent = 'var(--mqtt)';

  // Same instant-local-state / Enter-or-blur-commit split as
  // MqttSubscribeSettingsPanel — see that component's comments for why.
  const [localHost, setLocalHost] = useState(host);
  const [localPort, setLocalPort] = useState(port);
  const [localTopic, setLocalTopic] = useState(topic);
  const [localQos, setLocalQos] = useState(qos);
  const [localRetain, setLocalRetain] = useState(retain);
  const [localUsername, setLocalUsername] = useState(username);
  const [localPassword, setLocalPassword] = useState(password);

  useEffect(() => { setLocalHost(host); },         [host]);
  useEffect(() => { setLocalPort(port); },         [port]);
  useEffect(() => { setLocalTopic(topic); },       [topic]);
  useEffect(() => { setLocalQos(qos); },           [qos]);
  useEffect(() => { setLocalRetain(retain); },     [retain]);
  useEffect(() => { setLocalUsername(username); }, [username]);
  useEffect(() => { setLocalPassword(password); }, [password]);

  const updateLocal = (next: { host?: string; port?: number; topic?: string; qos?: 0 | 1 | 2;
                               retain?: boolean; username?: string; password?: string }) => {
    if (next.host      !== undefined) setLocalHost(next.host);
    if (next.port      !== undefined) setLocalPort(next.port);
    if (next.topic     !== undefined) setLocalTopic(next.topic);
    if (next.qos       !== undefined) setLocalQos(next.qos);
    if (next.retain    !== undefined) setLocalRetain(next.retain);
    if (next.username  !== undefined) setLocalUsername(next.username);
    if (next.password  !== undefined) setLocalPassword(next.password);
  };

  const commitNow = (overrides: { host?: string; port?: number; topic?: string; qos?: 0 | 1 | 2;
                                  retain?: boolean; username?: string; password?: string } = {}) => {
    const resolvedHost = overrides.host ?? localHost;
    // Same guard as MqttSubscribeSettingsPanel — see its comment for why.
    if (!isLikelyCompleteHost(resolvedHost)) return;

    Bridge.setMqttPublishSettings(
      nodeId,
      resolvedHost,
      overrides.port     ?? localPort,
      overrides.topic    ?? localTopic,
      overrides.qos      ?? localQos,
      overrides.retain   ?? localRetain,
      overrides.username ?? localUsername,
      overrides.password ?? localPassword,
    );
  };

  // See UdpDeviceSettingsPanel's comment for why programmatic blur() calls
  // need to suppress the separate onBlur handler from also firing.
  const suppressNextBlurRef = useRef(false);
  const blurSuppressed = (el: HTMLInputElement) => {
    suppressNextBlurRef.current = true;
    el.blur();
  };
  const commitOnBlur = (overrides: { host?: string; port?: number; topic?: string; qos?: 0 | 1 | 2;
                                     retain?: boolean; username?: string; password?: string } = {}) => {
    if (suppressNextBlurRef.current) { suppressNextBlurRef.current = false; return; }
    commitNow(overrides);
  };

  const commitOnEnter = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') { e.preventDefault(); commitNow(); blurSuppressed(e.target as HTMLInputElement); }
  };

  // Host field gets its own Enter handler — see MqttSubscribeSettingsPanel's
  // comment for why blur()-then-refocus triggers the beep as intentional
  // "not valid yet" feedback on a malformed address.
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
    commitNow();
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
        width: 220, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title="MQTT PUBLISH Settings"
        onReset={() => { updateLocal({ host: '', port: 1883, topic: '', qos: 1, retain: false, username: '', password: '' });
                         commitNow({ host: '', port: 1883, topic: '', qos: 1, retain: false, username: '', password: '' }); }}
        onClose={onClose}
      />

      <div style={labelStyle}>Broker Host</div>
      <input
        type="text" value={localHost} placeholder="127.0.0.1"
        autoCapitalize="off" autoCorrect="off" spellCheck={false}
        onChange={e => updateLocal({ host: e.target.value })}
        onKeyDown={commitHostOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={labelStyle}>Broker Port</div>
      <input
        type="number" min={1} max={65535} value={localPort || ''}
        placeholder="1883"
        onChange={e => updateLocal({ port: parseInt(e.target.value, 10) || 1883 })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={labelStyle}>Topic</div>
      <input
        type="text" value={localTopic} placeholder="e.g. sensors/kitchen/temp"
        autoCapitalize="off" autoCorrect="off" spellCheck={false}
        onChange={e => updateLocal({ topic: e.target.value })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />
      <div style={{ fontSize: 9, color: accent, marginTop: 4, opacity: 0.7 }}>
        Overridden per-message if the incoming value carries its own topic
      </div>

      <div style={labelStyle}>QoS</div>
      <NodeSelect
        value={String(localQos)}
        onChange={v => { const qos = parseInt(v, 10) as 0 | 1 | 2; updateLocal({ qos }); commitNow({ qos }); }}
        options={[
          { id: '0', name: '0 — At most once' },
          { id: '1', name: '1 — At least once' },
          { id: '2', name: '2 — Exactly once' },
        ]}
        accent={accent}
        showEmpty={false}
      />

      <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginTop: 8 }}>
        <Checkbox checked={localRetain} onChange={v => { updateLocal({ retain: v }); commitNow({ retain: v }); }} label="Retain" accent={accent} />
      </div>

      <div style={labelStyle}>Username (optional)</div>
      <input
        type="text" value={localUsername} placeholder=""
        autoCapitalize="off" autoCorrect="off" spellCheck={false}
        onChange={e => updateLocal({ username: e.target.value })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      <div style={labelStyle}>Password (optional)</div>
      <input
        type="password" value={localPassword} placeholder=""
        autoCapitalize="off" autoCorrect="off"
        onChange={e => updateLocal({ password: e.target.value })}
        onKeyDown={commitOnEnter}
        onBlur={() => commitOnBlur()}
        style={inputStyle}
      />

      {(!localHost || !localTopic) && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set broker host and topic to activate
        </div>
      )}
    </div>
  );
}

// ── MQTT Publish summary label ──────────────────────────────────────────────
export function MqttPublishSummary ({ host, port, topic, onClick }: {
  host:    string;
  port:    number;
  topic:   string;
  onClick: () => void;
}) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  if (!host || !topic) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        Set host and topic
      </div>
    );
  }
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>{host}:{port} · {topic}</span>
    </div>
  );
}

