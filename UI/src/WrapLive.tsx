// WrapLive.tsx — v0.0.986: the newer built-in controls of a wrap's ⚙ panel
// (and of its pinned rows on the folded box):
//   • DeviceName   — MIDI / Audio In / Out: the selected device, read-only,
//                    red when it's missing (user's choice, 2026-11-14)
//   • MiniWave     — Audio Monitor: a 40 px live wave
//   • MiniMidi     — MIDI Monitor: the last 3 events
//   • MqttSend     — MQTT Console: a remembered topic + a value + Send
// The monitors' data is pushed by the backend for every monitor in the graph
// anyway (whether its node is drawn or not): a view here costs only its own
// drawing — the wave redraws on each snapshot (no animation loop).

import { useEffect, useRef, useState } from 'react';
import { Bridge, virtualPortNumber, virtualPortName } from './Bridge';
import type { RawMidiMonitorEvent } from './Bridge';
import { NodeSelect } from './NodeSelect';   // Patchy's dropdown (not the OS one — user, 2026-11-14)

const mono = "'JetBrains Mono', monospace";

// ── Device name ──────────────────────────────────────────────────────────────
// Names seen this session, by device id: an unplugged device leaves the list,
// so its name comes from here ("Launchkey — missing"), never its raw id
// (CoreMIDI ids are numbers like -1096627353 — user, 2026-11-14).
const seenNames = new Map<string, string> ();
export function DeviceName ({ nodeType, deviceId }: { nodeType: 1 | 2 | 3 | 4; deviceId?: string }) {
  const [list, setList] = useState<{ id: string; name: string }[] | null> (null);
  useEffect (() => nodeType <= 2
    ? Bridge.onMidiDevices (l => setList (nodeType === 2 ? l.midiOutDevices : l.midiInDevices))
    : Bridge.onAudioDevices (l => setList (nodeType === 4 ? l.audioOutDevices : l.audioInDevices)),
  [nodeType]);
  const v = virtualPortNumber (deviceId);
  list?.forEach (d => seenNames.set (d.id, d.name));
  const found = list?.find (d => d.id === deviceId);
  const known = deviceId ? seenNames.get (deviceId) : undefined;
  const text = ! deviceId ? '— none —'
             : v > 0 ? virtualPortName (nodeType === 1, v)
             : found ? found.name
             : known ? `${known} — missing` : 'Device missing';
  const missing = !! deviceId && v === 0 && list !== null && ! found;
  return (
    <span title={text} style={{ fontSize: 9, fontFamily: mono, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                                maxWidth: 150, color: missing ? '#ef4444' : ! deviceId ? 'var(--text-muted)' : 'var(--text-dim)' }}>
      {text}
    </span>
  );
}

// ── Audio Monitor: mini wave ─────────────────────────────────────────────────
export function MiniWave ({ nodeId, paused }: { nodeId: string; paused: boolean }) {
  const ref = useRef<HTMLCanvasElement> (null);
  // v0.0.986 — sharp on Retina / HiDPI: the backing store at the screen's
  // pixel ratio (180×40 was stretched → blurry — user)
  useEffect (() => {
    const c = ref.current;
    if (! c) return;
    const fit = () => {
      const dpr = window.devicePixelRatio || 1;
      const w = Math.max (1, Math.round (c.clientWidth * dpr)), h = Math.round (40 * dpr);
      if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    };
    fit ();
    const ro = new ResizeObserver (fit);
    ro.observe (c);
    return () => ro.disconnect ();
  }, []);
  const pausedRef = useRef (paused);
  pausedRef.current = paused;
  useEffect (() => Bridge.onAudioSnapshot (snaps => {
    const s = snaps.find (x => x.nodeId === nodeId);
    const c = ref.current;
    if (! s || ! c || pausedRef.current) return;
    const ctx = c.getContext ('2d');
    if (! ctx) return;
    const l = s.l ? s.l.split (',').map (Number) : [];
    const r = s.r ? s.r.split (',').map (Number) : [];
    const n = Math.max (l.length, r.length);
    const w = c.width, h = c.height, mid = h / 2;
    ctx.clearRect (0, 0, w, h);
    ctx.strokeStyle = 'rgba(255,255,255,0.08)';
    ctx.lineWidth = 1;
    ctx.beginPath (); ctx.moveTo (0, mid); ctx.lineTo (w, mid); ctx.stroke ();
    if (n < 2) return;
    ctx.strokeStyle = getComputedStyle (c).getPropertyValue ('--audio').trim () || '#4ade80';   // Canvas can't read CSS vars itself
    ctx.lineWidth = window.devicePixelRatio || 1;
    ctx.beginPath ();
    for (let i = 0; i < n; i++) {
      const v = ((l[i] ?? 0) + (r[i] ?? l[i] ?? 0)) / 2000;   // ×1000 ints, L+R averaged
      const x = (i / (n - 1)) * w, y = mid - v * (mid - 1);
      if (i === 0) ctx.moveTo (x, y); else ctx.lineTo (x, y);
    }
    ctx.stroke ();
  }), [nodeId]);
  return <canvas ref={ref} width={180} height={40}
                 style={{ width: '100%', height: 40, display: 'block', marginTop: 3, borderRadius: 3, background: 'var(--surface)' }} />;
}

