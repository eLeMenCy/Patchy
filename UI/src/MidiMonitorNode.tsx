import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, RawMidiMonitorEvent } from './Bridge';
import { NodeSelect } from './NodeSelect';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHeaderButton, NodeHandle, Checkbox, useNodeCollapsed, SettingsPanelHeader } from './NodeUtils';
import { Pause, Play, Trash2 } from 'lucide-react';
import { HintContext } from './HintPanel';

// ── Settings interface ────────────────────────────────────────────────────────
export interface MonitorSettings {
  customName: string;
  // Format
  numberFormat:    'dec' | 'hex';
  noteOctave:      'yamaha' | 'roland';     // middle C = C3 or C4
  noteOffDisplay:  'noteOff' | 'noteOnV0';
  timeFormat:      'wall' | 'session' | 'delta' | 'transport';
  // Visible columns
  colTime:    boolean;
  colNode:    boolean;
  colDevice:  boolean;
  colCh:      boolean;
  colStt:     boolean;
  colNote:    boolean;
  colDt1:     boolean;
  colDt2:     boolean;
  colEvent:   boolean;
  // Channel filter (0 = all, 1-16 = specific)
  channels:   number[];   // empty = all channels
  // Event type filter
  filterNoteOn:      boolean;
  filterNoteOff:     boolean;
  filterCC:          boolean;
  filterPitchBend:   boolean;
  filterAftertouch:  boolean;
  filterPolyAT:      boolean;
  filterProgChange:  boolean;
  filterSysEx:       boolean;
  filterClock:       boolean;
  filterMTC:         boolean;
  filterActiveSense: boolean;
  filterReset:       boolean;
  // Buffer
  maxRows:     20 | 50 | 100 | 200;
  autoScroll:  boolean;
  autoClearMs: 0 | 5000 | 10000 | 30000;
  // State
  paused: boolean;
}

const DEFAULT_SETTINGS: MonitorSettings = {
  customName: '',
  numberFormat: 'dec', noteOctave: 'yamaha', noteOffDisplay: 'noteOff',
  timeFormat: 'wall',
  colTime: true, colNode: true, colDevice: true, colCh: true,
  colStt: true, colDt1: true, colDt2: true, colNote: true, colEvent: true,
  channels: [],
  filterNoteOn: true, filterNoteOff: true, filterCC: true,
  filterPitchBend: true, filterAftertouch: true, filterPolyAT: true,
  filterProgChange: true, filterSysEx: true, filterClock: true,
  filterMTC: true, filterActiveSense: true, filterReset: true,
  maxRows: 50, autoScroll: true, autoClearMs: 0,
  paused: false,
};

export interface MidiMonitorNodeData {
  label:    string;
  nodeType: 5;
  ports:    { id: string; label: string; type: string; direction: string }[];
  settings?: Partial<MonitorSettings>;
  settingsJson?: string;
  [key: string]: unknown;
}

// ── MIDI helpers ──────────────────────────────────────────────────────────────
function noteName(midiNote: number, octaveConvention: 'yamaha' | 'roland'): string {
  const names = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
  const offset = octaveConvention === 'yamaha' ? -1 : 0;
  return names[midiNote % 12] + (Math.floor(midiNote / 12) + offset);
}

function eventName(status: number, d1: number, d2: number,
                   s: MonitorSettings): { name: string; isNote: boolean } {
  const type = status & 0xF0;
  switch (type) {
    case 0x80: return { name: 'Note Off', isNote: true };
    case 0x90:
      if (d2 === 0 && s.noteOffDisplay === 'noteOff') return { name: 'Note Off', isNote: true };
      return { name: 'Note On', isNote: true };
    case 0xA0: return { name: 'Poly AT', isNote: false };
    case 0xB0: return { name: 'CC', isNote: false };
    case 0xC0: return { name: 'Prog Change', isNote: false };
    case 0xD0: return { name: 'Aftertouch', isNote: false };
    case 0xE0: return { name: 'Pitch Bend', isNote: false };
    case 0xF0:
      if (status === 0xF0) return { name: 'SysEx', isNote: false };
      if (status === 0xF8) return { name: 'Clock', isNote: false };
      if (status === 0xF1) return { name: 'MTC', isNote: false };
      if (status === 0xFE) return { name: 'Active Sense', isNote: false };
      if (status === 0xFF) return { name: 'Reset', isNote: false };
      return { name: 'System', isNote: false };
    default: return { name: `0x${status.toString(16).toUpperCase()}`, isNote: false };
  }
}

