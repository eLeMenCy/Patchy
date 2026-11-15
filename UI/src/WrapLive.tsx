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

// ═════════════════════════════════════════════════════════════════════════════
// v0.0.987 — the other built-in nodes (user, 2026-11-15): read-only, like the
// devices above (user: "stay consistent"); red only for what the backend
// already reports (port taken: UDP / OSC / Art-Net In; DMX port status; a DMX
// port not in the system's list; an Art-Net Out with no host). MQTT's broker
// state isn't reported to the UI yet — no red there for now.
// ═════════════════════════════════════════════════════════════════════════════

const DMX_STATUS = ['', 'in use', "can't open", 'half-duplex conflict'];

// v0.0.987 — an unplugged Enttec keeps its port "open" (status 0) and the
// serial port list was only fetched on request: while a DMX port is shown
// in a wrap, the list is re-fetched every 2 s (one shared poll), so a port
// that vanished turns red (user, 2026-11-15).
let serialPollers = 0;
let serialPollTimer: ReturnType<typeof setInterval> | null = null;
function watchSerialPorts (): () => void {
  if (serialPollers++ === 0) { Bridge.listSerialPorts (); serialPollTimer = setInterval (() => Bridge.listSerialPorts (), 2000); }
  return () => { if (--serialPollers === 0 && serialPollTimer) { clearInterval (serialPollTimer); serialPollTimer = null; } };
}
const UDP_MODE   = ['Unicast', 'Multicast', 'Broadcast'];

/** UDP / OSC / Art-Net / DMX / MQTT device nodes: what they listen on or send to. */
export function NetStatus ({ nodeId, nodeType, settings }: { nodeId: string; nodeType: number; settings: Record<string, unknown> }) {
  const s = settings as Record<string, any>;
  const [inUse, setInUse]   = useState (0);
  const [dmxSt, setDmxSt]   = useState (0);
  const [ports, setPorts]   = useState<string[] | null> (null);
  const watchesPort = nodeType === 8 || nodeType === 10 || nodeType === 12;
  const isDmx       = nodeType === 14 || nodeType === 15;
  useEffect (() => {
    if (! watchesPort && ! isDmx) return;
    return Bridge.onPortActivity (entries => {
      const e = entries.find (x => x.id === nodeId);
      if (! e) return;
      setInUse (prev => prev === (e.portInUse ?? 0) ? prev : (e.portInUse ?? 0));
      setDmxSt (prev => prev === (e.dmxStatus ?? 0) ? prev : (e.dmxStatus ?? 0));
    });
  }, [nodeId, watchesPort, isDmx]);
  useEffect (() => {
    if (! isDmx) return;
    const off = Bridge.onSerialPorts (setPorts), stop = watchSerialPorts ();
    return () => { off (); stop (); };
  }, [isDmx]);

  let text = '', red = '';
  switch (nodeType) {
    case 8:  text = `:${s.udpPort ?? 0} · ${UDP_MODE[s.udpMode ?? 0] ?? ''}${s.udpMode === 1 && s.udpMulticastAddr ? ` ${s.udpMulticastAddr}` : ''}`;
             if (inUse > 0) red = `port ${inUse} in use`; break;
    case 9:  text = `${s.udpTargetHost || '—'}:${s.udpPort ?? 0} · ${UDP_MODE[s.udpMode ?? 0] ?? ''}`; break;
    case 10: text = `:${s.oscPort ?? 0}`; if (inUse > 0) red = `port ${inUse} in use`; break;
    case 11: text = `${s.oscTargetHost || '—'}:${s.oscPort ?? 0} · ${s.oscAddress ?? '/patchy'}`; break;
    case 12: text = `Universe ${s.artNetUniverse ?? 0}`; if (inUse > 0) red = `port ${inUse} in use`; break;
    case 13: text = `Universe ${s.artNetUniverse ?? 0} → ${s.artNetTargetHost || 'no host'}`; if (! s.artNetTargetHost) red = 'no host'; break;
    case 14: case 15: {
      const path = String (s.dmxDevicePath ?? '');
      text = path ? path.split ('/').pop () ?? path : '— none —';
      if (path && dmxSt > 0) red = DMX_STATUS[dmxSt] ?? 'error';
      else if (path && ports !== null && ! ports.includes (path)) red = 'missing';
      break;
    }
    case 22: case 23:
      text = `${s.mqttHost || '—'}:${s.mqttPort ?? 1883} · ${s.mqttTopic || '—'}`; break;
  }
  const full = red ? `${text} — ${red}` : text;
  return (
    <span title={full} style={{ fontSize: 9, fontFamily: mono, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                                maxWidth: 170, color: red ? '#ef4444' : 'var(--text-dim)' }}>{full}</span>
  );
}

