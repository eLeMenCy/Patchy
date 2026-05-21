import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, AudioSnapshot } from './Bridge';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHeaderButton, NodeHandle, Checkbox, useNodeCollapsed, SettingsPanelHeader } from './NodeUtils';
import { Pause, Play } from 'lucide-react';
import { HintContext } from './HintPanel';
import { NodeSelect } from './NodeSelect';

// ── Settings ──────────────────────────────────────────────────────────────────
export interface AudioMonitorSettings {
  customName: string;
  channelMode:    'L' | 'R' | 'overlay' | 'stacked';
  triggerMode:    'free' | 'rising' | 'falling';
  triggerLevel:   number;   // -1.0 to 1.0
  amplitudeMode:  'auto' | 'fixed';
  amplitudeZoom:  number;   // 0.1 to 4.0
  timeWindowMs:   number;   // 10 | 25 | 50 | 100 | 200
  showClipping:    boolean;
  clipThresholdDb: number;   // dBFS threshold, 0 = true clip, -3 = headroom warning etc.
  paused:         boolean;
}

const DEFAULT_SETTINGS: AudioMonitorSettings = {
  customName: '',
  channelMode:   'overlay',
  triggerMode:   'free',
  triggerLevel:  0,
  amplitudeMode: 'fixed',
  amplitudeZoom: 1.0,
  timeWindowMs:  50,
  showClipping:    true,
  clipThresholdDb: 0.0,   // 0 dBFS = true digital clip
  paused:        false,
};

export interface AudioMonitorNodeData {
  label:    string;
  nodeType: 6;
  ports:    { id: string; label: string; type: 'midi' | 'audio'; direction: 'input' | 'output' }[];
  settings?: Partial<AudioMonitorSettings>;
  settingsJson?: string;
  [key: string]: unknown;
}

// ── Colours ───────────────────────────────────────────────────────────────────
const COL_L       = '#4FC3F7';   // light blue — left channel
const COL_R       = '#81C784';   // light green — right channel
const COL_CLIP    = '#EF5350';   // red — clipping
const COL_GRID    = 'rgba(255,255,255,0.06)';
const COL_ZERO    = 'rgba(255,255,255,0.15)';
const COL_TRIGGER = 'rgba(255,200,50,0.5)';
const BG          = 'rgba(13, 15, 20, 0.52)';   // matches --bg with transparency

// ── Trigger search ────────────────────────────────────────────────────────────
function findTriggerOffset (
  samples: Float32Array, mode: 'rising' | 'falling', level: number
): number {
  const half = Math.floor (samples.length / 2);
  for (let i = 1; i < half; i++) {
    if (mode === 'rising'  && samples[i - 1] < level && samples[i] >= level) return i;
    if (mode === 'falling' && samples[i - 1] > level && samples[i] <= level) return i;
  }
  return 0;
}

// ── Decode integer-encoded samples (value × 1000) from Bridge string ──────────
function parseFloats (str: string): Float32Array {
  try {
    if (!str || str.length === 0) return new Float32Array(0);
    const parts = str.split(',');
    const arr   = new Float32Array(parts.length);
    for (let i = 0; i < parts.length; i++) {
      const v = parseInt(parts[i], 10);
      arr[i] = isNaN(v) ? 0 : v / 1000.0;
    }
    return arr;
  } catch { return new Float32Array(0); }
}

