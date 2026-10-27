import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, PortActivityEntry } from './Bridge';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHandle, useNodeCollapsed, Checkbox, SettingsPanelHeader, readoutBoxStyle } from './NodeUtils';
import { HintContext } from './HintPanel';
import { NodeSelect } from './NodeSelect';
import ValueBox from './ValueBox';
import { LearnButton } from './LearnButton';
import { useMidiLearn } from './Learn';
import { KbdFader, KbdFaderType, TYPE_NAMES, faderLabel, fadersOfSettings, maxValue, maxNum, hasNum, isWide,
         getMaxKbdFaders, sendKbdFader, resendKbdFaders } from './KbdFaders';   // v0.0.956

// MidiKeyboardNode.tsx — a fully interactive, mouse-playable piano keyboard
// (click-drag across keys, velocity from click position, pitch + mod
// wheels) with its own MIDI In/Out ports: it both sends what you play here
// and lights up keys from upstream MIDI arriving on its In port.
//
// Raw MIDI status bytes appear throughout below rather than named
// constants — standard MIDI convention, kept for anyone unfamiliar: the
// high nibble is the message type (0x90 note-on, 0x80 note-off, 0xB0
// control change, 0xE0 pitch bend), the low nibble is the channel
// (0-indexed, so channel-1 = 0x_0). Pitch bend's value is 14-bit, split
// across two 7-bit data bytes (MIDI data bytes can only use 7 of 8 bits —
// the 8th marks status vs. data bytes) — `v & 0x7F` is the LSB, `(v >> 7)
// & 0x7F` is the MSB.

// ── Types ─────────────────────────────────────────────────────────────────────
export interface MidiKeyboardSettings {
  octaves:    1 | 2 | 3 | 4;
  startNote:  number;   // 0=C-1, 12=C0, 24=C1 ... 108=C9
  channel:    number;   // 1-16, 0=omni
  velocity:   'mouse' | '64' | '100' | '127';
  showNames:  boolean;
  noteOctave: 'yamaha' | 'roland';   // v0.0.928 — middle C (60) = C3 / C4, as in the MIDI Monitor
  customName: string;
  modWheel:   number;
  // v0.0.956 — faders (between the header and the keyboard), aftertouch, glide
  faders:     KbdFader[];
  aftertouch: 'off' | 'channel' | 'poly';
  atSource:   'mouse' | 'pressure';   // mouse: drag down on a held key; pressure: Force Touch trackpad / pen
  glide:      boolean;                // dragging across the keys plays each key it reaches
}

export interface MidiKeyboardNodeData {
  label:    string;
  nodeType: 7;
  ports:    { id: string; label: string; type: 'midi' | 'audio'; direction: 'input' | 'output' }[];
  settingsJson?: string;
  [key: string]: unknown;
}

const DEFAULT_SETTINGS: MidiKeyboardSettings = {
  octaves:   1,
  startNote: 48,   // C2 Yamaha / C3 Roland
  channel:   1,
  velocity:   'mouse',
  showNames:  true,
  noteOctave: 'yamaha',
  customName: '',
  modWheel:   0,
  faders:     [],
  aftertouch: 'off',
  atSource:   'mouse',
  glide:      true,
};
const AT_DRAG_PX = 40;   // mouse aftertouch: this many pixels down = full pressure

// ── Note layout ───────────────────────────────────────────────────────────────
// White key pattern in an octave: C D E F G A B
const WHITE_NOTES = [0, 2, 4, 5, 7, 9, 11]; // semitones from root
const BLACK_NOTES = [1, 3, 6, 8, 10];         // semitones from root
const BLACK_POS   = [1, 2, 4, 5, 6];          // position after which white key

const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];
/** v0.0.928 — note name in the chosen convention (Yamaha: 60 = C3, Roland: 60 = C4). */
const noteLabel = (n: number, conv: 'yamaha' | 'roland') =>
  `${NOTE_NAMES[n % 12]}${Math.floor (n / 12) + (conv === 'roland' ? -1 : -2)}`;

function isBlack (note: number) { return BLACK_NOTES.includes(note % 12); }

