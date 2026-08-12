import { memo, useContext, useCallback, useEffect, useRef, useState } from 'react';
import { NodeProps } from '@xyflow/react';
import { Bridge, PortActivityEntry } from './Bridge';
import { useNodeSettings, useNodeDelete, NodeHeader, NodeHandle, useNodeCollapsed, Checkbox, SettingsPanelHeader } from './NodeUtils';
import { HintContext } from './HintPanel';
import { NodeSelect } from './NodeSelect';

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
  customName: string;
  modWheel:   number;
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
  startNote: 48,   // C3
  channel:   1,
  velocity:   'mouse',
  showNames:  true,
  customName: '',
  modWheel:   0,
};

// ── Note layout ───────────────────────────────────────────────────────────────
// White key pattern in an octave: C D E F G A B
const WHITE_NOTES = [0, 2, 4, 5, 7, 9, 11]; // semitones from root
const BLACK_NOTES = [1, 3, 6, 8, 10];         // semitones from root
const BLACK_POS   = [1, 2, 4, 5, 6];          // position after which white key

const NOTE_NAMES = ['C','C#','D','D#','E','F','F#','G','G#','A','A#','B'];

function isBlack (note: number) { return BLACK_NOTES.includes(note % 12); }

// ── Settings panel ────────────────────────────────────────────────────────────
function SettingsPanel ({ s, onChange, onDiscreteChange, onClose, onReset }: {
  s: MidiKeyboardSettings;
  onChange: (p: Partial<MidiKeyboardSettings>) => void;
  onDiscreteChange: (p: Partial<MidiKeyboardSettings>) => void;
  onClose: () => void;
  onReset: () => void;
}) {
  const startNoteOptions = [];
  for (let oct = -1; oct <= 9; oct++)
    startNoteOptions.push({ id: String((oct + 1) * 12), name: `C${oct}` });

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
      {row('Name', (
        <input
          type="text"
          value={s.customName}
          placeholder="MIDI Keyboard"
          onChange={e => onChange({ customName: e.target.value })}
          style={{ flex: 1, background: 'var(--surface)', border: '1px solid var(--border)',
                   color: 'var(--text)', fontSize: 10, borderRadius: 3,
                   padding: '2px 6px', fontFamily: "'JetBrains Mono', monospace",
                   outline: 'none', width: '100%' }}
        />
      ))}
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
      {row('Note names', (
        <Checkbox checked={s.showNames}
          onChange={v => onDiscreteChange({ showNames: v })}
          label="Show" accent="var(--midi)" />
      ))}
    </div>
  );
}

// ── Keyboard canvas ───────────────────────────────────────────────────────────
const WHITE_W = 22;
const WHITE_H = 70;
const BLACK_W = 13;
const BLACK_H = 44;