// ── Draw waveform onto canvas ─────────────────────────────────────────────────
function drawWaveform (
  ctx:    CanvasRenderingContext2D,
  w:      number,
  h:      number,
  left:   Float32Array,
  right:  Float32Array,
  s:      AudioMonitorSettings,
  sr:     number
) {
  ctx.clearRect (0, 0, w, h);   // clear to transparent
  ctx.fillStyle = BG;
  ctx.fillRect (0, 0, w, h);

  // Grid
  ctx.strokeStyle = COL_GRID;
  ctx.lineWidth   = 0.5;
  for (let i = 1; i < 4; i++) {
    const y = (h / 4) * i;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }
  for (let i = 1; i < 8; i++) {
    const x = (w / 8) * i;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  }

  // Zero line
  ctx.strokeStyle = COL_ZERO;
  ctx.lineWidth   = 0.8;
  const drawChannel = (samples: Float32Array, colour: string, yOff: number, hh: number) => {
    if (samples.length === 0) return;

    // Buffer always holds 200ms of audio downsampled to 512 display points.
    // Select the proportional slice for the requested time window.
    const MAX_WINDOW_MS = 200;
    const fraction      = Math.min (s.timeWindowMs, MAX_WINDOW_MS) / MAX_WINDOW_MS;
    const count         = Math.max (4, Math.floor (samples.length * fraction));

    // Find trigger offset
    let offset = 0;
    if (s.triggerMode !== 'free')
      offset = findTriggerOffset (samples, s.triggerMode, s.triggerLevel);

    const zoom = s.amplitudeMode === 'auto'
      ? (() => {
          let peak = 0.001;
          for (let i = 0; i < count; i++) peak = Math.max(peak, Math.abs(samples[(offset + i) % samples.length]));
          return 1 / peak;
        })()
      : s.amplitudeZoom;

    // Zero line
    ctx.strokeStyle = COL_ZERO;
    ctx.lineWidth   = 0.5;
    const mid = yOff + hh / 2;
    ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke();

    // Trigger level line
    if (s.triggerMode !== 'free') {
      const ty = mid - s.triggerLevel * zoom * (hh / 2);
      ctx.strokeStyle = COL_TRIGGER;
      ctx.lineWidth   = 0.8;
      ctx.setLineDash([4, 4]);
      ctx.beginPath(); ctx.moveTo(0, ty); ctx.lineTo(w, ty); ctx.stroke();
      ctx.setLineDash([]);
    }

    // Waveform
    ctx.beginPath();
    ctx.strokeStyle = colour;
    ctx.lineWidth   = 1.2;
    let hasClip = false;

    for (let i = 0; i < count; i++) {
      const idx = (offset + i) % samples.length;
      const v   = samples[idx];
      const x   = (i / count) * w;
      const y   = mid - v * zoom * (hh / 2);
      i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      const clipLin = Math.pow(10, s.clipThresholdDb / 20);
      if (s.showClipping && Math.abs(v) >= clipLin) hasClip = true;
    }
    ctx.stroke();

    // Clip flash — red overlay + red border on the affected channel area
    if (hasClip && s.showClipping) {
      ctx.fillStyle = `${COL_CLIP}55`;
      ctx.fillRect(0, yOff, w, hh);
      ctx.strokeStyle = COL_CLIP;
      ctx.lineWidth   = 1.5;
      ctx.strokeRect(1, yOff + 1, w - 2, hh - 2);
    }
  };

  const stacked = s.channelMode === 'stacked';
  const hh      = stacked ? h / 2 : h;

  if (s.channelMode === 'L' || s.channelMode === 'overlay')
    drawChannel(left,  COL_L, 0,           stacked ? hh : h);
  if (s.channelMode === 'R' || s.channelMode === 'overlay')
    drawChannel(right, COL_R, 0,           stacked ? hh : h);
  if (s.channelMode === 'stacked') {
    drawChannel(left,  COL_L, 0,           hh);
    // Divider
    ctx.strokeStyle = 'rgba(255,255,255,0.12)';
    ctx.lineWidth   = 1;
    ctx.beginPath(); ctx.moveTo(0, hh); ctx.lineTo(w, hh); ctx.stroke();
    drawChannel(right, COL_R, hh,          hh);
  }
}

