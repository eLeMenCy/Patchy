import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, RawMqttMonitorEvent } from './Bridge';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHeaderButton, NodeHandle, Checkbox, useNodeCollapsed, SettingsPanelHeader } from './NodeUtils';
import { NodeSelect } from './NodeSelect';
import { Pause, Play, Trash2 } from 'lucide-react';
import { HintContext } from './HintPanel';

// ── Settings interface ────────────────────────────────────────────────────────
export interface MqttMonitorSettings {
  customName: string;
  timeFormat: 'wall' | 'delta';
  maxRows:    20 | 50 | 100 | 200;
  autoScroll: boolean;
  autoClearMs: 0 | 5000 | 10000 | 30000;
  paused: boolean;
}

const DEFAULT_SETTINGS: MqttMonitorSettings = {
  customName: '',
  timeFormat: 'wall',
  maxRows: 50,
  autoScroll: true,
  autoClearMs: 0,
  paused: false,
};

export interface MqttMonitorNodeData {
  label:    string;
  nodeType: 24;
  ports:    { id: string; label: string; type: string; direction: string }[];
  settings?: Partial<MqttMonitorSettings>;
  settingsJson?: string;
  [key: string]: unknown;
}

// ── Displayed row ─────────────────────────────────────────────────────────────
interface DisplayRow extends RawMqttMonitorEvent {
  key:     number;
  timeStr: string;
}

// MqttMonitorNode.tsx — the simplest of the three protocol event-log
// monitors (UDP/OSC/MQTT): just topic + payload, no per-protocol filter
// like OSC's addressFilter. Same scrolling-log shape, same module-level
// rowKey/prevTs pattern (its own separate pair) as the other two — see
// UdpMonitorNode.tsx for the full reasoning and the cross-instance
// "delta" time caveat this creates.
let rowKey = 0;
let prevTs = 0;

function buildRow(ev: RawMqttMonitorEvent, s: MqttMonitorSettings): DisplayRow {
  let timeStr = '';
  if (s.timeFormat === 'wall') {
    const d = new Date(ev.ts);
    timeStr = d.toTimeString().slice(0, 8) + '.' + String(d.getMilliseconds()).padStart(3, '0');
  } else {
    const delta = prevTs ? (ev.ts - prevTs) / 1000 : 0;
    timeStr = `+${delta.toFixed(3)}s`;
  }
  prevTs = ev.ts;
  return { ...ev, key: ++rowKey, timeStr };
}

// ── Settings panel ────────────────────────────────────────────────────────────
function SettingsPanel({ s, onChange, onDiscreteChange, onClose, onReset, onCommit }: {
  s: MqttMonitorSettings;
  onChange: (patch: Partial<MqttMonitorSettings>) => void;
  onDiscreteChange: (patch: Partial<MqttMonitorSettings>) => void;
  onClose: () => void;
  onReset: () => void;
  onCommit: () => void;
}) {
  const row = (label: string, children: React.ReactNode) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
      <div style={{ width: 90, fontSize: 10, color: 'var(--text-dim)', flexShrink: 0 }}>{label}</div>
      {children}
    </div>
  );

  const select = (key: keyof MqttMonitorSettings, options: {v: string; l: string}[]) => (
    <NodeSelect
      value={String(s[key])}
      onChange={v => {
        const coerced = typeof s[key] === 'number' ? Number(v) : v;
        onDiscreteChange({ [key]: coerced });
      }}
      options={options.map(o => ({ id: o.v, name: o.l }))}
      accent="var(--mqtt)"
      showEmpty={false}
    />
  );

  const section = (title: string) => (
    <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
                  textTransform: 'uppercase', marginTop: 10, marginBottom: 4,
                  borderTop: '1px solid var(--border)', paddingTop: 6 }}>
      {title}
    </div>
  );

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
        width: 260, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader title="MQTT Monitor" onReset={onReset} onClose={onClose} />

      {row('Name', (
        <input type="text" value={s.customName} placeholder="MQTT Monitor"
          onChange={e => onChange({ customName: e.target.value })}
          onBlur={onCommit}
          style={{ flex:1, background:'transparent', border:'1px solid var(--border)',
                   color:'var(--text)', fontSize:10, borderRadius:3,
                   padding:'2px 6px', outline:'none', width:'100%' }} />
      ))}

      {section('Format')}
      {row('Time', select('timeFormat', [{v:'wall',l:'Wall clock'},{v:'delta',l:'Delta'}]))}

      {section('Buffer')}
      {row('Max rows',   select('maxRows', [{v:'20',l:'20'},{v:'50',l:'50'},{v:'100',l:'100'},{v:'200',l:'200'}]))}
      {row('Auto-scroll', <Checkbox checked={s.autoScroll} onChange={v => onDiscreteChange({ autoScroll: v })} label="" accent="var(--mqtt)" />)}
      {row('Auto-clear', select('autoClearMs', [{v:'0',l:'Off'},{v:'5000',l:'5s'},{v:'10000',l:'10s'},{v:'30000',l:'30s'}]))}
    </div>
  );
}

