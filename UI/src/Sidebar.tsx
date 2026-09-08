import { DragEvent, useEffect, useState, useContext } from 'react';
import { DawContext } from './DawContext';
import { HintPanel, NODE_HINTS, HintContext } from './HintPanel';
import { Bridge, PaxInfo } from './Bridge';
import { detectPaxTheme, detectPaxCategoryKey, resolveCssColor } from './NodeUtils';

const BUILTIN_GROUPS = [
  {
    label: 'MIDI', accent: 'var(--midi)', dim: 'var(--midi-dim)',
    nodes: [
      { type: 1 as const, label: 'MIDI In Device',  desc: 'Receive from MIDI device', icon: '⊲' },
      { type: 2 as const, label: 'MIDI Out Device', desc: 'Send to MIDI device',      icon: '⊳' },
      { type: 5 as const, label: 'MIDI Monitor',    desc: 'Inspect MIDI events',      icon: '⊞' },
      { type: 7 as const, label: 'MIDI Keyboard',   desc: 'Virtual keyboard / viewer', icon: '♩' },
    ],
  },
  {
    label: 'Audio', accent: 'var(--audio)', dim: 'var(--audio-dim)',
    nodes: [
      { type: 3 as const, label: 'Audio In Device',  desc: 'Receive from audio device', icon: '◀' },
      { type: 4 as const, label: 'Audio Out Device', desc: 'Send to audio device',       icon: '▶' },
      { type: 6 as const, label: 'Audio Monitor',    desc: 'Waveform display',           icon: '◎' },
      { type: 26 as const, label: 'Audio Player',    desc: 'Play a file or generate a signal', icon: '▷' },
    ],
  },
  {
    label: 'UDP', accent: 'var(--udp)', dim: 'var(--udp-dim)',
    nodes: [
      { type: 8  as const, label: 'UDP In Device',     desc: 'Listen on a UDP port',    icon: '⊲' },
      { type: 9  as const, label: 'UDP Out Device',    desc: 'Send to a UDP target',    icon: '⊳' },
      { type: 21 as const, label: 'UDP Monitor',       desc: 'Display raw UDP packets', icon: '⊞' },
    ],
  },
  {
    label: 'Art-Net', accent: 'var(--artnet)', dim: 'var(--artnet-dim)',
    nodes: [
      { type: 12 as const, label: 'ArtNet In Device',  desc: 'Receive ArtDmx universe', icon: '⊲' },
      { type: 13 as const, label: 'ArtNet Out Device', desc: 'Send ArtDmx universe',    icon: '⊳' },
      { type: 19 as const, label: 'ArtNet Console',    desc: 'Control 512 ArtNet channels', icon: '▤' },
      { type: 18 as const, label: 'ArtNet Monitor',    desc: 'Display ArtNet channels',     icon: '⊞' },
    ],
  },
  {
    label: 'DMX', accent: 'var(--dmx)', dim: 'var(--dmx-dim)',
    nodes: [
      { type: 14 as const, label: 'DMX In Device',  desc: 'Receive DMX via Enttec Pro', icon: '⊲' },
      { type: 15 as const, label: 'DMX Out Device', desc: 'Send DMX via Enttec Pro',    icon: '⊳' },
      { type: 17 as const, label: 'DMX Console',    desc: 'Control 512 DMX channels',   icon: '▤' },
      { type: 16 as const, label: 'DMX Monitor',    desc: 'Display 512 DMX channels',   icon: '⊞' },
    ],
  },
  {
    label: 'OSC', accent: 'var(--osc)', dim: 'var(--osc-dim)',
    nodes: [
      { type: 10 as const, label: 'OSC In Device',  desc: 'Receive OSC messages on a UDP port', icon: '⊲' },
      { type: 11 as const, label: 'OSC Out Device', desc: 'Send OSC messages to a target',       icon: '⊳' },
      { type: 20 as const, label: 'OSC Monitor',    desc: 'Display OSC messages',                icon: '⊞' },
    ],
  },
  {
    label: 'MQTT', accent: 'var(--mqtt)', dim: 'var(--mqtt-dim)',
    nodes: [
      { type: 22 as const, label: 'MQTT Subscribe', desc: 'Subscribe to a broker topic', icon: '⊲' },
      { type: 23 as const, label: 'MQTT Publish',   desc: 'Publish to a broker topic',   icon: '⊳' },
      { type: 24 as const, label: 'MQTT Monitor',   desc: 'Display MQTT messages',       icon: '⊞' },
      { type: 25 as const, label: 'MQTT Console',   desc: 'Manual topic+payload composer', icon: '⊟' },
    ],
  },
] as const;

