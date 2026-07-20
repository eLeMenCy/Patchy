import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge } from './Bridge';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHandle, useNodeCollapsed, SettingsPanelHeader } from './NodeUtils';
import { Send } from 'lucide-react';
import { HintContext } from './HintPanel';

// ── Settings interface ────────────────────────────────────────────────────────
// Only a display name is configurable — topic/payload/history are working
// state, not settings, and live inline on the node face rather than behind
// a settings panel, since they're the primary interaction here (unlike
// Monitor nodes where settings are secondary to the log display).
export interface MqttConsoleSettings {
  customName: string;
  topicHistory: string[];   // remembered topics, most recent first
}

const DEFAULT_SETTINGS: MqttConsoleSettings = {
  customName: '',
  topicHistory: [],
};

const MAX_HISTORY = 20;

export interface MqttConsoleNodeData {
  label:    string;
  nodeType: 25;
  ports:    { id: string; label: string; type: string; direction: string }[];
  settings?: Partial<MqttConsoleSettings>;
  settingsJson?: string;
  [key: string]: unknown;
}

// ── Main component ────────────────────────────────────────────────────────────
export const MqttConsoleNode = memo(function MqttConsoleNode({ id, data, selected }: NodeProps) {
  const d = data as MqttConsoleNodeData;
  const [settings, setSettings] = useState<MqttConsoleSettings>({
    ...DEFAULT_SETTINGS, ...(d.settings ?? {}),
    ...(d.settingsJson ? JSON.parse(d.settingsJson) : {})
  });
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const { setHint } = useContext(HintContext);
  const portBodyRef = useRef<HTMLDivElement>(null);

  const [topic, setTopic] = useState('');
  const [payload, setPayload] = useState('');
  const [historyOpen, setHistoryOpen] = useState(false);
  const topicInputRef = useRef<HTMLInputElement>(null);

  useEffect(() => {
    const sj = (data as any)?.settingsJson;
    try { setSettings(s => ({ ...DEFAULT_SETTINGS, ...(sj ? JSON.parse(sj) : {}) })); } catch {}
  }, [(data as any)?.settingsJson]);

  const persistSettings = useCallback((next: MqttConsoleSettings) => {
    Bridge.commitSettingsChange(id, next);
  }, [id]);

  const commitName = useCallback((customName: string) => {
    setSettings(s => {
      const next = { ...s, customName };
      persistSettings(next);
      return next;
    });
    Bridge.setNodeLabel(id, customName);
  }, [id, persistSettings]);

  // Send: fires the one-shot backend trigger, adds the topic to history
  // (if new), and clears the payload field only — the topic stays, since
  // sending several payloads to the same topic in a row is the common case.
  const handleSend = useCallback(() => {
    const t = topic.trim();
    const p = parseFloat(payload);
    if (!t || isNaN(p)) return;

    Bridge.sendMqttConsole(id, t, p);

    setSettings(s => {
      if (s.topicHistory.includes(t)) return s;
      const next = { ...s, topicHistory: [t, ...s.topicHistory].slice(0, MAX_HISTORY) };
      persistSettings(next);
      return next;
    });

    setPayload('');
  }, [id, topic, payload, persistSettings]);

  const handleTopicKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') { handleSend(); setHistoryOpen(false); }
    if (e.key === 'Escape') setHistoryOpen(false);
  };
  const handlePayloadKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (e.key === 'Enter') handleSend();
  };

  const inputStyle: React.CSSProperties = {
    width: '100%', fontSize: 11, padding: '4px 8px',
    background: 'var(--surface)', border: '1px solid var(--border)',
    borderRadius: 3, color: 'var(--text)', outline: 'none',
    fontFamily: "'JetBrains Mono', monospace",
  };

  const canSend = topic.trim() !== '' && payload.trim() !== '' && !isNaN(parseFloat(payload));

  return (
    <div
      style={{
        width: 220,
        background: 'var(--surface)',
        border: `1px solid ${selected ? 'var(--mqtt)' : 'var(--border)'}`,
        borderTop: '3px solid var(--mqtt)',
        borderRadius: 'var(--radius)',
        boxShadow: selected ? '0 0 0 1px var(--mqtt), 0 8px 32px var(--mqtt-glow)' : '0 4px 16px rgba(0,0,0,.5)',
        position: 'relative',
        userSelect: 'none',
      }}>

      <NodeHandle nodeId={id} label="MQTT Out" direction="out" colour="var(--mqtt)" index={0} total={1} offset={-3} portBodyRef={portBodyRef} />

      <NodeHeader title={settings.customName || "MQTT CONSOLE"} accent="var(--mqtt)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed} />

      {!collapsed && (
        <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()}
             onPointerDown={e => e.stopPropagation()}
             style={{ padding: '8px 10px', position: 'relative' }}>

          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
                        textTransform: 'uppercase', marginBottom: 4 }}>
            Topic
          </div>
          <input
            ref={topicInputRef}
            type="text" value={topic} placeholder="e.g. patchy/console"
            autoCapitalize="off" autoCorrect="off" spellCheck={false}
            onChange={e => setTopic(e.target.value)}
            onFocus={() => setHistoryOpen(settings.topicHistory.length > 0)}
            onClick={() => setHistoryOpen(settings.topicHistory.length > 0)}
            onBlur={() => setTimeout(() => setHistoryOpen(false), 150)}
            onKeyDown={handleTopicKeyDown}
            style={inputStyle}
          />

          {historyOpen && settings.topicHistory.length > 0 && (
            <div style={{
              position: 'absolute', left: 10, right: 10, top: 44, zIndex: 1000,
              background: 'var(--surface2)', border: '1px solid var(--border-hi)',
              borderRadius: 3, boxShadow: '0 8px 24px rgba(0,0,0,.6)',
              maxHeight: 140, overflowY: 'auto',
            }}>
              {settings.topicHistory.map(t => (
                <div key={t}
                  // onMouseDown (not onClick) + preventDefault so this fires
                  // BEFORE the input's onBlur closes the list — otherwise the
                  // blur would hide the list before the click ever registers.
                  onMouseDown={e => { e.preventDefault(); setTopic(t); setHistoryOpen(false); topicInputRef.current?.focus(); }}
                  style={{
                    fontSize: 10, padding: '5px 8px', cursor: 'pointer',
                    color: 'var(--text-dim)', borderBottom: '1px solid var(--border)',
                    whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
                  }}
                  onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
                  onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
                >
                  {t}
                </div>
              ))}
            </div>
          )}

          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
                        textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
            Payload
          </div>
          <input
            type="number" value={payload} placeholder="e.g. 23.5"
            onChange={e => setPayload(e.target.value)}
            onKeyDown={handlePayloadKeyDown}
            style={inputStyle}
          />

          <button
            onClick={handleSend}
            disabled={!canSend}
            onMouseEnter={() => setHint({ title: 'Send', body: 'Publishes this topic + payload once. Connect to an MQTT Publish node to actually reach a broker.' })}
            onMouseLeave={() => setHint(null)}
            style={{
              width: '100%', marginTop: 10, padding: '6px 0',
              display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6,
              background: canSend ? 'var(--mqtt-dim)' : 'transparent',
              border: `1px solid ${canSend ? 'var(--mqtt)' : 'var(--border)'}`,
              borderRadius: 3, color: canSend ? 'var(--mqtt)' : 'var(--text-muted)',
              fontSize: 10, fontWeight: 700, letterSpacing: '0.08em',
              cursor: canSend ? 'pointer' : 'default',
              opacity: canSend ? 1 : 0.5,
            }}>
            <Send size={12} /> SEND
          </button>
        </div>
      )}

      {showSettings && (
        <div
          className="nodrag"
          onMouseDown={e => e.stopPropagation()}
          onPointerDown={e => e.stopPropagation()}
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
          <SettingsPanelHeader title="MQTT Console" onReset={() => commitName('')} onClose={() => closeSettings()} />
          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
                        textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
            Name
          </div>
          <input type="text" defaultValue={settings.customName} placeholder="MQTT Console"
            onBlur={e => commitName(e.target.value)}
            onKeyDown={e => { if (e.key === 'Enter') (e.target as HTMLInputElement).blur(); }}
            style={{ width: '100%', fontSize: 10, padding: '3px 6px',
                     background: 'var(--surface)', border: '1px solid var(--border)',
                     borderRadius: 3, color: 'var(--text-dim)', outline: 'none',
                     fontFamily: "'JetBrains Mono', monospace" }} />
        </div>
      )}
    </div>
  );
});