// ── Main component ────────────────────────────────────────────────────────────
export const MqttMonitorNode = memo(function MqttMonitorNode({ id, data, selected }: NodeProps) {
  const d = data as MqttMonitorNodeData;
  // settingsJson wins on any overlapping key — it's spread second below.
  const [settings, setSettings] = useState<MqttMonitorSettings>({
    ...DEFAULT_SETTINGS, ...(d.settings ?? {}),
    ...(d.settingsJson ? JSON.parse(d.settingsJson) : {})
  });
  const [rows, setRows] = useState<DisplayRow[]>([]);
  const { showSettings, openSettings, closeSettings, toggleSettings } = useNodeSettings(id);
  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const { setHint } = useContext(HintContext);
  const portBodyRef = useRef<HTMLDivElement>(null);
  const tableRef    = useRef<HTMLDivElement>(null);
  const lastEventTs = useRef<number>(0);

  const settingsRef = useRef<MqttMonitorSettings>(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  useEffect(() => {
    const unsub = Bridge.onMqttMonitorEvents(batches => {
      const batch = batches.find(b => b.nodeId === id);
      if (!batch) return;

      const s = settingsRef.current;
      if (s.paused) return;

      const newRows = batch.events.map(ev => buildRow(ev, s));

      if (newRows.length > 0) {
        lastEventTs.current = Date.now();
        setRows(prev => [...prev, ...newRows].slice(-s.maxRows));
      }
    });
    return unsub;
  }, [id]);

  useEffect(() => {
    if (settings.autoClearMs === 0) return;
    const interval = setInterval(() => {
      if (lastEventTs.current > 0 && Date.now() - lastEventTs.current > settings.autoClearMs) {
        setRows([]);
        lastEventTs.current = 0;
      }
    }, 1000);
    return () => clearInterval(interval);
  }, [settings.autoClearMs]);

  useEffect(() => {
    if (settings.autoScroll && tableRef.current)
      tableRef.current.scrollTop = tableRef.current.scrollHeight;
  }, [rows, settings.autoScroll]);

  const patch = useCallback((patch: Partial<MqttMonitorSettings>) => {
    setSettings(s => {
      const next = { ...s, ...patch };
      Bridge.setNodeSettings(id, next);
      return next;
    });
    if ('customName' in patch)
      Bridge.setNodeLabel(id, patch.customName ?? '');
  }, [id]);

  const commitPatch = useCallback((p: Partial<MqttMonitorSettings>) => {
    setSettings(s => {
      const next = { ...s, ...p };
      Bridge.commitSettingsChange(id, next);
      return next;
    });
    if ('customName' in p)
      Bridge.setNodeLabel(id, p.customName ?? '');
  }, [id]);

  useEffect(() => {
    const sj = (data as any)?.settingsJson;
    try { setSettings(s => ({ ...DEFAULT_SETTINGS, ...(sj ? JSON.parse(sj) : {}) })); } catch {}
  }, [(data as any)?.settingsJson]);

  const cols = [
    { key: 'time',    label: 'TIME',    w: 90,  get: (r: DisplayRow) => r.timeStr },
    { key: 'topic',   label: 'TOPIC',   w: 160, get: (r: DisplayRow) => r.tp || '—' },
    { key: 'payload', label: 'PAYLOAD', w: 100, get: (r: DisplayRow) => r.pl || '—' },
  ];

  const cellStyle = (w: number): React.CSSProperties => ({
    width: w, minWidth: w, maxWidth: w,
    overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
    padding: '0 4px', fontSize: 10,
    borderRight: '1px solid var(--border)',
  });

  const totalW = Math.max(cols.reduce((s, c) => s + c.w, 0), 300);
  const frozenW = useRef(totalW);
  if (!showSettings) frozenW.current = totalW;
  const displayW = showSettings ? frozenW.current : totalW;

  return (
    <div
      style={{
        width: displayW + 2,
        background: 'var(--surface)',
        border: `1px solid ${selected ? 'var(--mqtt)' : 'var(--border)'}`,
        borderTop: '3px solid var(--mqtt)',
        borderRadius: 'var(--radius)',
        boxShadow: selected ? '0 0 0 1px var(--mqtt), 0 8px 32px var(--mqtt-glow)' : '0 4px 16px rgba(0,0,0,.5)',
        position: 'relative',
        userSelect: 'none',
      }}>

      <NodeHandle nodeId={id} label="MQTT In"  direction="in"  colour="var(--mqtt)" index={0} total={1} offset={-3} portBodyRef={portBodyRef} />
      <NodeHandle nodeId={id} label="MQTT Out" direction="out" colour="var(--mqtt)" index={0} total={1} offset={-3} portBodyRef={portBodyRef} />

      <NodeHeader title={settings.customName || "MQTT MONITOR"} accent="var(--mqtt)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}>
        <NodeHeaderButton onClick={() => patch({ paused: !settings.paused })}
          active={settings.paused} activeAccent="var(--mqtt)"
          onHint={{ onMouseEnter: () => setHint({ title: settings.paused ? 'Resume' : 'Pause', body: settings.paused ? 'Resume recording incoming MQTT messages.' : 'Pause recording. Existing messages remain visible.' }), onMouseLeave: () => setHint(null) }}>
          {settings.paused ? <Play size={14} /> : <Pause size={14} />}
        </NodeHeaderButton>
        <NodeHeaderButton onClick={() => { setRows([]); lastEventTs.current = 0; }}
          onHint={{ onMouseEnter: () => setHint({ title: 'Clear', body: 'Clears all MQTT messages from the monitor.' }), onMouseLeave: () => setHint(null) }}>
          <Trash2 size={14} />
        </NodeHeaderButton>
      </NodeHeader>

      {!collapsed && <>
      <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()}
           onPointerDown={e => e.stopPropagation()}>
      <div style={{ display: 'flex', background: 'var(--surface2)',
                    borderBottom: '1px solid var(--border)' }}>
        {cols.map(c => (
          <div key={c.key} style={{ ...cellStyle(c.w), fontSize: 9,
                                    color: 'var(--text-muted)', fontWeight: 700,
                                    letterSpacing: '0.08em', padding: '3px 4px' }}>
            {c.label}
          </div>
        ))}
      </div>

      <div ref={tableRef}
        style={{ height: 180, overflowY: 'auto', overflowX: 'hidden' }}>
        {rows.length === 0 ? (
          <div style={{ padding: '20px 8px', textAlign: 'center',
                        fontSize: 10, color: 'var(--text-muted)', opacity: 0.5 }}>
            waiting for MQTT…
          </div>
        ) : (
          rows.map((r, i) => (
            <div key={r.key}
              style={{ display: 'flex',
                       background: i === rows.length - 1 ? 'var(--mqtt-dim)' : i % 2 ? 'var(--surface2)' : 'transparent',
                       borderBottom: '1px solid var(--border)',
                       transition: 'background 0.3s' }}>
              {cols.map(c => {
                const val = c.get(r);
                return (
                  <div key={c.key} style={{ ...cellStyle(c.w), color: 'var(--text)',
                                            padding: '2px 4px',
                                            textAlign: val === '—' ? 'center' : 'left' }}
                    title={val}>
                    {val}
                  </div>
                );
              })}
            </div>
          ))
        )}
      </div>

      </div>

      {showSettings && (
        <div onMouseDown={e => e.stopPropagation()} onPointerDown={e => e.stopPropagation()}>
        <SettingsPanel s={settings} onChange={patch} onDiscreteChange={commitPatch}
          onClose={() => closeSettings()} onReset={() => { commitPatch(DEFAULT_SETTINGS); }} onCommit={() => Bridge.commitNodeSettings(id)} />
        </div>
      )}
      </>}
    </div>
  );
});