// Maps the backend's value-type tag strings (from PaxInfo.valueInputTypes/
// valueOutputTypes — "generic"/"mqtt"/"osc"/"dmx"/"udp"/"artnet"/"midi") to
// the {type, label} shape detectPaxTheme's portGroupKey expects, so the
// sidebar can build a "ports-like" array from a PaxInfo entry's raw counts
// and call the exact same auto-detection algorithm a placed node uses —
// found this was needed after noticing the sidebar was colouring by a
// fixed per-paxType lookup, producing a real mismatch against placed
// nodes' genuinely auto-detected colour.
const VALUE_TAG_TO_PORT: Record<string, { type: string; label: string }> = {
  generic: { type: 'value', label: 'Value' },
  mqtt:    { type: 'mqtt',  label: 'MQTT' },
  osc:     { type: 'osc',   label: 'OSC' },
  dmx:     { type: 'dmx',  label: 'DMX' },
  udp:     { type: 'udp',   label: 'UDP' },
  artnet:  { type: 'dmx',  label: 'ArtDMX' },
  midi:    { type: 'midi', label: 'MIDI' },
};

function paxInfoToPorts (p: PaxInfo): { type: string; label: string; direction: string }[] {
  const ports: { type: string; label: string; direction: string }[] = [];
  if (p.audioInputs  > 0) ports.push({ type: 'audio', label: 'Audio', direction: 'input' });
  if (p.audioOutputs > 0) ports.push({ type: 'audio', label: 'Audio', direction: 'output' });
  if (p.midiInputs   > 0) ports.push({ type: 'midi',  label: 'MIDI',  direction: 'input' });
  if (p.midiOutputs  > 0) ports.push({ type: 'midi',  label: 'MIDI',  direction: 'output' });
  (p.valueInputTypes  ?? []).forEach(tag => {
    const m = VALUE_TAG_TO_PORT[tag] ?? VALUE_TAG_TO_PORT.generic;
    ports.push({ ...m, direction: 'input' });
  });
  (p.valueOutputTypes ?? []).forEach(tag => {
    const m = VALUE_TAG_TO_PORT[tag] ?? VALUE_TAG_TO_PORT.generic;
    ports.push({ ...m, direction: 'output' });
  });
  return ports;
}

// Grouped by detected category (via detectPaxCategoryKey), not raw
// paxType — a Pax's declared descriptor nodeType (1-4) is just a rough
// starting point for its default port layout, not what it actually does;
// grouping by that instead of the real auto-detected category put e.g. a
// pure-sink Pax with mixed input types (falling back to Hybrid/orange)
// in the same section as genuine Converters, purely because both happen
// to have nodeType=4. category key must match detectPaxCategoryKey's
// return values exactly.
const PLUGIN_GROUPS = [
  { label: 'MIDI',      category: 'midi',      accent: 'var(--midi)',    dim: 'var(--midi-dim)'    },
  { label: 'Audio',     category: 'audio',     accent: 'var(--audio)',   dim: 'var(--audio-dim)'   },
  { label: 'Hybrid',    category: 'hybrid',    accent: 'var(--av)',      dim: 'var(--av-dim)'      },
  { label: 'Converter', category: 'converter', accent: 'var(--value)',  dim: 'var(--value-dim)'  },
  { label: 'OSC',       category: 'osc',       accent: 'var(--osc)',     dim: 'var(--osc-dim)'     },
  { label: 'DMX',       category: 'dmx',       accent: 'var(--dmx)',     dim: 'var(--dmx-dim)'     },
  { label: 'ArtNet',    category: 'artnet',    accent: 'var(--artnet)',  dim: 'var(--artnet-dim)'  },
  { label: 'MQTT',      category: 'mqtt',      accent: 'var(--mqtt)',    dim: 'var(--mqtt-dim)'    },
  { label: 'UDP',       category: 'udp',       accent: 'var(--udp)',     dim: 'var(--udp-dim)'     },
  { label: 'Generic',   category: 'generic',   accent: 'var(--generic)', dim: 'var(--generic-dim)' },
];