function passesFilter(ev: RawMidiMonitorEvent, s: MonitorSettings): boolean {
  const ch = (ev.st & 0x0F) + 1;
  if (s.channels.length > 0 && !s.channels.includes(ch)) return false;
  const type = ev.st & 0xF0;
  const isNoteOff = type === 0x80 || (type === 0x90 && ev.d2 === 0);
  if (type === 0x90 && !isNoteOff && !s.filterNoteOn)  return false;
  if (isNoteOff                   && !s.filterNoteOff) return false;
  if (type === 0xB0 && !s.filterCC)          return false;
  if (type === 0xE0 && !s.filterPitchBend)   return false;
  if (type === 0xD0 && !s.filterAftertouch)  return false;
  if (type === 0xA0 && !s.filterPolyAT)      return false;
  if (type === 0xC0 && !s.filterProgChange)  return false;
  if (ev.st === 0xF0 && !s.filterSysEx)      return false;
  if (ev.st === 0xF8 && !s.filterClock)      return false;
  if (ev.st === 0xF1 && !s.filterMTC)        return false;
  if (ev.st === 0xFE && !s.filterActiveSense)return false;
  if (ev.st === 0xFF && !s.filterReset)      return false;
  return true;
}

function fmt(n: number, format: 'dec' | 'hex', pad = 0): string {
  if (format === 'hex') {
    const h = n.toString(16).toUpperCase();
    return pad ? h.padStart(pad, '0') : h;
  }
  return String(n);
}

// ── Displayed row ─────────────────────────────────────────────────────────────
interface DisplayRow extends RawMidiMonitorEvent {
  key:       number;
  timeStr:   string;
  chStr:     string;
  sttStr:    string;
  d1Str:     string;
  d2Str:     string;
  noteStr:   string;
  eventStr:  string;
}

let rowKey = 0;
let sessionStartMs = 0;
let prevTs = 0;

function buildRow(ev: RawMidiMonitorEvent, s: MonitorSettings): DisplayRow {
  if (sessionStartMs === 0) sessionStartMs = ev.ts;

  const ch = (ev.st & 0x0F) + 1;
  const type = ev.st & 0xF0;
  const isNote = type === 0x90 || type === 0x80;
  const { name: evName } = eventName(ev.st, ev.d1, ev.d2, s);

  // Time string
  let timeStr = '';
  switch (s.timeFormat) {
    case 'wall': {
      const d = new Date(ev.ts);
      timeStr = d.toTimeString().slice(0, 8) + '.' +
                String(d.getMilliseconds()).padStart(3, '0');
      break;
    }
    case 'session': {
      const diff = ev.ts - sessionStartMs;
      const mm = Math.floor(diff / 60000);
      const ss = ((diff % 60000) / 1000).toFixed(3).padStart(6, '0');
      timeStr = `${mm}:${ss}`;
      break;
    }
    case 'delta': {
      const delta = prevTs ? (ev.ts - prevTs) / 1000 : 0;
      timeStr = `+${delta.toFixed(3)}s`;
      break;
    }
    case 'transport':
      timeStr = `${ev.ts}`;
      break;
  }
  prevTs = ev.ts;

  return {
    ...ev,
    key:      ++rowKey,
    timeStr,
    chStr:    String(ch),
    sttStr:   fmt(ev.st, s.numberFormat, s.numberFormat === 'hex' ? 2 : 0),
    d1Str:    fmt(ev.d1, s.numberFormat),
    d2Str:    fmt(ev.d2, s.numberFormat),
    noteStr:  isNote ? noteName(ev.d1, s.noteOctave) : '',
    eventStr: evName,
  };
}

