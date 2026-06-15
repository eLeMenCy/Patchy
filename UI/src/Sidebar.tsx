import { DragEvent, useEffect, useState, useContext } from 'react';
import { DawContext } from './DawContext';
import { HintPanel, NODE_HINTS, HintContext } from './HintPanel';
import { Bridge, PaxInfo } from './Bridge';

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
      { type: 6 as const, label: 'Audio Monitor',    desc: 'Waveform display',        icon: '◎' },
    ],
  },
] as const;

const PLUGIN_GROUPS = [
  { label: 'MIDI',         ngaType: 1, accent: 'var(--midi)',  dim: 'var(--midi-dim)'  },
  { label: 'Audio',        ngaType: 2, accent: 'var(--audio)', dim: 'var(--audio-dim)' },
  { label: 'Hybrid',       ngaType: 3, accent: 'var(--av)',    dim: 'var(--av-dim)'    },
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
  const onDragStart = (e: DragEvent<HTMLDivElement>) => {
    e.dataTransfer.setData('text/plain', JSON.stringify({
      nodeType:  paxName ? 0 : nodeType,
      paxName,
      ngaType:   paxName ? nodeType : 0,
    }));
    e.dataTransfer.effectAllowed = 'copy';
  };

  const { setHint } = useContext(HintContext);
  const nodeHint = NODE_HINTS[paxName ?? ''] ?? NODE_HINTS[label] ?? NODE_HINTS[label.toUpperCase()] ?? null;
  const buildHint = () => {
    if (paxInfo) {
      const typeLabel = paxInfo.nodeType === 1 ? 'MIDI' : paxInfo.nodeType === 2 ? 'Audio' : 'Hybrid';
      const ports = [];
      if (paxInfo.audioInputs)  ports.push(`${paxInfo.audioInputs} audio in`);
      if (paxInfo.audioOutputs) ports.push(`${paxInfo.audioOutputs} audio out`);
      if (paxInfo.midiInputs)   ports.push(`${paxInfo.midiInputs} MIDI in`);
      if (paxInfo.midiOutputs)  ports.push(`${paxInfo.midiOutputs} MIDI out`);
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
        el.style.boxShadow = `0 0 12px ${accent}44`;
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
            const items = paxItems.filter(p => p.nodeType === group.ngaType);
            if (items.length === 0) return null;
            return (
              <Section key={group.label} label={group.label} accent={group.accent}>
                {items.map(p => (
                  <DragItem key={p.name} nodeType={p.nodeType} label={p.name}
                    desc={p.vendor || 'Pax'} accent={group.accent} dim={group.dim}
                    icon="⬡" paxName={p.name} paxInfo={p} />
                ))}
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
        {[{ color: 'var(--midi)', label: 'MIDI' }, { color: 'var(--audio)', label: 'Audio' }].map(({ color, label }) => (
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