function DragItem({ nodeType, label, desc, accent, dim, icon, paxName = '', paxInfo }: {
  nodeType:   number;
  label:      string;
  desc:       string;
  accent:     string;
  dim:        string;
  icon:       string;
  paxName?: string;
  paxInfo?: PaxInfo;
}) {
  // FIXED (2026-08-12): resolved once per render, used only where a
  // hex-alpha suffix gets concatenated below (the hover glow) — same bug
  // class as NodeSelect.tsx. accent arrives as a raw CSS var() string
  // from every caller (the protocol group definitions above, and
  // detectPaxTheme for Pax items).
  const accentHex = resolveCssColor(accent);

  const onDragStart = (e: DragEvent<HTMLDivElement>) => {
    e.dataTransfer.setData('text/plain', JSON.stringify({
      nodeType:  paxName ? 0 : nodeType,
      paxName,
      paxType:   paxName ? nodeType : 0,
    }));
    e.dataTransfer.effectAllowed = 'copy';
  };

  const { setHint } = useContext(HintContext);
  const nodeHint = NODE_HINTS[paxName ?? ''] ?? NODE_HINTS[label] ?? NODE_HINTS[label.toUpperCase()] ?? null;
  const buildHint = () => {
    if (paxInfo) {
      const categoryKey = detectPaxCategoryKey(paxInfoToPorts(paxInfo), paxInfo.colourCategory);
      const CATEGORY_DISPLAY: Record<string,string> = {
        midi:'MIDI', audio:'Audio', hybrid:'Hybrid', converter:'Converter',
        osc:'OSC', dmx:'DMX', artnet:'ArtNet', mqtt:'MQTT', udp:'UDP', generic:'Generic',
      };
      const typeLabel = CATEGORY_DISPLAY[categoryKey] ?? 'Plugin';
      const ports = [];
      if (paxInfo.audioInputs)  ports.push(`${paxInfo.audioInputs} audio in`);
      if (paxInfo.audioOutputs) ports.push(`${paxInfo.audioOutputs} audio out`);
      if (paxInfo.midiInputs)   ports.push(`${paxInfo.midiInputs} MIDI in`);
      if (paxInfo.midiOutputs)  ports.push(`${paxInfo.midiOutputs} MIDI out`);
      if (paxInfo.valueInputs)  ports.push(`${paxInfo.valueInputs} value in`);
      if (paxInfo.valueOutputs) ports.push(`${paxInfo.valueOutputs} value out`);
      return {
        title: label,
        body: (nodeHint?.body ?? '') +
              `

v${paxInfo.version} · ${paxInfo.vendor}
Type: ${typeLabel} · ${ports.join(', ')}`
      };
    }
    return nodeHint;
  };

  return (
    <div
      draggable onDragStart={onDragStart}
      style={{
        display: 'flex', alignItems: 'center', gap: 10,
        padding: '7px 10px', borderRadius: 'var(--radius)',
        border: '1px solid var(--border)', background: 'var(--surface)',
        cursor: 'grab', userSelect: 'none',
        transition: 'border-color .15s, background .15s, box-shadow .15s',
      }}
      onMouseEnter={e => {
        const el = e.currentTarget as HTMLDivElement;
        el.style.borderColor = accent; el.style.background = dim;
        el.style.boxShadow = `0 0 12px ${accentHex}44`;
        const h = buildHint(); if (h) setHint(h);
      }}
      onMouseLeave={e => {
        const el = e.currentTarget as HTMLDivElement;
        el.style.borderColor = 'var(--border)'; el.style.background = 'var(--surface)';
        el.style.boxShadow = 'none';
        setHint(null);
      }}
    >
      <div style={{
        width: 28, height: 28, borderRadius: 4, flexShrink: 0,
        background: dim, border: `1px solid ${accent}`,
        display: 'flex', alignItems: 'center', justifyContent: 'center',
        fontSize: 14, color: accent,
      }}>
        {icon}
      </div>
      <div style={{ overflow: 'hidden' }}>
        <div style={{
          fontFamily: "'Syne', sans-serif", fontWeight: 600, fontSize: 10,
          color: accent, letterSpacing: '0.06em',
          whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
        }}>
          {label}
        </div>
        <div style={{ fontSize: 9, color: 'var(--text-muted)', marginTop: 1 }}>{desc}</div>
      </div>
    </div>
  );
}

function Section({ label, accent, children, defaultOpen = true }: {
  label: string; accent: string; children: React.ReactNode; defaultOpen?: boolean;
}) {
  const [open, setOpen] = useState(defaultOpen);
  return (
    <div style={{ borderTop: '1px solid var(--border)' }}>
      <button onClick={() => setOpen(v => !v)} style={{
        width: '100%', display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        padding: '7px 12px', background: 'none', border: 'none', cursor: 'pointer',
      }}>
        <span style={{
          fontSize: 9, fontWeight: 700, letterSpacing: '0.12em',
          textTransform: 'uppercase', color: accent,
        }}>
          {label}
        </span>
        <span style={{
          fontSize: 8, color: 'var(--text-muted)', display: 'inline-block',
          transition: 'transform 0.2s', transform: open ? 'rotate(180deg)' : 'none',
        }}>▼</span>
      </button>
      {open && (
        <div style={{ padding: '0 8px 8px', display: 'flex', flexDirection: 'column', gap: 5 }}>
          {children}
        </div>
      )}
    </div>
  );
}