// ── Settings panel ────────────────────────────────────────────────────────────
function SettingsPanel ({ s, onChange, onClose, onReset }: {
  s: AudioMonitorSettings;
  onChange: (p: Partial<AudioMonitorSettings>) => void;
  onClose: () => void;
  onReset: () => void;
}) {
  const row = (label: string, child: React.ReactNode) => (
    <div style={{ display:'flex', alignItems:'center', gap:8, marginBottom:6 }}>
      <div style={{ width:90, fontSize:10, color:'var(--text-dim)', flexShrink:0 }}>{label}</div>
      <div style={{ flex:1, minWidth:0, display:'flex', alignItems:'center', overflow:'visible' }}>{child}</div>
    </div>
  );

  const sel = (key: keyof AudioMonitorSettings, opts: {v:string;l:string}[]) => (
    <div style={{ flex: 1 }}>
      <NodeSelect
        value={String(s[key])}
        onChange={v => {
          // Coerce back to number if the current value is numeric
          const coerced = typeof s[key] === 'number' ? Number(v) : v;
          onChange({ [key]: coerced });
        }}
        options={opts.map(o => ({ id: o.v, name: o.l }))}
        accent="var(--audio)"
        showEmpty={false}
      />
    </div>
  );

  const { setHint: setSliderHint } = useContext(HintContext);
  const slider = (key: keyof AudioMonitorSettings, min:number, max:number, step:number, fmt:(v:number)=>string, label?:string) => (
    <>
      <span style={{ fontSize:10, color:'var(--text-dim)', width:36, textAlign:'right',
                     flexShrink:0, fontVariantNumeric:'tabular-nums' }}>
        {fmt(s[key] as number)}
      </span>
      <input type="range" min={min} max={max} step={step}
        value={s[key] as number}
        onChange={e => onChange({ [key]: parseFloat(e.target.value) })}
        onMouseEnter={() => label && setSliderHint({ title: label, body: `Range: ${min} to ${max}. Double-click to reset.` })}
        onMouseLeave={() => setSliderHint(null)}
        style={{ flex:1, minWidth:0, maxWidth:120 }} />
    </>
  );

  const tog = (key: keyof AudioMonitorSettings, label: string) => (
    <label style={{ display:'flex', alignItems:'center', gap:4,
                    fontSize:10, color:'var(--text-dim)', cursor:'pointer' }}>
      <Checkbox checked={s[key] as boolean}
             onChange={v => onChange({ [key]: v })} accent="var(--audio)" />
      {label}
    </label>
  );

  const section = (t: string) => (
    <div style={{ fontSize:9, color:'var(--text-muted)', letterSpacing:'0.1em',
                  textTransform:'uppercase', marginTop:10, marginBottom:4,
                  borderTop:'1px solid var(--border)', paddingTop:6 }}>
      {t}
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
        position:'absolute', top:0, left:'100%', marginLeft:6,
        width:260, background:'var(--surface2)',
        border:'1px solid var(--border-hi)', borderRadius:'var(--radius)',
        padding:'10px 12px', zIndex:1000,
        boxShadow:'0 8px 32px rgba(0,0,0,.6)',
        fontFamily:"'JetBrains Mono', monospace",
        userSelect: 'none',
        overflow: 'visible',
      }}>
      <SettingsPanelHeader title="Audio Monitor" onReset={onReset} onClose={onClose} />

      {section('Display')}
      {row('Name', (
        <input type="text" value={s.customName} placeholder="Audio Monitor"
          onChange={e => onChange({ customName: e.target.value })}
          style={{ flex:1, background:'transparent', border:'1px solid var(--border)',
                   color:'var(--text)', fontSize:10, borderRadius:3,
                   padding:'2px 6px', outline:'none', width:'100%' }} />
      ))}
      {row('Channels', sel('channelMode', [
        {v:'L',       l:'Left only'},
        {v:'R',       l:'Right only'},
        {v:'overlay', l:'L + R overlay'},
        {v:'stacked', l:'L / R stacked'},
      ]))}
      {row('Amplitude', sel('amplitudeMode', [{v:'fixed',l:'Fixed ±1.0'},{v:'auto',l:'Auto-scale'}]))}
      {row('Amp zoom',   slider('amplitudeZoom', 0.1, 4.0, 0.1, v => `${v.toFixed(1)}×`, 'Amplitude Zoom'))}
      {row('Time window',sel('timeWindowMs', [
        {v:'10', l:'10 ms'}, {v:'25', l:'25 ms'}, {v:'50', l:'50 ms'},
        {v:'100',l:'100 ms'},{v:'200',l:'200 ms'},
      ]))}
      {row('Clip indicator', tog('showClipping', ''))}
      {s.showClipping && row('Clip threshold', (
        <div style={{ flex: 1 }}>
          <input type="range" min={-18} max={0} step={0.5}
            value={s.clipThresholdDb}
            onChange={e => onChange({ clipThresholdDb: parseFloat(e.target.value) })}
            style={{ width: '100%' }} />
          <div style={{ textAlign: 'right', fontSize: 9, color: 'var(--text-dim)', marginTop: 1 }}>
            {(s.clipThresholdDb >= 0 ? '+' : '') + s.clipThresholdDb.toFixed(1) + ' dBFS'}
          </div>
        </div>
      ))}

      {section('Trigger')}
      {row('Mode', sel('triggerMode', [
        {v:'free',    l:'Free running'},
        {v:'rising',  l:'Rising edge ↑'},
        {v:'falling', l:'Falling edge ↓'},
      ]))}
      <div style={{ opacity: s.triggerMode === 'free' ? 0.35 : 1,
                     pointerEvents: s.triggerMode === 'free' ? 'none' : 'auto' }}>
        {row('Level', slider('triggerLevel', -1.0, 1.0, 0.05, v => v.toFixed(2), 'Trigger Level'))}
      </div>
    </div>
  );
}

