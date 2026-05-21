import { useEffect, useRef, useState } from 'react';

// ── Preferences interface — extend here for future settings ───────────────────
export interface GraphPreferences {
  invertZoom: boolean;
  // future: snapToGrid, edgeStyle, backgroundStyle, etc.
}

export const DEFAULT_PREFS: GraphPreferences = {
  invertZoom: false,
};

const STORAGE_KEY = 'nodegraph_prefs';

export function loadPrefs(): GraphPreferences {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) return { ...DEFAULT_PREFS, ...JSON.parse(raw) };
  } catch {}
  return { ...DEFAULT_PREFS };
}

export function savePrefs(prefs: GraphPreferences) {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(prefs)); } catch {}
}

// ── Section header ────────────────────────────────────────────────────────────
function SectionHeader ({ label }: { label: string }) {
  return (
    <div style={{
      fontSize: 9, fontWeight: 700, letterSpacing: '0.12em',
      textTransform: 'uppercase', color: 'var(--text-muted)',
      borderBottom: '1px solid var(--border)', paddingBottom: 5,
      marginBottom: 8, marginTop: 12,
    }}>
      {label}
    </div>
  );
}

// ── Toggle row ────────────────────────────────────────────────────────────────
function ToggleRow ({ label, desc, value, onChange, disabled = false }: {
  label:    string;
  desc?:    string;
  value:    boolean;
  onChange: (v: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <div style={{ display: 'flex', alignItems: 'flex-start',
                  justifyContent: 'space-between', gap: 12, marginBottom: 10,
                  opacity: disabled ? 0.45 : 1, cursor: disabled ? 'not-allowed' : 'auto' }}>
      <div style={{ flex: 1 }}>
        <div style={{ fontSize: 11, color: 'var(--text)', marginBottom: desc ? 2 : 0 }}>
          {label}
        </div>
        {desc && (
          <div style={{ fontSize: 9, color: 'var(--text-muted)', lineHeight: 1.4 }}>
            {desc}
          </div>
        )}
      </div>
      {/* Toggle switch */}
      <div
        onClick={() => !disabled && onChange(!value)}
        style={{
          width: 32, height: 18, borderRadius: 9, flexShrink: 0,
          background: value ? 'var(--accent)' : 'var(--border)',
          position: 'relative', cursor: 'pointer',
          transition: 'background 0.2s',
          boxShadow: value ? '0 0 8px var(--accent)66' : 'none',
        }}
      >
        <div style={{
          position: 'absolute', top: 2,
          left: value ? 16 : 2,
          width: 14, height: 14, borderRadius: '50%',
          background: 'var(--text)', transition: 'left 0.2s',
          boxShadow: '0 1px 3px rgba(0,0,0,0.4)',
        }} />
      </div>
    </div>
  );
}

// ── Main panel ────────────────────────────────────────────────────────────────
export default function PreferencesPanel ({
  prefs, onChange, onClose,
}: {
  prefs:    GraphPreferences;
  onChange: (p: GraphPreferences) => void;
  onClose:  () => void;
}) {
  const ref = useRef<HTMLDivElement>(null);

  // Close on outside click
  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node)) onClose();
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [onClose]);

  const patch = (partial: Partial<GraphPreferences>) => {
    const next = { ...prefs, ...partial };
    onChange(next);
    savePrefs(next);
  };

  return (
    <div
      ref={ref}
      onMouseDown={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 48, right: 0,
        width: 280, zIndex: 99999,
        background: 'var(--surface2)',
        border: '1px solid var(--border-hi)',
        borderRadius: 'var(--radius)',
        boxShadow: '0 8px 32px rgba(0,0,0,.7)',
        padding: '12px 14px',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}
    >
      {/* Header */}
      <div style={{ display: 'flex', justifyContent: 'space-between',
                    alignItems: 'center', marginBottom: 4 }}>
        <div style={{
          fontSize: 12, fontWeight: 700, color: 'var(--text)',
          letterSpacing: '0.08em', fontFamily: "'Syne', sans-serif",
        }}>
          PATCHY PREFERENCES
        </div>
        <button onClick={onClose} style={{
          background: 'none', border: 'none', color: 'var(--text-muted)',
          cursor: 'pointer', fontSize: 14, padding: '0 2px', lineHeight: 1,
        }}>✕</button>
      </div>

      {/* ── Navigation ─────────────────────────────────────────────────── */}
      <SectionHeader label="Navigation" />
      <ToggleRow
        label="Invert zoom direction"
        desc="Coming soon — macOS trackpad gestures require deeper integration"
        value={false}
        onChange={() => {}}
        disabled
      />

      {/* Future sections go here — e.g.:
        <SectionHeader label="Canvas" />
        <SectionHeader label="Connections" />
      */}
    </div>
  );
}