// ── MIDI Monitor: the last 3 events ──────────────────────────────────────────
const NOTE = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const noteName = (n: number) => `${NOTE[n % 12]}${Math.floor (n / 12) - 1}`;
export function midiEventText (e: RawMidiMonitorEvent): string {
  const kind = e.st & 0xF0, ch = (e.st & 0x0F) + 1;
  switch (kind) {
    case 0x90: return e.d2 > 0 ? `Note On  ${noteName (e.d1)} ${e.d2}  ch${ch}` : `Note Off ${noteName (e.d1)}  ch${ch}`;
    case 0x80: return `Note Off ${noteName (e.d1)}  ch${ch}`;
    case 0xA0: return `Poly AT  ${noteName (e.d1)} ${e.d2}  ch${ch}`;
    case 0xB0: return `CC ${e.d1} = ${e.d2}  ch${ch}`;
    case 0xC0: return `Program ${e.d1}  ch${ch}`;
    case 0xD0: return `Ch AT ${e.d1}  ch${ch}`;
    case 0xE0: return `Pitch ${((e.d2 << 7) | e.d1) - 8192}  ch${ch}`;
    default:   return `System ${e.st.toString (16).toUpperCase ()}`;
  }
}
export function MiniMidi ({ nodeId, paused }: { nodeId: string; paused: boolean }) {
  const [rows, setRows] = useState<string[]> ([]);
  const pausedRef = useRef (paused);
  pausedRef.current = paused;
  useEffect (() => Bridge.onMidiMonitorEvents (batches => {
    const b = batches.find (x => x.nodeId === nodeId);
    if (! b || b.events.length === 0 || pausedRef.current) return;
    setRows (prev => [...prev, ...b.events.slice (-3).map (midiEventText)].slice (-3));
  }), [nodeId]);
  return (
    <div style={{ height: 40, marginTop: 3, padding: '2px 6px', borderRadius: 3, background: 'var(--surface)', overflow: 'hidden',
                  fontFamily: mono, fontSize: 9, lineHeight: '12px', color: 'var(--text-dim)', display: 'flex', flexDirection: 'column',
                  justifyContent: 'flex-end' }}>
      {rows.length === 0 ? <span style={{ color: 'var(--text-muted)' }}>no MIDI yet</span>
                         : rows.map ((t, k) => <span key={k} style={{ whiteSpace: 'nowrap', opacity: k === rows.length - 1 ? 1 : 0.6 }}>{t}</span>)}
    </div>
  );
}

// ── MQTT Console: send a value ───────────────────────────────────────────────
export function MqttSend ({ nodeId, topics, accent }: { nodeId: string; topics: string[]; accent: string }) {
  const [topic, setTopic] = useState (topics[0] ?? '');
  const [value, setValue] = useState ('');
  useEffect (() => { if (! topics.includes (topic)) setTopic (topics[0] ?? ''); }, [topics, topic]);
  if (topics.length === 0)
    return <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>Send once from the node: its topics show here.</span>;
  const v = parseFloat (value.replace (',', '.'));
  const ok = topic !== '' && value.trim () !== '' && ! isNaN (v);
  const send = () => { if (ok) Bridge.sendMqttConsole (nodeId, topic, v); };
  const box: React.CSSProperties = { fontSize: 9, fontFamily: mono, padding: '2px 4px', borderRadius: 3, outline: 'none',
                                     background: 'var(--surface)', border: '1px solid var(--border)', color: 'var(--text)' };
  return (
    <div style={{ display: 'flex', gap: 4, marginTop: 3, alignItems: 'center' }}>
      <div className="nodrag" style={{ flex: 1, minWidth: 0 }}>
        <NodeSelect value={topic} onChange={setTopic} options={topics.map (t => ({ id: t, name: t }))} accent={accent} />
      </div>
      <input className="nodrag" value={value} placeholder="value" onChange={e => setValue (e.target.value)}
             onKeyDown={e => { if (e.key === 'Enter') send (); }} style={{ ...box, width: 48 }} />
      <span onClick={send} style={{ fontSize: 9, padding: '2px 8px', borderRadius: 3, cursor: ok ? 'pointer' : 'default',
                                    border: `1px solid ${ok ? accent : 'var(--border)'}`, color: ok ? accent : 'var(--text-muted)' }}>Send</span>
    </div>
  );
}
