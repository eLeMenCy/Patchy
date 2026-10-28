// Toasts.tsx — v0.0.958: short messages in the bottom-right corner (first
// user: the Pax folder autoscan — "Pax added: …", "Pax removed: …").
// Each one fades out after a few seconds; click to dismiss it sooner.
import { useEffect, useState } from 'react';
import { Bridge, ToastMessage } from './Bridge';

interface Shown extends ToastMessage { id: number; leaving: boolean }
const COLOURS: Record<string, string> = { ok: '#22c55e', info: 'var(--accent)', warn: '#f59e0b', error: '#ef4444' };
let nextId = 1;

export default function Toasts () {
  const [list, setList] = useState<Shown[]> ([]);
  useEffect (() => Bridge.onToast (t => {
    const id = nextId++;
    setList (l => [...l.slice (-4), { ...t, id, leaving: false }]);   // at most 5 at once
    const life = t.kind === 'error' ? 8000 : 5000;
    window.setTimeout (() => setList (l => l.map (x => x.id === id ? { ...x, leaving: true } : x)), life);
    window.setTimeout (() => setList (l => l.filter (x => x.id !== id)), life + 400);
  }), []);
  if (list.length === 0) return null;
  return (
    <div style={{ position: 'absolute', right: 16, bottom: 16, zIndex: 100000, display: 'flex', flexDirection: 'column',
                  gap: 6, alignItems: 'flex-end', pointerEvents: 'none', maxWidth: 'calc(100% - 32px)' }}>
      {list.map (t => (
        <div key={t.id} onClick={() => setList (l => l.filter (x => x.id !== t.id))}
          style={{ pointerEvents: 'auto', cursor: 'pointer', maxWidth: 360, padding: '7px 12px', borderRadius: 'var(--radius)',
                   background: 'var(--surface2)', border: '1px solid var(--border-hi)',
                   borderLeft: `3px solid ${COLOURS[t.kind] ?? COLOURS.info}`, boxShadow: '0 6px 24px rgba(0,0,0,.55)',
                   fontSize: 10, color: 'var(--text)', fontFamily: "'JetBrains Mono', monospace", lineHeight: 1.4,
                   opacity: t.leaving ? 0 : 1, transform: t.leaving ? 'translateY(6px)' : 'none', transition: 'opacity .35s, transform .35s' }}>
          {t.text}
        </div>
      ))}
    </div>
  );
}