// ── Settings panel ────────────────────────────────────────────────────────────
function SettingsPanel({ s, onChange, onDiscreteChange, onClose, onReset, onCommit }: {
  s: MonitorSettings;
  onChange: (patch: Partial<MonitorSettings>) => void;
  onDiscreteChange: (patch: Partial<MonitorSettings>) => void;
  onClose: () => void;
  onReset: () => void;
  onCommit: () => void;
}) {
  const row = (label: string, children: React.ReactNode) => (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
      <div style={{ width: 110, fontSize: 10, color: 'var(--text-dim)', flexShrink: 0 }}>{label}</div>
      {children}
    </div>
  );

  const toggle = (key: keyof MonitorSettings, label: string) => (
    <Checkbox checked={s[key] as boolean}
      onChange={v => onDiscreteChange({ [key]: v })}
      label={label} accent="var(--midi)" />
  );

  const select = (key: keyof MonitorSettings, options: {v: string; l: string}[]) => (
    <NodeSelect
      value={String(s[key])}
      onChange={v => {
        const coerced = typeof s[key] === 'number' ? Number(v) : v;
        onDiscreteChange({ [key]: coerced });
      }}
      options={options.map(o => ({ id: o.v, name: o.l }))}
      accent="var(--midi)"
      showEmpty={false}
    />
  );

  const chBtn = (ch: number) => {
    const active = s.channels.includes(ch);
    return (
      <div key={ch} onClick={() => {
          const next = active
            ? s.channels.filter(c => c !== ch)
            : [...s.channels, ch].sort((a, b) => a - b);
          onDiscreteChange({ channels: next });
        }}
        style={{
          width: 22, height: 18, display: 'flex', alignItems: 'center',
          justifyContent: 'center', fontSize: 9, borderRadius: 3, cursor: 'pointer',
          background: active ? 'var(--midi)' : 'var(--surface)',
          color: active ? '#000' : 'var(--text-muted)',
          border: `1px solid ${active ? 'var(--midi)' : 'var(--border)'}`,
          userSelect: 'none',
        }}>
        {ch}
      </div>
    );
  };

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
        width: 280, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader title="MIDI Monitor" onReset={onReset} onClose={onClose} />

      {row('Name', (
        <input type="text" value={s.customName} placeholder="MIDI Monitor"
          onChange={e => onChange({ customName: e.target.value })}
          onBlur={onCommit}
          style={{ flex:1, background:'transparent', border:'1px solid var(--border)',
                   color:'var(--text)', fontSize:10, borderRadius:3,
                   padding:'2px 6px', outline:'none', width:'100%' }} />
      ))}
      {section('Format')}
      {row('Numbers',    select('numberFormat',   [{v:'dec',l:'Decimal'},{v:'hex',l:'Hex'}]))}
      {row('Note C3/C4', select('noteOctave',     [{v:'yamaha',l:'Yamaha (C3)'},{v:'roland',l:'Roland (C4)'}]))}
      {row('Note Off',   select('noteOffDisplay', [{v:'noteOff',l:'Show as Note Off'},{v:'noteOnV0',l:'Note On vel=0'}]))}
      {row('Time',       select('timeFormat',     [{v:'wall',l:'Wall clock'},{v:'session',l:'Session'},{v:'delta',l:'Delta'},{v:'transport',l:'Transport'}]))}

      {section('Visible Columns')}
      <div className="nodrag" onPointerDown={e => e.stopPropagation()} style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
        {toggle('colTime',   'TIME')}
        {toggle('colNode',   'NODE')}
        {toggle('colDevice', 'NAME')}
        {toggle('colCh',     'CH')}
        {toggle('colStt',    'STT')}
        {toggle('colDt1',    'DT1')}
        {toggle('colDt2',    'DT2')}
        {toggle('colNote',   'NOTE')}
        {toggle('colEvent',  'EVENT')}
      </div>

      {section('Channel Filter')}
      <div style={{ fontSize: 9, color: 'var(--text-muted)', marginBottom: 4 }}>
        All channels when none selected
      </div>
      <div style={{ display: 'flex', flexWrap: 'wrap', gap: 3 }}>
        {Array.from({length: 16}, (_, i) => chBtn(i + 1))}
      </div>

      {section('Event Filter')}
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 4 }}>
        {toggle('filterNoteOn',      'Note On')}
        {toggle('filterNoteOff',     'Note Off')}
        {toggle('filterCC',          'CC')}
        {toggle('filterPitchBend',   'Pitch Bend')}
        {toggle('filterAftertouch',  'Aftertouch')}
        {toggle('filterPolyAT',      'Poly AT')}
        {toggle('filterProgChange',  'Prog Change')}
        {toggle('filterSysEx',       'SysEx')}
        {toggle('filterClock',       'Clock')}
        {toggle('filterMTC',         'MTC')}
        {toggle('filterActiveSense', 'Active Sense')}
        {toggle('filterReset',       'Reset')}
      </div>

      {section('Buffer')}
      {row('Max rows',   select('maxRows', [{v:'20',l:'20'},{v:'50',l:'50'},{v:'100',l:'100'},{v:'200',l:'200'}]))}
      {row('Auto-scroll',toggle('autoScroll', ''))}
      {row('Auto-clear', select('autoClearMs', [{v:'0',l:'Off'},{v:'5000',l:'5s'},{v:'10000',l:'10s'},{v:'30000',l:'30s'}]))}
    </div>
  );
}

