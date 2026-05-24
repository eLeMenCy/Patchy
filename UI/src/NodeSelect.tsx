import { useEffect, useRef, useState } from 'react';

// ── Custom select component ──────────────────────────────────────────────────
function NodeSelect({ value, onChange, options, disabled, accent, showEmpty = true, onOptionHover }: {
  value:          string;
  onChange:       (v: string) => void;
  options:        { id: string; name: string; disabled?: boolean; warning?: boolean; hint?: { title: string; body: string } }[];
  disabled?:      boolean;
  accent?:        string;
  showEmpty?:     boolean;
  onOptionHover?: (hint: { title: string; body: string } | null) => void;
}) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);

  const accentColor = accent ?? 'var(--midi)';
  const selected = options.find(o => o.id === value);
  const displayLabel = selected?.name ?? '— select device —';
  const isEmpty = options.length === 0;

  // Close on outside click
  useEffect(() => {
    if (!open) return;
    const handler = (e: MouseEvent) => {
      if (ref.current && !ref.current.contains(e.target as Node))
        setOpen(false);
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [open]);

  const baseStyle: React.CSSProperties = {
    width:       '100%',
    position:    'relative',
    userSelect:  'none',
  };

  const triggerStyle: React.CSSProperties = {
    display:        'flex',
    alignItems:     'center',
    justifyContent: 'space-between',
    padding:        '4px 8px',
    background:     disabled || isEmpty ? 'var(--surface)' : open ? 'rgba(28, 32, 48, 0.92)' : 'transparent',
    border:         `1px solid ${open ? accentColor : 'var(--border)'}`,
    borderRadius:   4,
    color:          disabled || isEmpty ? 'var(--text-muted)' : 'var(--text)',
    fontSize:       10,
    fontFamily:     "'JetBrains Mono', monospace",
    cursor:         disabled || isEmpty ? 'not-allowed' : 'pointer',
    opacity:        disabled || isEmpty ? 0.45 : 1,
    boxShadow:      open ? `0 0 0 1px ${accentColor}44` : 'none',
    transition:     'border-color 0.15s, box-shadow 0.15s',
  };

  const dropdownStyle: React.CSSProperties = {
    position:   'absolute',
    top:        'calc(100% + 3px)',
    left:       0,
    right:      0,
    background: 'rgba(28, 32, 48, 0.92)',
    border:     `1px solid ${accentColor}`,
    borderRadius: 4,
    boxShadow:  `0 8px 24px rgba(0,0,0,0.6), 0 0 0 1px ${accentColor}22`,
    zIndex:     9999,
    overflow:   'hidden',
    maxHeight:  180,
    overflowY:  'auto',
  };

  return (
    <div
      ref={ref}
      style={baseStyle}
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
    >
      {/* Trigger */}
      <div
        style={triggerStyle}
        onClick={() => { if (!disabled && !isEmpty) setOpen(v => !v); }}
      >
        <span style={{
          overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          color: value ? 'var(--text)' : 'var(--text-muted)',
          letterSpacing: '0.02em',
        }}>
          {isEmpty ? '— No Devices —' : displayLabel}
        </span>
        <span style={{
          color:      open ? accentColor : 'var(--text-muted)',
          fontSize:   8,
          marginLeft: 4,
          flexShrink: 0,
          transition: 'transform 0.15s, color 0.15s',
          transform:  open ? 'rotate(180deg)' : 'none',
          display:    'inline-block',
        }}>▼</span>
      </div>

      {/* Dropdown */}
      {open && (
        <div style={dropdownStyle}>
          {showEmpty && (
            <div
              onClick={() => { onChange(''); setOpen(false); }}
              style={{
                padding:    '5px 8px',
                fontSize:   10,
                fontFamily: "'JetBrains Mono', monospace",
                color:      'var(--text-muted)',
                cursor:     'pointer',
                borderBottom: '1px solid var(--border)',
              }}
              onMouseEnter={e => (e.currentTarget.style.background = 'var(--surface)')}
              onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
            >
              — select device —
            </div>
          )}
          {options.map(opt => (
            <div
              key={opt.id}
              onClick={() => { if (!opt.disabled) { onChange(opt.id); setOpen(false); } }}
              onMouseEnter={e => {
                if (onOptionHover) onOptionHover(opt.hint ?? null);
                if (!opt.disabled) (e.currentTarget as HTMLDivElement).style.background = opt.id === value ? `${accentColor}28` : 'var(--surface)';
              }}
              onMouseLeave={e => {
                if (onOptionHover) onOptionHover(null);
                (e.currentTarget as HTMLDivElement).style.background = opt.id === value ? `${accentColor}18` : 'transparent';
              }}
              style={{
                padding:    '5px 8px',
                fontSize:   10,
                fontFamily: "'JetBrains Mono', monospace",
                color:      opt.disabled && !opt.warning ? 'var(--text-muted)'
                          : opt.warning ? '#f59e0b'
                          : opt.id === value ? accentColor : 'var(--text)',
                background: opt.id === value ? `${accentColor}18` : 'transparent',
                cursor:     opt.disabled ? 'not-allowed' : 'pointer',
                opacity:    opt.disabled && !opt.warning ? 0.6 : 1,
                display:    'flex',
                alignItems: 'center',
                justifyContent: 'space-between',
              }}
            >
              <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                {opt.name}
              </span>
              {opt.disabled && <span style={{ fontSize: 8, opacity: 0.6, marginLeft: 4, flexShrink: 0 }}>in use</span>}
              {opt.id === value && !opt.disabled && <span style={{ color: accentColor, fontSize: 9, marginLeft: 4, flexShrink: 0 }}>✓</span>}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}


export { NodeSelect };