function Keyboard ({ nodeId, settings, activeNotes, onNoteOn, onNoteOff }: {
  nodeId:     string;
  settings:   MidiKeyboardSettings;
  activeNotes: Set<number>;
  onNoteOn:   (note: number, vel: number) => void;
  onNoteOff:  (note: number) => void;
}) {
  const { octaves, startNote, showNames } = settings;
  const totalWhites = octaves * 7 + 1; // +1 for final C
  const W = totalWhites * WHITE_W;
  const H = WHITE_H;
  const pressedRef = useRef<number | null>(null);

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
  // Final C
  keys.push({ note: startNote + octaves * 12, white: true, x: wx, w: WHITE_W, h: WHITE_H });

  // Black keys
  for (let oct = 0; oct < octaves; oct++) {
    for (let i = 0; i < BLACK_POS.length; i++) {
      const note = startNote + oct * 12 + BLACK_NOTES[i];
      const baseX = (oct * 7 + BLACK_POS[i]) * WHITE_W;
      keys.push({ note, white: false, x: baseX - BLACK_W / 2, w: BLACK_W, h: BLACK_H });
    }
  }

  // Simulates touch-sensitivity with a mouse, which has no pressure/velocity
  // of its own: in 'mouse' mode, where you click within the key's height
  // stands in for how hard you'd have struck a real key — near the top
  // (rel≈0) reads as soft, near the bottom (rel≈1) as hard.
  const getVelocity = (e: React.MouseEvent, keyH: number, keyY: number) => {
    if (settings.velocity !== 'mouse') return Number(settings.velocity);
    const rel = (e.clientY - keyY) / keyH;
    return Math.max(1, Math.min(127, Math.round(rel * 127)));
  };

  const svgRef = useRef<SVGSVGElement>(null);

  // A mouse can only "press" one key at a time, unlike real hands on a
  // real keyboard — so dragging across keys while the button stays down
  // (a glissando-style sweep) needs the previous note explicitly released
  // before the new one starts, or notes would pile up as still "on".
  const handleMouseDown = (e: React.MouseEvent, note: number, h: number) => {
    e.stopPropagation();
    e.preventDefault();
    if (pressedRef.current !== null) onNoteOff(pressedRef.current);
    const rect = (e.target as SVGElement).getBoundingClientRect();
    const vel  = getVelocity(e, h, rect.top);
    pressedRef.current = note;
    onNoteOn(note, vel);
  };

  // Listens on window, not the key element itself, since the mouse can be
  // released anywhere — dragged off the keyboard entirely, over the
  // settings panel, wherever — and a note left "on" because its release
  // happened outside the original element would be a real, audible stuck
  // note, not just a cosmetic bug.
  useEffect(() => {
    const up = () => {
      if (pressedRef.current !== null) {
        onNoteOff(pressedRef.current);
        pressedRef.current = null;
      }
    };
    window.addEventListener('mouseup', up);
    return () => window.removeEventListener('mouseup', up);
  }, [onNoteOff]);

  return (
    <svg ref={svgRef} width={W} height={H}
      style={{ display:'block', cursor:'pointer', userSelect:'none' }}
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
    >
      {/* White keys first */}
      {keys.filter(k => k.white).map(k => {
        const active = activeNotes.has(k.note);
        const isC    = k.note % 12 === 0;
        return (
          <g key={k.note} onMouseDown={e => handleMouseDown(e, k.note, k.h)}>
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
                {NOTE_NAMES[k.note % 12]}{Math.floor(k.note / 12) - 1}
              </text>
            )}
          </g>
        );
      })}
      {/* Black keys on top */}
      {keys.filter(k => !k.white).map(k => {
        const active = activeNotes.has(k.note);
        return (
          <g key={k.note} onMouseDown={e => handleMouseDown(e, k.note, k.h)}>
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

// ── Wheel slider ──────────────────────────────────────────────────────────────
function WheelSlider ({ label, value, min, max, onChange, onRelease, color = 'var(--midi)', wheelHint, wheelHintClear }: {
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
      <div style={{ fontSize:7, color:'var(--text-muted)', letterSpacing:'0.05em',
                    textTransform:'uppercase', writingMode:'vertical-lr',
                    transform:'rotate(180deg)', marginBottom:2 }}>
        {label}
      </div>
      <input
        type="range" min={min} max={max} value={value}
        onChange={e => onChange(Number(e.target.value))}
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



  const totalWhites = settings.octaves * 7 + 1;
  const kbdW = totalWhites * WHITE_W;
  const W    = kbdW + 50; // + sliders

  return (
    <div style={{
      width: W,
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
      <NodeHandle nodeId={id} label="MIDI In"  direction="in"  colour="var(--midi)" index={0} total={1} offset={-10} portBodyRef={portBodyRef} />
      {/* MIDI Out handle */}
      <NodeHandle nodeId={id} label="MIDI Out" direction="out" colour="var(--midi)" index={0} total={1} offset={-10} portBodyRef={portBodyRef} />

      {/* Header */}
      <NodeHeader title={settings.customName || "MIDI KEYBOARD"} accent="var(--midi)"
        showSettings={showSettings} onToggleSettings={toggleSettings}
        onDelete={handleDelete} collapsed={collapsed} onToggleCollapsed={toggleCollapsed} />

      {!collapsed && <>
      {/* Body — sliders + keyboard */}
      <div ref={portBodyRef} className="nodrag" onMouseDown={e => e.stopPropagation()}
           onPointerDown={e => e.stopPropagation()}
           style={{ display:'flex', alignItems:'flex-start', gap:4, padding:'6px 6px 6px' }}>

        {/* Pitch wheel */}
        <WheelSlider label="P" value={pitchWheel} min={0} max={16383}
          wheelHint={wheelHint} wheelHintClear={wheelHintClear}
          onChange={onPitchChange} onRelease={onPitchRelease} />

        {/* Mod wheel */}
        <WheelSlider label="M" value={modWheel} min={0} max={127}
          wheelHint={wheelHint} wheelHintClear={wheelHintClear}
          onChange={onModChange} onRelease={() => Bridge.commitNodeSettings(id)} color="var(--midi)" />

        {/* Keyboard */}
        <Keyboard
          nodeId={id}
          settings={settings}
          activeNotes={activeNotes}
          onNoteOn={onNoteOn}
          onNoteOff={onNoteOff}
        />
      </div>

      {/* Channel + octave info bar */}
      <div style={{ padding:'2px 8px 5px', display:'flex', gap:8, fontSize:9,
                    color:'var(--text-muted)' }}>
        <span>Ch {settings.channel === 0 ? 'Omni' : settings.channel}</span>
        <span>·</span>
        <span>{NOTE_NAMES[settings.startNote % 12]}{Math.floor(settings.startNote/12)-1}
          {' '}→{' '}
          {NOTE_NAMES[(settings.startNote + settings.octaves * 12) % 12]}
          {Math.floor((settings.startNote + settings.octaves * 12) / 12) - 1}
        </span>
        <span>·</span>
        <span>{settings.octaves} oct</span>
      </div>

      {/* Settings panel */}
      {showSettings && (
        <SettingsPanel s={settings} onChange={patch} onDiscreteChange={commitPatch} onClose={() => closeSettings()} onReset={() => commitPatch(DEFAULT_SETTINGS)} />
      )}
      </>}
    </div>
  );
}

export default memo(MidiKeyboardNode);