// ── Settings panel ────────────────────────────────────────────────────────────
function SettingsPanel ({ s, onChange, onDiscreteChange, onClose, onReset, learnTarget, onLearn }: {
  s: MidiKeyboardSettings;
  onChange: (p: Partial<MidiKeyboardSettings>) => void;
  onDiscreteChange: (p: Partial<MidiKeyboardSettings>) => void;
  onClose: () => void;
  onReset: () => void;
  learnTarget: string | null;
  onLearn: (k: number) => void;
}) {
  const { setHint } = useContext (HintContext);
  const tip = (title: string, body: string) => ({ onMouseEnter: () => setHint ({ title, body }), onMouseLeave: () => setHint (null) });
  const faders = s.faders ?? [];
  const setFader = (k: number, p: Partial<KbdFader>) => {
    const next = faders.map ((f, i) => {
      if (i !== k) return f;
      const m = { ...f, ...p };
      m.num   = Math.min (m.num, maxNum (m.type));
      m.value = Math.min (m.value, maxValue (m.type));
      if (p.type && isWide (p.type) !== isWide (f.type))   // keep the position when the resolution changes
        m.value = isWide (p.type) ? Math.min (16383, f.value << 7) : f.value >> 7;
      return m;
    });
    onDiscreteChange ({ faders: next });
  };
  const typeOptions = (['cc', 'pc', 'cc14', 'nrpn'] as KbdFaderType[]).map (t => ({ id: t, name: TYPE_NAMES[t] }));
  const nextNum = () => {   // a new CC fader starts on an unused CC (1, 2, 3…)
    const used = new Set (faders.filter (f => f.type === 'cc').map (f => f.num));
    let n = 1; while (used.has (n) && n < 127) n++;
    return n;
  };

  const startNoteOptions = [];
  for (let n = 0; n <= 108; n += 12)
    startNoteOptions.push({ id: String(n), name: noteLabel(n, s.noteOctave ?? 'yamaha') });

  const row = (label: string, child: React.ReactNode) => (
    <div style={{ display:'flex', alignItems:'center', gap:8, marginBottom:6 }}>
      <div style={{ width:90, fontSize:10, color:'var(--text-dim)', flexShrink:0 }}>{label}</div>
      <div style={{ flex:1, minWidth:0 }}>{child}</div>
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
        width:240, background:'var(--surface2)',
        border:'1px solid var(--border-hi)', borderRadius:'var(--radius)',
        padding:'10px 12px', zIndex:1000,
        boxShadow:'0 8px 32px rgba(0,0,0,.6)',
        fontFamily:"'JetBrains Mono', monospace",
        userSelect:'none',
      }}>
      <SettingsPanelHeader title="Keyboard" onReset={onReset} onClose={onClose} />
      {row('Start note', (
        <NodeSelect value={String(s.startNote)} showEmpty={false} accent="var(--midi)"
          onChange={v => onDiscreteChange({ startNote: Number(v) })}
          options={startNoteOptions} />
      ))}
      {row('Octaves', (
        <NodeSelect value={String(s.octaves)} showEmpty={false} accent="var(--midi)"
          onChange={v => onDiscreteChange({ octaves: Number(v) as 1|2|3|4 })}
          options={[{id:'1',name:'1 octave'},{id:'2',name:'2 octaves'},
                    {id:'3',name:'3 octaves'},{id:'4',name:'4 octaves'}]} />
      ))}
      {row('Channel', (
        <NodeSelect value={String(s.channel)} showEmpty={false} accent="var(--midi)"
          onChange={v => onDiscreteChange({ channel: Number(v) })}
          options={[{id:'0',name:'Omni'},...Array.from({length:16},(_,i)=>({id:String(i+1),name:`Ch ${i+1}`}))]} />
      ))}
      {row('Velocity', (
        <NodeSelect value={s.velocity} showEmpty={false} accent="var(--midi)"
          onChange={v => onDiscreteChange({ velocity: v as MidiKeyboardSettings['velocity'] })}
          options={[{id:'mouse',name:'Mouse pos'},{id:'64',name:'Fixed 64'},
                    {id:'100',name:'Fixed 100'},{id:'127',name:'Fixed 127'}]} />
      ))}
      {row('Note C3/C4', (
        <NodeSelect value={s.noteOctave ?? 'yamaha'} showEmpty={false} accent="var(--midi)"
          onChange={v => onDiscreteChange({ noteOctave: v === 'roland' ? 'roland' : 'yamaha' })}
          options={[{id:'yamaha',name:'Yamaha (C3)'},{id:'roland',name:'Roland (C4)'}]} />
      ))}
      {row('Note names', (
        <Checkbox checked={s.showNames}
          onChange={v => onDiscreteChange({ showNames: v })}
          label="Show" accent="var(--midi)" />
      ))}
      {/* v0.0.956 — glide and aftertouch */}
      <div {...tip ('Glide', 'On: dragging across the keys with the button held plays each key you reach (the previous one is released). Off: only the key you clicked plays.')}>
        {row('Glide', (
          <Checkbox checked={s.glide ?? true} onChange={v => onDiscreteChange({ glide: v })} label="Drag plays the keys" accent="var(--midi)" />
        ))}
      </div>
      <div {...tip ('Aftertouch', 'Pressure on a held key. Channel: one value for the whole channel. Poly: per note. Off: none sent.')}>
        {row('Aftertouch', (
          <NodeSelect value={s.aftertouch ?? 'off'} showEmpty={false} accent="var(--midi)"
            onChange={v => onDiscreteChange({ aftertouch: v as MidiKeyboardSettings['aftertouch'] })}
            options={[{id:'off',name:'Off'},{id:'channel',name:'Channel'},{id:'poly',name:'Poly'}]} />
        ))}
      </div>
      {(s.aftertouch ?? 'off') !== 'off' && (
        <div {...tip ('Aftertouch source', `Mouse: hold a key and drag down (${AT_DRAG_PX} px = full), back up to release. Pressure: press harder on a Force Touch trackpad (Mac), or a pen's pressure.`)}>
          {row('Pressure', (
            <NodeSelect value={s.atSource ?? 'mouse'} showEmpty={false} accent="var(--midi)"
              onChange={v => onDiscreteChange({ atSource: v as MidiKeyboardSettings['atSource'] })}
              options={[{id:'mouse',name:'Mouse ↕'},{id:'pressure',name:'Trackpad / pen'}]} />
          ))}
        </div>
      )}

      {/* v0.0.956 — faders */}
      <div style={{ fontSize:9, color:'var(--text-dim)', textTransform:'uppercase', letterSpacing:0.5, margin:'10px 0 4px', opacity:0.7 }}>
        Faders ({faders.length}/{getMaxKbdFaders()})
      </div>
      {faders.map ((f, k) => (
        <div key={k} style={{ display:'flex', alignItems:'center', gap:4, marginBottom:4 }}>
          <span style={{ width:12, fontSize:9, color:'var(--text-muted)' }}>{k + 1}</span>
          <div style={{ width:92 }}>
            <NodeSelect value={f.type} showEmpty={false} accent="var(--midi)"
              onChange={v => setFader (k, { type: v as KbdFaderType })} options={typeOptions} />
          </div>
          {hasNum (f.type)
            ? <ValueBox value={f.num} min={0} max={maxNum (f.type)} accent="var(--midi)" width={42}
                title={f.type === 'nrpn' ? 'NRPN number (0-16383) — drag / double-click to type' : f.type === 'cc14' ? 'CC number (0-31; its LSB is CC +32)' : 'CC number (0-127)'}
                onCommit={v => setFader (k, { num: v })} />
            : <span style={{ width:42 }} />}
          <LearnButton armed={learnTarget === String (k)} onClick={() => onLearn (k)} what="this fader (CC number or program change)" />
          <span onClick={() => onDiscreteChange ({ faders: faders.filter ((_, i) => i !== k) })}
            {...tip ('Remove fader', 'Removes this fader. Undoable (⌘Z).')}
            style={{ marginLeft:'auto', cursor:'pointer', fontSize:10, color:'var(--text-muted)', padding:'0 2px' }}>✕</span>
        </div>
      ))}
      {faders.length < getMaxKbdFaders() && (
        <span onClick={() => onDiscreteChange ({ faders: [...faders, { type: 'cc', num: nextNum(), value: 0 }] })}
          {...tip ('Add a fader', `Up to ${getMaxKbdFaders()} faders (Preferences → MIDI Keyboard). Each sends on the Keyboard's channel: CC, Program, CC 14-bit or NRPN. Undoable.`)}
          style={{ display:'inline-block', fontSize:9, padding:'2px 8px', borderRadius:3, cursor:'pointer',
                   border:'1px solid var(--border)', color:'var(--text-muted)' }}>+ fader</span>
      )}
    </div>
  );
}