/** OSC / UDP / MQTT Monitor: the last 3 messages. */
export function MiniLog ({ nodeId, nodeType, paused }: { nodeId: string; nodeType: number; paused: boolean }) {
  const [rows, setRows] = useState<string[]> ([]);
  const pausedRef = useRef (paused);
  pausedRef.current = paused;
  useEffect (() => {
    const add = (lines: string[]) => { if (lines.length && ! pausedRef.current) setRows (prev => [...prev, ...lines.slice (-3)].slice (-3)); };
    if (nodeType === 20)
      return Bridge.onOscMonitorEvents (bs => { const b = bs.find (x => x.nodeId === nodeId); if (b) add (b.events.map (e => `${e.ad}  ${e.ar}`)); });
    if (nodeType === 21)
      return Bridge.onUdpMonitorEvents (bs => { const b = bs.find (x => x.nodeId === nodeId);
        if (b) add (b.events.map (e => `${e.ip ? `${e.ip}:${e.pt}` : '?'}  ${e.by} B  ${e.hx.slice (0, 16)}${e.hx.length > 16 ? '…' : ''}`)); });
    return Bridge.onMqttMonitorEvents (bs => { const b = bs.find (x => x.nodeId === nodeId);
      if (b) add (b.events.map (e => `${e.tp}  ${e.pl.replace (/(\.\d*?)0+$/, '$1').replace (/\.$/, '')}`)); });
  }, [nodeId, nodeType]);
  return (
    <div style={{ height: 40, marginTop: 3, padding: '2px 6px', borderRadius: 3, background: 'var(--surface)', overflow: 'hidden',
                  fontFamily: mono, fontSize: 9, lineHeight: '12px', color: 'var(--text-dim)', display: 'flex', flexDirection: 'column',
                  justifyContent: 'flex-end' }}>
      {rows.length === 0 ? <span style={{ color: 'var(--text-muted)' }}>no messages yet</span>
                         : rows.map ((t, k) => <span key={k} style={{ whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
                                                                      opacity: k === rows.length - 1 ? 1 : 0.6 }}>{t}</span>)}
    </div>
  );
}

/** DMX / Art-Net Monitor: the channels the node shows, as small bars. */
export function MiniLevels ({ nodeId, artNet, start, count, accent }: { nodeId: string; artNet: boolean; start: number; count: number; accent: string }) {
  const ref = useRef<HTMLCanvasElement> (null);
  const last = useRef<number[] | null> (null);
  const draw = () => {
    const c = ref.current, ch = last.current;
    if (! c) return;
    const dpr = window.devicePixelRatio || 1;
    const n0 = Math.max (1, count), cssW = n0 * 10 - 2;   // v0.0.987 — 8 px bars, 2 px gaps (user)
    if (c.style.width !== `${cssW}px`) c.style.width = `${cssW}px`;
    const w = Math.round (cssW * dpr), h = Math.round (40 * dpr);
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    const ctx = c.getContext ('2d');
    if (! ctx) return;
    ctx.clearRect (0, 0, w, h);
    const n = n0, gap = 2 * dpr, bw = 8 * dpr;
    const col = getComputedStyle (c).getPropertyValue (accent.replace (/^var\((--[a-z]+)\)$/, '$1')).trim () || '#f59e0b';
    for (let i = 0; i < n; i++) {
      const v = (ch?.[start + i] ?? 0) / 255, x = i * (bw + gap);
      ctx.fillStyle = 'rgba(255,255,255,0.06)'; ctx.fillRect (x, 0, bw, h);
      ctx.fillStyle = col; ctx.fillRect (x, h - v * h, bw, v * h);
    }
  };
  useEffect (() => {
    const on = (snaps: { id: string; ch: number[] }[]) => { const s = snaps.find (x => x.id === nodeId); if (s) { last.current = s.ch; draw (); } };
    return artNet ? Bridge.onArtNetSnapshot (on) : Bridge.onDmxSnapshot (on);
  }, [nodeId, artNet, start, count]);   // eslint-disable-line react-hooks/exhaustive-deps
  useEffect (() => { draw (); });       // range changed: redraw at once
  return (
    <div style={{ marginTop: 3 }}>
      <canvas ref={ref} style={{ maxWidth: '100%', height: 40, display: 'block', borderRadius: 2 }} />
      <div style={{ display: 'flex', justifyContent: 'space-between', width: Math.max (1, count) * 10 - 2, maxWidth: '100%', minWidth: 60, fontSize: 8, fontFamily: mono, color: 'var(--text-muted)', marginTop: 1 }}>
        <span>Ch {start + 1}</span><span>Ch {Math.min (start + count, 512)}</span>
      </div>
    </div>
  );
}

/** MIDI CH. Matrix: its mapping, e.g. "1→2 · 3→3+4 · others dropped". */
export function matrixText (settings: Record<string, unknown>): string {
  const cells = Array.isArray (settings.cells) ? (settings.cells as [number, number][]) : [];
  const drop  = settings.dropUnmapped !== false;
  const by = new Map<number, number[]> ();
  for (const [r, c] of cells) by.set (r, [...(by.get (r) ?? []), c]);
  const parts = [...by.keys ()].sort ((a, b) => a - b)
    .map (r => `${r + 1}→${(by.get (r) ?? []).sort ((a, b) => a - b).map (c => c + 1).join ('+')}`);
  return parts.length === 0 ? (drop ? 'nothing mapped (all dropped)' : 'nothing mapped (all pass)')
                            : `${parts.join (' · ')} · others ${drop ? 'dropped' : 'pass'}`;
}
