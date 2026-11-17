// PortMenu.tsx — v0.0.989: right-click a port → the wires on it, each with a
// ✕ to disconnect just that one (user: "choose a specific wire to disconnect
// from a port to which a few wires are connected"). Hovering a row selects
// its wire on the canvas, so you see which one it is. Preferences → Wires →
// "Port menu on right-click" turns it off.

import { useEffect, useRef } from 'react';

export interface PortMenuItem {
  edgeId: string;
  text:   string;   // "→ Synth · MIDI In"
}

export interface PortMenuState {
  x: number;
  y: number;
  title:  string;   // "MIDI Out · Matrix"
  colour: string;
  items:  PortMenuItem[];
}

export default function PortMenu ({ menu, onRemove, onHover, onClose }: {
  menu: PortMenuState;
  onRemove: (edgeId: string) => void;
  onHover:  (edgeId: string | null) => void;
  onClose:  () => void;
}) {
  const ref = useRef<HTMLDivElement> (null);
  useEffect (() => {
    const down = (e: MouseEvent) => { if (ref.current && ! ref.current.contains (e.target as Node)) onClose (); };
    const key  = (e: KeyboardEvent) => { if (e.key === 'Escape') onClose (); };
    window.addEventListener ('mousedown', down, true);
    window.addEventListener ('keydown', key);
    window.addEventListener ('wheel', onClose, { passive: true });
    return () => {
      window.removeEventListener ('mousedown', down, true);
      window.removeEventListener ('keydown', key);
      window.removeEventListener ('wheel', onClose);
    };
  }, [onClose]);

  // keep it on screen
  const left = Math.min (menu.x + 6, window.innerWidth  - 260);
  const top  = Math.min (menu.y + 6, window.innerHeight - 40 - menu.items.length * 24);

  return (
    <div ref={ref} onContextMenu={e => e.preventDefault ()} onMouseLeave={() => onHover (null)}
      style={{ position: 'fixed', left, top, zIndex: 3000, minWidth: 180, maxWidth: 250, padding: '4px 0',
               background: 'var(--surface)', border: '1px solid var(--border-hi)', borderRadius: 6,
               boxShadow: '0 8px 24px rgba(0,0,0,.45)', fontFamily: "'JetBrains Mono', monospace", userSelect: 'none' }}>
      <div style={{ fontSize: 9, letterSpacing: '0.06em', color: menu.colour, padding: '3px 10px 5px',
                    borderBottom: '1px solid var(--border)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
        {menu.title} — {menu.items.length} wire{menu.items.length === 1 ? '' : 's'}
      </div>
      {menu.items.map (it => (
        <div key={it.edgeId} className="port-menu-row" onMouseEnter={() => onHover (it.edgeId)}
          style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '3px 6px 3px 10px', fontSize: 10, color: 'var(--text)' }}>
          <span style={{ flex: 1, minWidth: 0, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }} title={it.text}>{it.text}</span>
          <span onClick={() => onRemove (it.edgeId)} title="Disconnect this wire (undoable)"
            style={{ cursor: 'pointer', color: '#ef4444', fontSize: 14, fontWeight: 700, lineHeight: '14px', padding: '0 4px',
                     fontFamily: 'system-ui, sans-serif' }}>×</span>
        </div>
      ))}
    </div>
  );
}