// ── Keyboard canvas ───────────────────────────────────────────────────────────
const WHITE_W = 22;
const WHITE_H = 70;
const BLACK_W = 13;
const BLACK_H = 44;

function Keyboard ({ nodeId, settings, activeNotes, onNoteOn, onNoteOff, onAftertouch }: {
  nodeId:     string;
  settings:   MidiKeyboardSettings;
  activeNotes: Set<number>;
  onNoteOn:   (note: number, vel: number) => void;
  onNoteOff:  (note: number) => void;
  onAftertouch: (note: number, value: number) => void;   // v0.0.956
}) {
  const { octaves, startNote, showNames } = settings;
  const totalWhites = octaves * 7;   // C to B per octave — the extra final C was dropped 2026-09-25 (user's request)
  const W = totalWhites * WHITE_W;
  const H = WHITE_H;
  const pressedRef = useRef<number | null>(null);
  const atRef      = useRef ({ startY: 0, last: 0 });   // v0.0.956 — aftertouch: where the key was taken, last value sent
  const settingsRef = useRef (settings); settingsRef.current = settings;
  const cbRef = useRef ({ onNoteOn, onNoteOff, onAftertouch });
  cbRef.current = { onNoteOn, onNoteOff, onAftertouch };

  // Build key list
  const keys: { note: number; white: boolean; x: number; w: number; h: number }[] = [];
  let wx = 0;
  for (let oct = 0; oct < octaves; oct++) {
    for (let i = 0; i < 7; i++) {
      const note = startNote + oct * 12 + WHITE_NOTES[i];
      keys.push({ note, white: true, x: wx, w: WHITE_W, h: WHITE_H });
      wx += WHITE_W;
    }
  }

  // Black keys
  for (let oct = 0; oct < octaves; oct++) {
    for (let i = 0; i < BLACK_POS.length; i++) {
      const note = startNote + oct * 12 + BLACK_NOTES[i];
      const baseX = (oct * 7 + BLACK_POS[i]) * WHITE_W;
      keys.push({ note, white: false, x: baseX - BLACK_W / 2, w: BLACK_W, h: BLACK_H });
    }
  }
  const keysRef = useRef (keys); keysRef.current = keys;

  const svgRef = useRef<SVGSVGElement>(null);

  // v0.0.956 — which key is under the pointer (black keys first), in the
  // keyboard's own pixels: the canvas zoom is divided out
  const keyAt = (clientX: number, clientY: number) => {
    const svg = svgRef.current;
    if (! svg) return null;
    const r = svg.getBoundingClientRect();
    const scale = r.width / (svg.width.baseVal.value || 1) || 1;
    const x = (clientX - r.left) / scale, y = (clientY - r.top) / scale;
    if (x < 0 || y < 0 || y > H || x >= svg.width.baseVal.value) return null;
    const ks = keysRef.current;
    const k = ks.find (q => ! q.white && x >= q.x && x < q.x + q.w && y < q.h)
           ?? ks.find (q => q.white && x >= q.x && x < q.x + q.w);
    return k ? { k, y, scale } : null;
  };

  // Simulates touch-sensitivity with a mouse, which has no pressure/velocity
  // of its own: in 'mouse' mode, where you click within the key's height
  // stands in for how hard you'd have struck a real key — near the top
  // (rel≈0) reads as soft, near the bottom (rel≈1) as hard.
  const velocityAt = (y: number, keyH: number) => {
    const sv = settingsRef.current.velocity;
    if (sv !== 'mouse') return Number(sv);
    return Math.max(1, Math.min(127, Math.round((y / keyH) * 127)));
  };

  const releaseAt = () => {
    const note = pressedRef.current;
    if (note !== null && atRef.current.last > 0) { cbRef.current.onAftertouch (note, 0); atRef.current.last = 0; }
  };

  const take = (note: number, vel: number, clientY: number) => {
    releaseAt();
    if (pressedRef.current !== null) cbRef.current.onNoteOff (pressedRef.current);
    pressedRef.current = note;
    atRef.current = { startY: clientY, last: 0 };
    cbRef.current.onNoteOn (note, vel);
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    e.stopPropagation();
    e.preventDefault();
    if (e.button !== 0) return;
    const hit = keyAt (e.clientX, e.clientY);
    if (hit) take (hit.k.note, velocityAt (hit.y, hit.k.h), e.clientY);
  };

  // Listens on window, not the key element itself, since the mouse can be
  // released anywhere — dragged off the keyboard entirely, over the
  // settings panel, wherever — and a note left "on" because its release
  // happened outside the original element would be a real, audible stuck
  // note, not just a cosmetic bug.
  // v0.0.956 — moving with the button held: glide to the key under the
  // pointer, and (mouse aftertouch) the distance dragged down = pressure.
  // Force Touch / pen pressure arrive as their own events.
  useEffect(() => {
    const up = () => {
      if (pressedRef.current !== null) {
        releaseAt();
        cbRef.current.onNoteOff(pressedRef.current);
        pressedRef.current = null;
      }
    };
    const sendAt = (v: number) => {
      const note = pressedRef.current;
      if (note === null) return;
      v = Math.max (0, Math.min (127, Math.round (v)));
      if (v === atRef.current.last) return;
      atRef.current.last = v;
      cbRef.current.onAftertouch (note, v);
    };
    const move = (e: MouseEvent) => {
      if (pressedRef.current === null) return;
      const st = settingsRef.current;
      if (st.glide ?? true) {
        const hit = keyAt (e.clientX, e.clientY);
        if (hit && hit.k.note !== pressedRef.current) { take (hit.k.note, velocityAt (hit.y, hit.k.h), e.clientY); return; }
      }
      if ((st.aftertouch ?? 'off') !== 'off' && (st.atSource ?? 'mouse') === 'mouse') {
        const svg = svgRef.current;
        const scale = svg ? (svg.getBoundingClientRect().width / (svg.width.baseVal.value || 1) || 1) : 1;
        sendAt ((e.clientY - atRef.current.startY) / scale / AT_DRAG_PX * 127);
      }
    };
    const pressureOn = () => (settingsRef.current.aftertouch ?? 'off') !== 'off' && settingsRef.current.atSource === 'pressure';
    // Force Touch trackpad (WebKit, macOS): webkitForce 1 (a click) … 3 (full)
    const force = (e: Event) => {
      if (! pressureOn()) return;
      const f = (e as any).webkitForce as number | undefined;
      if (typeof f === 'number') sendAt ((f - 1) / 2 * 127);
    };
    // A pen's pressure (0-1), wherever the web view reports it
    const pen = (e: PointerEvent) => { if (pressureOn() && e.pointerType === 'pen' && e.buttons) sendAt (e.pressure * 127); };
    // No force-click "Look Up" while playing
    const noForceClick = (e: Event) => { if (pressedRef.current !== null || pressureOn()) e.preventDefault(); };
    const svg = svgRef.current;
    window.addEventListener('mouseup', up);
    window.addEventListener('mousemove', move);
    window.addEventListener('pointermove', pen);
    svg?.addEventListener('webkitmouseforcechanged', force);
    svg?.addEventListener('webkitmouseforcewillbegin', noForceClick);
    return () => {
      window.removeEventListener('mouseup', up);
      window.removeEventListener('mousemove', move);
      window.removeEventListener('pointermove', pen);
      svg?.removeEventListener('webkitmouseforcechanged', force);
      svg?.removeEventListener('webkitmouseforcewillbegin', noForceClick);
    };
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return (
    <svg ref={svgRef} width={W} height={H}
      style={{ display:'block', cursor:'pointer', userSelect:'none' }}
      className="nodrag"
      onMouseDown={handleMouseDown}
    >
      {/* White keys first */}
      {keys.filter(k => k.white).map(k => {
        const active = activeNotes.has(k.note);
        const isC    = k.note % 12 === 0;
        return (
          <g key={k.note}>
            <rect x={k.x + 0.5} y={0.5} width={k.w - 1} height={k.h - 1}
              rx={2}
              fill={active ? '#4FC3F7' : '#f0f0f0'}
              stroke="#999"
              strokeWidth={1}
              style={{ transition: 'fill 0.05s' }}
            />
            {showNames && (isC || octaves === 1) && (
              <text x={k.x + k.w / 2} y={k.h - 8}
                textAnchor="middle" fontSize={7}
                fill={active ? '#0d1117' : '#666'}
                style={{ pointerEvents: 'none', fontFamily: "'JetBrains Mono', monospace" }}>
                {noteLabel(k.note, settings.noteOctave ?? 'yamaha')}
              </text>
            )}
          </g>
        );
      })}
      {/* Black keys on top */}
      {keys.filter(k => !k.white).map(k => {
        const active = activeNotes.has(k.note);
        return (
          <g key={k.note}>
            <rect x={k.x} y={0} width={k.w} height={k.h}
              rx={2}
              fill={active ? '#4FC3F7' : '#1a1d23'}
              stroke={active ? '#4FC3F7' : '#111'}
              strokeWidth={1}
              style={{ transition: 'fill 0.05s' }}
            />
          </g>
        );
      })}
    </svg>
  );
}

// ── Faders (v0.0.956) ─────────────────────────────────────────────────────────
// One row each: label, slider, value box — between the header and the keyboard.
function FaderRow ({ f, k, onValue, onHint }: {
  f: KbdFader; k: number;
  onValue: (k: number, v: number, final: boolean) => void;
  onHint: (h: { title: string; body: string } | null) => void;
}) {
  const max = maxValue (f.type);
  const label = faderLabel (f);
  return (
    <div style={{ display:'flex', alignItems:'center', gap:6, height:18 }}
      onMouseEnter={() => onHint ({ title: label, body: `Fader ${k + 1}: ${label} on the Keyboard's channel, 0-${max}. Double-click the slider for 0.${f.type === 'pc' ? ' The program is sent when you let go.' : ''} ⚙ to change, add or remove faders.` })}
      onMouseLeave={() => onHint (null)}>
      <span style={{ width:86, flexShrink:0, fontSize:9, color:'var(--text-dim)', whiteSpace:'nowrap', overflow:'hidden', textOverflow:'ellipsis',
                     fontFamily:"'JetBrains Mono', monospace" }}>{label}</span>
      <input type="range" className="nodrag" min={0} max={max} step={1} value={f.value}
        onChange={e => onValue (k, Number (e.target.value), false)}
        onMouseUp={() => onValue (k, f.value, true)} onKeyUp={() => onValue (k, f.value, true)}
        onDoubleClick={e => { e.stopPropagation(); onValue (k, 0, true); }}
        style={{ flex:1, minWidth:0, height:18, margin:0, cursor:'pointer', ['--thumb-color' as any]: 'var(--midi)' }} />
      <ValueBox value={f.value} min={0} max={max} accent="var(--midi)" width={isWide (f.type) ? 44 : 34}
        coarse={isWide (f.type) ? 64 : 1}
        onLive={v => onValue (k, v, false)} onCommit={v => onValue (k, v, true)}
        style={{ ...readoutBoxStyle ('var(--text-dim)'), fontSize: 9 }} />
    </div>
  );
}

// ── Wheel slider ──────────────────────────────────────────────────────────────
function WheelSlider ({ label, value, min, max, onChange, onRelease, color = 'var(--midi)', wheelHint, wheelHintClear, portAnchor, resetValue }: {
  portAnchor?: string;   // v0.0.918 — marks the label as a port anchor (see NodeUtils' "Port anchors")
  resetValue?: number;   // v0.0.945 — double-click goes back to it
  label:       string;
  value:       number;
  min:         number;
  max:         number;
  onChange:    (v: number) => void;
  onRelease?:  () => void;
  color?:      string;
  wheelHint?:  (label: string, min: number, max: number) => void;
  wheelHintClear?: () => void;
}) {
  const H = WHITE_H + 8;
  const pct = (value - min) / (max - min);
  const thumbY = H - 12 - pct * (H - 24);

  return (
    <div style={{ display:'flex', flexDirection:'column', alignItems:'center',
                  gap:2, width:18 }}>
      <div data-port-anchor={portAnchor} style={{ fontSize:7, color:'var(--text-muted)', letterSpacing:'0.05em',
                    textTransform:'uppercase', writingMode:'vertical-lr',
                    transform:'rotate(180deg)', marginBottom:2 }}>
        {label}
      </div>
      <input
        type="range" min={min} max={max} value={value}
        onChange={e => onChange(Number(e.target.value))}
        onDoubleClick={resetValue === undefined ? undefined : (e => { e.stopPropagation(); onChange(resetValue); onRelease?.(); })}
        onMouseUp={onRelease}
        onTouchEnd={onRelease}
        onMouseEnter={() => wheelHint?.(label, min, max)}
        onMouseLeave={() => wheelHintClear?.()}
        style={{
          writingMode: 'vertical-lr' as const,
          direction: 'rtl' as const,
          height: H,
          cursor: 'pointer',
        }}
        className="nodrag"
        onMouseDown={e => e.stopPropagation()}
        onPointerDown={e => e.stopPropagation()}
      />
    </div>
  );
}

// ── Main component ────────────────────────────────────────────────────────────
function MidiKeyboardNode ({ id, data, selected }: NodeProps) {
  const d = data as MidiKeyboardNodeData;
  const [settings, setSettings]     = useState<MidiKeyboardSettings>(() => ({
    ...DEFAULT_SETTINGS,
    ...(d.settingsJson ? JSON.parse(d.settingsJson) as Partial<MidiKeyboardSettings> : {}),
  }));
  const settingsRef = useRef(settings); settingsRef.current = settings;   // v0.0.956
  const [activeNotes, setActiveNotes]   = useState<Set<number>>(new Set());
  const [pitchWheel, setPitchWheel]     = useState(8192);   // centre — not saved (transient)
  const modWheel    = settings.modWheel;
  const { showSettings, openSettings, closeSettings, toggleSettings } = useNodeSettings(id);
  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const { setHint } = useContext(HintContext);
  const wheelHint = (label: string, min: number, max: number) =>
    setHint({ title: label === 'P' ? 'Pitch Wheel' : 'Mod Wheel', body: `Range: ${min} to ${max}. Release to reset to centre.` });
  const wheelHintClear = () => setHint(null);
  const portBodyRef = useRef<HTMLDivElement>(null);

  // patch(): live update, no Undo-history entry — used while a control is
  // actively being dragged (mod wheel movement, live text typing).
  // commitPatch(): pushes one Undo-history entry — used for discrete
  // choices (a dropdown selection) and on drag-release, mirroring the
  // same drag-then-commit shape sliders use elsewhere in this project.

  const patch = (p: Partial<MidiKeyboardSettings>) => {
    setSettings(s => {
      const next = { ...s, ...p };
      Bridge.setNodeSettings(id, next);
      return next;
    });
    if ('customName' in p)
      Bridge.setNodeLabel(id, p.customName ?? '');
  };

  const commitPatch = (p: Partial<MidiKeyboardSettings>) => {
    setSettings(s => {
      const next = { ...s, ...p };
      Bridge.commitSettingsChange(id, next);
      return next;
    });
    if ('customName' in p)
      Bridge.setNodeLabel(id, p.customName ?? '');
  };



  // Omni (channel 0) is a receiver-side concept — "listen on every
  // channel" — that has no equivalent for a sender: a single outgoing MIDI
  // message can only carry one channel number. Since this node is a
  // source (it sends what you play), Omni falls back to channel 1 for
  // anything actually transmitted; it only means something for input
  // filtering elsewhere in the graph.
  const ch = settings.channel === 0 ? 1 : settings.channel;

  // This node has both a MIDI In and a MIDI Out port — so beyond the keys
  // it lights up itself when clicked (onNoteOn/onNoteOff below), it also
  // needs to reflect notes arriving from *upstream* on its In port. The
  // wire format here (`entry.notes`, "status,note status,note ...") is a
  // compact per-block batch of raw MIDI events for this node specifically.
  useEffect(() => {
    const unsub = Bridge.onPortActivity((entries: PortActivityEntry[]) => {
      const entry = entries.find(e => e.id === id);
      if (!entry || !entry.notes || entry.notes.trim() === '') return;

      const noteEvents = entry.notes.trim().split(' ').map(s => {
        const [st, n] = s.split(',').map(Number);
        return { status: st, note: n };
      });

      setActiveNotes(prev => {
        const next = new Set(prev);
        noteEvents.forEach(({ status, note }) => {
          const isNoteOn = (status & 0xF0) === 0x90;
          const isNoteOff = (status & 0xF0) === 0x80;
          if (isNoteOn) next.add(note);
          if (isNoteOff) next.delete(note);
        });
        return next;
      });
    });
    return unsub;
  }, [id]);

  // Undo/redo restores the UI's settings state automatically (React state
  // from JSON), but the backend audio engine's actual mod-wheel value
  // isn't part of that — it only changes in response to a real MIDI CC
  // message. Without explicitly re-sending one here, the panel could show
  // a restored mod wheel position that the audio engine never actually
  // received, leaving the two silently out of sync.
  useEffect(() => {
    const sj = (data as any)?.settingsJson;
    try {
      const restored = sj ? JSON.parse(sj) : DEFAULT_SETTINGS;
      setSettings(s => ({ ...DEFAULT_SETTINGS, ...restored }));
      // Re-send mod wheel MIDI CC so audio reflects restored value
      const modVal = restored.modWheel ?? DEFAULT_SETTINGS.modWheel;
      const chVal  = (restored.channel ?? DEFAULT_SETTINGS.channel) || 1;
      Bridge.sendMidiKeyEvent(id, 0xB0 | (chVal - 1), 1, modVal);
      // v0.0.956 — and the faders whose values changed (project opened, undo / redo)
      resendKbdFaders(id, chVal, fadersOfSettings(restored));
    } catch {}
  }, [(data as any)?.settingsJson]);

  const onNoteOn = useCallback((note: number, vel: number) => {
    setActiveNotes(s => new Set([...s, note]));
    Bridge.sendMidiKeyEvent(id, 0x90 | (ch - 1), note, vel);
  }, [id, ch]);

  const onNoteOff = useCallback((note: number) => {
    setActiveNotes(s => { const n = new Set(s); n.delete(note); return n; });
    Bridge.sendMidiKeyEvent(id, 0x80 | (ch - 1), note, 0);
  }, [id, ch]);

  // Pitch bend and mod wheel behave differently on purpose, matching how
  // real MIDI controllers themselves distinguish the two: pitch bend is
  // spring-loaded, always snapping back to centre (8192) the instant it's
  // released, so its value is never persisted — pointless to save a
  // position that never stays put. The mod wheel is positional; it stays
  // wherever it's left, so its value genuinely is part of the node's
  // saved settings (`modWheel`), not just transient UI state.
  const onPitchChange = useCallback((v: number) => {
    setPitchWheel(v);
    // Pitch bend: status 0xE0, LSB, MSB
    Bridge.sendMidiKeyEvent(id, 0xE0 | (ch - 1), v & 0x7F, (v >> 7) & 0x7F);
  }, [id, ch]);

  const onPitchRelease = useCallback(() => {
    setPitchWheel(8192);
    Bridge.sendMidiKeyEvent(id, 0xE0 | (ch - 1), 0, 64); // centre = 8192 = 0x40 MSB
  }, [id, ch]);

  const onModChange = useCallback((v: number) => {
    patch({ modWheel: v });
    Bridge.sendMidiKeyEvent(id, 0xB0 | (ch - 1), 1, v); // CC1 = mod wheel
  }, [id, ch]);

  // v0.0.956 — aftertouch: channel (0xD0, one value) or poly (0xA0, per note)
  const onAftertouch = useCallback((note: number, v: number) => {
    if (settingsRef.current.aftertouch === 'poly') Bridge.sendMidiKeyEvent(id, 0xA0 | (ch - 1), note, v);
    else if (settingsRef.current.aftertouch === 'channel') Bridge.sendMidiKeyEvent(id, 0xD0 | (ch - 1), v, 0);
  }, [id, ch]);

  // v0.0.956 — faders: live while dragging (no undo step), one undo step on release
  const onFaderValue = useCallback((k: number, v: number, final: boolean) => {
    const cur = settingsRef.current.faders ?? [];
    if (! cur[k]) return;
    const faders = cur.map((f, i) => i === k ? { ...f, value: Math.max(0, Math.min(maxValue(f.type), Math.round(v))) } : f);
    settingsRef.current = { ...settingsRef.current, faders };
    patch({ faders });
    sendKbdFader(id, ch, k, faders[k], final);
    if (final) Bridge.commitNodeSettings(id);
  }, [id, ch]);

  // v0.0.956 — Learn a fader from a controller on the MIDI In: a CC sets the
  // CC number (a 14-bit fader keeps its type for CC 0-31), a program change
  // sets the type
  const learn = useMidiLearn<string>(id, (t, ev) => {
    const k = Number(t), cur = settingsRef.current.faders ?? [];
    if (! cur[k]) return;
    const f = cur[k];
    let next: KbdFader | null = null;
    if (ev.type === 4) next = f.type === 'cc14' && ev.d1 < 32 ? { ...f, num: ev.d1 } : { type: 'cc', num: ev.d1, value: isWide(f.type) ? f.value >> 7 : f.value };
    else if (ev.type === 5) next = { ...f, type: 'pc', value: isWide(f.type) ? f.value >> 7 : f.value };
    if (next) commitPatch({ faders: cur.map((x, i) => i === k ? next! : x) });
  });



  const totalWhites = settings.octaves * 7;   // C to B (no final C), 2026-09-25
  const kbdW = totalWhites * WHITE_W;
  const W    = kbdW + 50; // + sliders

  return (
    <div style={{
      minWidth: W,   // was width — grows with a long title (in-place rename, 2026-09-25)
      background: 'var(--surface)',
      border: `1px solid ${selected ? 'var(--midi)' : 'var(--border)'}`,
      borderTop: '3px solid var(--midi)',
      borderRadius: 'var(--radius)',
      boxShadow: selected
        ? '0 0 0 1px var(--midi), 0 8px 32px var(--midi-glow)'
        : '0 4px 16px rgba(0,0,0,.5)',
      position: 'relative',
    }}>
      {/* MIDI In handle */}
      <NodeHandle nodeId={id} label="MIDI In"  direction="in"  colour="var(--midi)" index={0} total={1} anchor="wheel-label" portBodyRef={portBodyRef} />
      {/* MIDI Out handle */}
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour="var(--midi)" index={0} total={1} anchor="wheel-label" portBodyRef={portBodyRef} />

      {/* Header */}
      <NodeHeader title={settings.customName || "MIDI KEYBOARD"}
        rename={{ value: settings.customName ?? '', placeholder: 'MIDI Keyboard', onCommit: v => commitPatch({ customName: v }) }} accent="var(--midi)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed} />

      {!collapsed && <>
      {/* v0.0.956 — faders, between the header and the keyboard */}
      {(settings.faders ?? []).length > 0 && (
        <div className="nodrag" onMouseDown={e => e.stopPropagation()} onPointerDown={e => e.stopPropagation()}
          style={{ display:'flex', flexDirection:'column', gap:2, padding:'5px 8px 0' }}>
          {(settings.faders ?? []).map((f, k) => <FaderRow key={k} f={f} k={k} onValue={onFaderValue} onHint={setHint} />)}
        </div>
      )}
      {/* Body — sliders + keyboard */}
      <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()}
           onPointerDown={e => e.stopPropagation()}
           style={{ display:'flex', alignItems:'flex-start', gap:4, padding:'6px 6px 6px' }}>

        {/* Pitch wheel */}
        {/* v0.0.918 — the MIDI ports line up with this "P" label: just above the
            wheel track, so never on the pitch head, which rests at its centre
            (user's choice, 2026-09-29). */}
        <WheelSlider label="P" portAnchor="wheel-label" value={pitchWheel} min={0} max={16383}
          wheelHint={wheelHint} wheelHintClear={wheelHintClear}
          onChange={onPitchChange} onRelease={onPitchRelease} />

        {/* Mod wheel */}
        <WheelSlider label="M" value={modWheel} min={0} max={127}
          wheelHint={wheelHint} wheelHintClear={wheelHintClear}
          onChange={onModChange} onRelease={() => Bridge.commitNodeSettings(id)} color="var(--midi)" resetValue={0} />

        {/* Keyboard */}
        <Keyboard
          nodeId={id}
          settings={settings}
          activeNotes={activeNotes}
          onNoteOn={onNoteOn}
          onNoteOff={onNoteOff}
          onAftertouch={onAftertouch}
        />
      </div>

      {/* Channel + octave info bar */}
      <div style={{ padding:'2px 8px 5px', display:'flex', gap:8, fontSize:9,
                    color:'var(--text-muted)' }}>
        <span>Ch {settings.channel === 0 ? 'Omni' : settings.channel}</span>
        <span>·</span>
        <span>{noteLabel(settings.startNote, settings.noteOctave ?? 'yamaha')}
          {' '}→{' '}
          {noteLabel(settings.startNote + settings.octaves * 12 - 1, settings.noteOctave ?? 'yamaha')}
        </span>
        <span>·</span>
        <span>{settings.octaves} oct</span>
      </div>

      {/* Settings panel */}
      {showSettings && (
        <SettingsPanel s={settings} onChange={patch} onDiscreteChange={commitPatch} onClose={() => closeSettings()}
          onReset={() => commitPatch({ ...DEFAULT_SETTINGS, faders: settings.faders ?? [] })}
          learnTarget={learn.target} onLearn={k => learn.start(String(k))} />
      )}
      </>}
    </div>
  );
}

export default memo(MidiKeyboardNode);