// ── Main component ────────────────────────────────────────────────────────────
function AudioMonitorNode ({ id, data, selected }: NodeProps) {
  const d = data as AudioMonitorNodeData;

  const [settings, setSettings] = useState<AudioMonitorSettings>({
    ...DEFAULT_SETTINGS, ...(d.settings ?? {}),
    ...(d.settingsJson ? JSON.parse(d.settingsJson) : {}),
  });
  const { showSettings, openSettings, closeSettings, toggleSettings } = useNodeSettings(id);
  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const { setHint } = useContext(HintContext);
  const portBodyRef = useRef<HTMLDivElement>(null);
  const [sampleRate,   setSampleRate]   = useState(48000);

  const canvasRef    = useRef<HTMLCanvasElement>(null);
  const settingsRef  = useRef(settings);
  const leftBuf      = useRef<Float32Array>(new Float32Array(0));
  const rightBuf     = useRef<Float32Array>(new Float32Array(0));
  const rafRef       = useRef<number>(0);

  useEffect(() => { settingsRef.current = settings; }, [settings]);

  const patch = useCallback((p: Partial<AudioMonitorSettings>) => {
    setSettings(s => {
      const next = { ...s, ...p };
      Bridge.setNodeSettings(id, next);
      return next;
    });
    if ('customName' in p)
      Bridge.setNodeLabel(id, p.customName ?? '');
  }, [id]);

  // Subscribe to audio snapshots — unsubscribe on unmount
  useEffect(() => {
    const unsub = Bridge.onAudioSnapshot((snaps: AudioSnapshot[]) => {
      try {
        const snap = snaps.find(s => s.nodeId === id);
        if (!snap || settingsRef.current.paused) return;
        const l = parseFloats(snap.l);
        const r = parseFloats(snap.r);
        if (l.length > 0) leftBuf.current  = l;
        if (r.length > 0) rightBuf.current = r;
        if (snap.sr > 0)  setSampleRate(snap.sr);
      } catch (e) {
        console.error('AudioMonitorNode snapshot error', e);
      }
    });
    return () => { try { unsub(); } catch {} };
  }, [id]);

  // Render loop
  useEffect(() => {
    const render = () => {
      const canvas = canvasRef.current;
      if (!canvas) return;
      const ctx = canvas.getContext('2d');
      if (!ctx) return;

      const w = canvas.width;
      const h = canvas.height;
      drawWaveform(ctx, w, h, leftBuf.current, rightBuf.current,
                   settingsRef.current, sampleRate);
      rafRef.current = requestAnimationFrame(render);
    };
    rafRef.current = requestAnimationFrame(render);
    return () => cancelAnimationFrame(rafRef.current);
  }, [sampleRate, collapsed]);  // restart when canvas remounts after expand



  const W         = 320;
  const canvasH   = settings.channelMode === 'stacked' ? 140 : 100;

  return (
    <div style={{
      width: W,
      background: 'var(--surface)',
      border: `1px solid ${selected ? 'var(--audio)' : 'var(--border)'}`,
      borderTop: '3px solid var(--audio)',
      borderRadius: 'var(--radius)',
      boxShadow: selected
        ? '0 0 0 1px var(--audio), 0 8px 32px var(--audio-glow)'
        : '0 4px 16px rgba(0,0,0,.5)',
      position: 'relative',
      zIndex: showSettings ? 9999 : undefined,
    }}>

      {/* Audio In handle */}
      <NodeHandle nodeId={id} label="Audio In"  direction="in"  colour="rgb(20,80,20)" index={0} total={1} offset={46} portBodyRef={portBodyRef} />

      {/* Audio Out handle */}
      <NodeHandle nodeId={id} label="Audio Out" direction="out" colour="rgb(20,80,20)" index={0} total={1} offset={46} portBodyRef={portBodyRef} />

      {/* Header */}
      <NodeHeader title={settings.customName || "AUDIO MONITOR"} accent="var(--audio)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed}>
        <NodeHeaderButton onClick={() => patch({ paused: !settings.paused })}
          active={settings.paused} activeAccent="var(--audio)"
          onHint={{ onMouseEnter: () => setHint({ title: settings.paused ? 'Resume' : 'Pause', body: settings.paused ? 'Resume the audio waveform display.' : 'Freeze the waveform display.' }), onMouseLeave: () => setHint(null) }}>
          {settings.paused ? <Play size={14} /> : <Pause size={14} />}
        </NodeHeaderButton>
      </NodeHeader>

      {!collapsed && <>
      {/* Scope body — canvas + amplitude slider side by side */}
      <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()}
           onPointerDown={e => e.stopPropagation()}
           style={{ display:'flex', gap:4, padding:'6px 6px 4px' }}>

        {/* Waveform canvas */}
        <canvas
          ref={canvasRef}
          width={W - 44}
          height={canvasH}
          style={{ borderRadius:3, display:'block', flexShrink:0 }}
        />

        {/* Amplitude zoom slider (vertical) */}
        <div style={{ display:'flex', flexDirection:'column', alignItems:'center',
                      gap:2, width:28 }}>
          <span style={{ fontSize:8, color:'var(--text-muted)' }}>4×</span>
          <input
            type="range" min={0.1} max={4.0} step={0.1}
                onMouseEnter={() => setHint({ title: 'Amplitude Zoom', body: 'Scales the waveform display vertically. Range: 0.1× to 4.0×.' })}
                onMouseLeave={() => setHint(null)}
            value={settings.amplitudeMode === 'auto' ? 1 : settings.amplitudeZoom}
            disabled={settings.amplitudeMode === 'auto'}
            onChange={e => patch({ amplitudeZoom: parseFloat(e.target.value) })}
            style={{ writingMode:'vertical-lr' as const, direction:'rtl' as const,
                     height: canvasH - 20, cursor:'pointer', flex:1 }}
          />
          <span style={{ fontSize:8, color:'var(--text-muted)' }}>.1×</span>
        </div>
      </div>

      {/* Time window slider (horizontal) */}
      <div className="nodrag" onMouseDown={e => e.stopPropagation()}
           onPointerDown={e => e.stopPropagation()}
           style={{ padding:'2px 8px 6px',
                    display:'flex', alignItems:'center', gap:6 }}>
        <span style={{ fontSize:8, color:'var(--text-muted)', flexShrink:0 }}>10ms</span>
        <input
          type="range" min={10} max={200}
                onMouseEnter={() => setHint({ title: 'Time Window', body: 'Sets the display time window in milliseconds. Range: 10ms to 200ms.' })}
                onMouseLeave={() => setHint(null)}
          step={0} /* snapped below */
          value={settings.timeWindowMs}
          onChange={e => {
            const raw = parseInt(e.target.value);
            const snapped = [10,25,50,100,200].reduce((a,b) =>
              Math.abs(b-raw) < Math.abs(a-raw) ? b : a);
            patch({ timeWindowMs: snapped });
          }}
          style={{ flex:1, cursor:'pointer' }}
        />
        <span style={{ fontSize:8, color:'var(--text-muted)', flexShrink:0 }}>200ms</span>
        <span style={{ fontSize:9, color:'var(--audio)', width:36, textAlign:'right', flexShrink:0 }}>
          {settings.timeWindowMs}ms
        </span>
      </div>

      {/* Channel mode colour legend */}
      <div style={{ padding:'0 8px 6px', display:'flex', gap:10 }}>
        {(settings.channelMode === 'L' || settings.channelMode === 'overlay' || settings.channelMode === 'stacked') && (
          <div style={{ display:'flex', alignItems:'center', gap:4 }}>
            <div style={{ width:16, height:2, background:COL_L, borderRadius:1 }}/>
            <span style={{ fontSize:9, color:'var(--text-muted)' }}>L</span>
          </div>
        )}
        {(settings.channelMode === 'R' || settings.channelMode === 'overlay' || settings.channelMode === 'stacked') && (
          <div style={{ display:'flex', alignItems:'center', gap:4 }}>
            <div style={{ width:16, height:2, background:COL_R, borderRadius:1 }}/>
            <span style={{ fontSize:9, color:'var(--text-muted)' }}>R</span>
          </div>
        )}
        {settings.triggerMode !== 'free' && (
          <div style={{ display:'flex', alignItems:'center', gap:4 }}>
            <div style={{ width:16, height:2, background:COL_TRIGGER, borderRadius:1 }}/>
            <span style={{ fontSize:9, color:'var(--text-muted)' }}>
              {settings.triggerMode === 'rising' ? 'trig ↑' : 'trig ↓'}
            </span>
          </div>
        )}
      </div>

      {/* Settings overlay */}
      {showSettings && (
        <SettingsPanel s={settings} onChange={patch}
          onClose={closeSettings} onReset={() => patch(DEFAULT_SETTINGS)} />
      )}
      </>}
    </div>
  );
}

export default memo(AudioMonitorNode);