// ── Main component ────────────────────────────────────────────────────────────
function MidiMonitorNode({ id, data, selected }: NodeProps) {
  const d = data as MidiMonitorNodeData;
  const [settings, setSettings] = useState<MonitorSettings>({
    ...DEFAULT_SETTINGS, ...(d.settings ?? {}),
    ...(d.settingsJson ? JSON.parse(d.settingsJson) : {})
  });
  const [rows,        setRows]        = useState<DisplayRow[]>([]);
  const { showSettings, openSettings, closeSettings, toggleSettings } = useNodeSettings(id);
  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const { setHint } = useContext(HintContext);
  const portBodyRef = useRef<HTMLDivElement>(null);
  const tableRef    = useRef<HTMLDivElement>(null);
  const lastEventTs = useRef<number>(0);

  // Keep a ref to settings so the subscription callback always has fresh values
  // without needing to re-subscribe every time settings change.
  const settingsRef = useRef<MonitorSettings>(settings);
  useEffect(() => { settingsRef.current = settings; }, [settings]);

  // Subscribe once per node id — unsubscribe on unmount to prevent duplicates
  useEffect(() => {
    const unsub = Bridge.onMidiMonitorEvents(batches => {
      const batch = batches.find(b => b.nodeId === id);
      if (!batch) return;

      const s = settingsRef.current;
      if (s.paused) return;

      const newRows = batch.events
        .filter(ev => passesFilter(ev, s))
        .map(ev => buildRow(ev, s));

      if (newRows.length > 0) {
        lastEventTs.current = Date.now();
        setRows(prev => [...prev, ...newRows].slice(-s.maxRows));
      }
    });
    return unsub;
  }, [id]);  // only re-subscribe if node id changes

  // Auto-clear after silence
  useEffect(() => {
    if (settings.autoClearMs === 0) return;
    const interval = setInterval(() => {
      if (lastEventTs.current > 0 &&
          Date.now() - lastEventTs.current > settings.autoClearMs) {
        setRows([]);
        lastEventTs.current = 0;
      }
    }, 1000);
    return () => clearInterval(interval);
  }, [settings.autoClearMs]);

  // Auto-scroll to bottom — newest events are appended at bottom
  useEffect(() => {
    if (settings.autoScroll && tableRef.current)
      tableRef.current.scrollTop = tableRef.current.scrollHeight;
  }, [rows, settings.autoScroll]);

  const patch = useCallback((patch: Partial<MonitorSettings>) => {
    setSettings(s => {
      const next = { ...s, ...patch };
      Bridge.setNodeSettings(id, next);
      return next;
    });
    if ('customName' in patch)
      Bridge.setNodeLabel(id, patch.customName ?? '');
  }, [id]);

  // For discrete controls — atomic set+commit in one step
  const commitPatch = useCallback((p: Partial<MonitorSettings>) => {
    setSettings(s => {
      const next = { ...s, ...p };
      Bridge.commitSettingsChange(id, next);
      return next;
    });
    if ('customName' in p)
      Bridge.setNodeLabel(id, p.customName ?? '');
  }, [id]);

  // Restore settings from settingsJson on undo/redo
  useEffect(() => {
    const sj = (data as any)?.settingsJson;
    try { setSettings(s => ({ ...DEFAULT_SETTINGS, ...(sj ? JSON.parse(sj) : {}) })); } catch {}
  }, [(data as any)?.settingsJson]);



  const cols = [
    { key: 'colTime',   label: 'TIME',   w: 90,  get: (r: DisplayRow) => r.timeStr },
    { key: 'colNode',   label: 'NODE',   w: 80,  get: (r: DisplayRow) => r.sn  ? r.sn  : '—' },
    { key: 'colDevice', label: 'NAME',   w: 100, get: (r: DisplayRow) => r.sd  ? r.sd  : '—' },
    { key: 'colCh',     label: 'CH',     w: 30,  get: (r: DisplayRow) => r.chStr },
    { key: 'colStt',    label: 'STT',    w: 38,  get: (r: DisplayRow) => r.sttStr },
    { key: 'colDt1',    label: 'DT1',    w: 35,  get: (r: DisplayRow) => r.d1Str },
    { key: 'colDt2',    label: 'DT2',    w: 35,  get: (r: DisplayRow) => r.d2Str },
    { key: 'colNote',   label: 'NOTE',   w: 40,  get: (r: DisplayRow) => r.noteStr || '—' },
    { key: 'colEvent',  label: 'EVENT',  w: 90,  get: (r: DisplayRow) => r.eventStr },
  ].filter(c => settings[c.key as keyof MonitorSettings]);

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
        border: `1px solid ${selected ? 'var(--midi)' : 'var(--border)'}`,
        borderTop: '3px solid var(--midi)',
        borderRadius: 'var(--radius)',
        boxShadow: selected ? '0 0 0 1px var(--midi), 0 8px 32px var(--midi-glow)' : '0 4px 16px rgba(0,0,0,.5)',
        position: 'relative',
        userSelect: 'none',
      }}>

      {/* MIDI In handle */}
      <NodeHandle nodeId={id} label="MIDI In"  direction="in"  colour="var(--midi)" index={0} total={1} offset={-3} portBodyRef={portBodyRef} />

      {/* MIDI Out handle */}
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour="var(--midi)" index={0} total={1} offset={-3} portBodyRef={portBodyRef} />

      {/* Header */}
      <NodeHeader title={settings.customName || "MIDI MONITOR"} accent="var(--midi)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}>
        <NodeHeaderButton onClick={() => patch({ paused: !settings.paused })}
          active={settings.paused} activeAccent="var(--midi)"
          onHint={{ onMouseEnter: () => setHint({ title: settings.paused ? 'Resume' : 'Pause', body: settings.paused ? 'Resume recording incoming MIDI events.' : 'Pause recording. Existing events remain visible.' }), onMouseLeave: () => setHint(null) }}>
          {settings.paused ? <Play size={14} /> : <Pause size={14} />}
        </NodeHeaderButton>
        <NodeHeaderButton onClick={() => { setRows([]); lastEventTs.current = 0; }}
          onHint={{ onMouseEnter: () => setHint({ title: 'Clear', body: 'Clears all MIDI events from the monitor.' }), onMouseLeave: () => setHint(null) }}>
          <Trash2 size={14} />
        </NodeHeaderButton>
      </NodeHeader>

      {/* Collapsible content */}
      {!collapsed && <>
      {/* Column headers + table — nodrag so these areas don't initiate node drag */}
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

      {/* Event rows */}
      <div ref={tableRef}
        style={{ height: 180, overflowY: 'auto', overflowX: 'hidden' }}>
        {rows.length === 0 ? (
          <div style={{ padding: '20px 8px', textAlign: 'center',
                        fontSize: 10, color: 'var(--text-muted)', opacity: 0.5 }}>
            waiting for MIDI…
          </div>
        ) : (
          rows.map((r, i) => (
            <div key={r.key}
              style={{ display: 'flex',
                       background: i === rows.length - 1 ? 'var(--midi-dim)' : i % 2 ? 'var(--surface2)' : 'transparent',
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

      </div>{/* end nodrag table wrapper */}

      {/* Settings overlay */}
      {showSettings && (
        <div onMouseDown={e => e.stopPropagation()} onPointerDown={e => e.stopPropagation()}>
        <SettingsPanel s={settings} onChange={patch} onDiscreteChange={commitPatch}
          onClose={() => closeSettings()} onReset={() => { commitPatch(DEFAULT_SETTINGS); }} onCommit={() => Bridge.commitNodeSettings(id)} />
        </div>
      )}
      </>}
    </div>
  );
}

export default memo(MidiMonitorNode);