export default function Sidebar() {
  const { isStandalone } = useContext(DawContext);
  const [paxItems, setPaxItems] = useState<PaxInfo[]>([]);
  useEffect(() => { Bridge.onPaxList(list => setPaxItems(list)); }, []);
  const hasPax = paxItems.length > 0;

  return (
    <aside style={{
      width: 210, background: 'var(--surface2)', borderRight: '1px solid var(--border)',
      display: 'flex', flexDirection: 'column', flexShrink: 0, overflow: 'hidden',
    }}>
      {/* Scrollable node list */}
      <div style={{ flex: 1, overflowY: 'auto', overflowX: 'hidden' }}>

      {/* Header */}
      <div style={{ padding: '16px 14px 12px', borderBottom: '1px solid var(--border)' }}>
        <div style={{
          fontFamily: "'Syne', sans-serif", fontWeight: 800,
          fontSize: 15, color: 'var(--text)', letterSpacing: '0.06em',
        }}>PATCHY</div>
        <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 4 }}>Drag nodes onto canvas</div>
      </div>

      {/* Built-in label */}
      <div style={{ padding: '8px 12px 0' }}>
        <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase' }}>
          Built-in
        </div>
      </div>

      {/* Built-in groups */}
      {BUILTIN_GROUPS.map(group => (
        <Section key={group.label} label={group.label} accent={group.accent}>
          {group.nodes.map(n => (
            <DragItem key={n.type} nodeType={n.type} label={n.label} desc={n.desc}
              accent={group.accent} dim={group.dim} icon={n.icon} />
          ))}
        </Section>
      ))}

      {/* Pax */}
      {hasPax && (
        <>
          <div style={{ padding: '10px 12px 0', borderTop: '1px solid var(--border)', marginTop: 4 }}>
            <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase' }}>
              Pax
            </div>
          </div>
          {PLUGIN_GROUPS.map(group => {
            const items = paxItems.filter(p => detectPaxCategoryKey(paxInfoToPorts(p), p.colourCategory) === group.category);
            if (items.length === 0) return null;
            return (
              <Section key={group.label} label={group.label} accent={group.accent}>
                {items.map(p => {
                  const itemTheme = detectPaxTheme(paxInfoToPorts(p), p.colourCategory);
                  return (
                    <DragItem key={p.name} nodeType={p.nodeType} label={p.name}
                      desc={p.vendor || 'Pax'} accent={itemTheme.accent} dim={itemTheme.dim}
                      icon="⬡" paxName={p.name} paxInfo={p} />
                  );
                })}
              </Section>
            );
          })}
        </>
      )}

      {/* No Pax hint */}
      {!hasPax && (
        <div style={{
          padding: '8px 14px', fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.6,
          borderTop: '1px solid var(--border)', marginTop: 4,
        }}>
          No Pax found.<br />
          Drop <span style={{ color: 'var(--text-dim)' }}>.dylib / .so / .dll</span><br />
          into the Pax folder.
        </div>
      )}

      {/* Port legend */}
      <div style={{ marginTop: 'auto', padding: '12px 14px', borderTop: '1px solid var(--border)' }}>
        <div style={{ fontSize: 10, color: 'var(--text-muted)', marginBottom: 8, letterSpacing: '0.08em' }}>
          PORT TYPES
        </div>
        {[
          { color: 'var(--midi)',    label: 'MIDI' },
          { color: 'var(--audio)',   label: 'Audio' },
          { color: 'var(--osc)',     label: 'OSC' },
          { color: 'var(--dmx)',     label: 'DMX' },
          { color: 'var(--artnet)',  label: 'ArtNet' },
          { color: 'var(--mqtt)',    label: 'MQTT' },
          { color: 'var(--udp)',     label: 'UDP' },
          { color: 'var(--generic)', label: 'Value' },
        ].map(({ color, label }) => (
          <div key={label} style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
            <div style={{
              width: 10, height: 10, borderRadius: '50%', background: color,
              border: '2px solid var(--bg)', boxShadow: `0 0 6px ${color}`,
            }} />
            <span style={{ fontSize: 11, color: 'var(--text-dim)' }}>{label}</span>
          </div>
        ))}
        <div style={{ marginTop: 10, fontSize: 10, color: 'var(--text-muted)', lineHeight: 1.5 }}>
          Connect ○ out → in ○<br />Same port type only
        </div>
      </div>

      </div>
      {/* Fixed hint panel — never scrolls */}
      <HintPanel />
    </aside>
  );
}
