/**
 * NodeShared.tsx
 *
 * Shared hooks and components used across all Patchy node types.
 * Eliminates duplicated boilerplate from MidiMonitorNode, AudioMonitorNode,
 * MidiKeyboardNode and GenericNode.
 */

import { useCallback, useState, useEffect, useContext } from 'react';
import { Handle, Position, useReactFlow, useUpdateNodeInternals } from '@xyflow/react';
import { Settings, X } from 'lucide-react';
import { Bridge } from './Bridge';
import { HintContext, NODE_HINTS, BUTTON_HINTS } from './HintPanel';

// ── NodeHandle ───────────────────────────────────────────────────────────────
/**
 * Standard port handle with consistent sizing and styling.
 * colour: e.g. 'var(--midi)' or 'var(--audio)'
 */
export function NodeHandle ({ nodeId, label, direction, colour, index = 0, total = 1, portBodyRef, portId, offset = 0 }: {
  nodeId:       string;
  label:        string;
  direction:    'in' | 'out';
  colour:       string;
  index?:       number;
  total?:       number;
  portBodyRef?: React.RefObject<HTMLDivElement | null>;
  portId?:      string;
  offset?:      number;
}) {
  const isInput = direction === 'in';
  const [topVal, setTopVal] = useState<string>('50%');

  useEffect(() => {
    if (!portBodyRef?.current) { setTopVal('50%'); return; }
    const measure = () => {
      const el = portBodyRef.current;
      if (!el) { setTopVal('50%'); return; }
      // Place port 12px below the header (headerHeight = el.offsetTop)
      const headerHeight = el.offsetTop;
      const spacing      = total <= 1 ? 0 : 14 * index;
      setTopVal(`${headerHeight + 12 + spacing + offset}px`);
    };
    measure();
    const observer = new ResizeObserver(measure);
    observer.observe(portBodyRef.current!);
    return () => observer.disconnect();
  }, [portBodyRef, portBodyRef?.current, total, index]);

  return (
    <Handle
      type={isInput ? 'target' : 'source'}
      position={isInput ? Position.Left : Position.Right}
      id={portId ?? `${nodeId}_${label}_${direction}`}
      style={{
        background:  colour,
        width:       10,
        height:      10,
        border:      '2px solid var(--bg)',
        top:         portBodyRef ? topVal : '50%',
      }}
    />
  );
}

// ── Checkbox ─────────────────────────────────────────────────────────────────
/**
 * Custom checkbox — replaces native <input type="checkbox"> which ignores
 * CSS overrides in JUCE's embedded WebKit.
 */
export function Checkbox ({ checked, onChange, label, accent = 'var(--text-dim)' }: {
  checked:  boolean;
  onChange: (v: boolean) => void;
  label?:   string;
  accent?:  string;
}) {
  return (
    <label style={{ display: 'flex', alignItems: 'center', gap: 6,
                    cursor: 'pointer', userSelect: 'none' }}>
      <div
        onClick={() => onChange(!checked)}
        style={{
          width:        13,
          height:       13,
          border:       `1px solid ${checked ? accent : 'var(--border)'}`,
          borderRadius: 3,
          background:   checked ? accent : 'var(--surface)',
          flexShrink:   0,
          position:     'relative',
          transition:   'background 0.15s, border-color 0.15s',
        }}
      >
        {checked && (
          <div style={{
            position:      'absolute',
            left:          3,
            top:           0,
            width:         5,
            height:        8,
            borderRight:   '2px solid var(--bg)',
            borderBottom:  '2px solid var(--bg)',
            transform:     'rotate(45deg)',
          }} />
        )}
      </div>
      {label && (
        <span style={{ fontSize: 10, color: 'var(--text-dim)' }}>{label}</span>
      )}
    </label>
  );
}

// ── useNodeSettings ───────────────────────────────────────────────────────────
/**
 * Manages settings panel open/close state, including elevating the node's
 * z-index via ReactFlow's updateNode so the settings panel always appears
 * above other nodes.
 */
export function useNodeSettings (id: string) {
  const { updateNode } = useReactFlow();
  const [showSettings, setShowSettings] = useState(false);

  const openSettings = useCallback(() => {
    setShowSettings(true);
    updateNode(id, { style: { zIndex: 9999 } });
  }, [id, updateNode]);

  const closeSettings = useCallback(() => {
    setShowSettings(false);
    updateNode(id, { style: { zIndex: undefined } });
  }, [id, updateNode]);

  const toggleSettings = useCallback(() => {
    if (showSettings) closeSettings();
    else              openSettings();
  }, [showSettings, openSettings, closeSettings]);

  return { showSettings, openSettings, closeSettings, toggleSettings };
}

// ── useNodeDelete ─────────────────────────────────────────────────────────────
/**
 * Returns a handleDelete callback that notifies the C++ bridge and removes
 * the node from the ReactFlow graph.
 */
export function useNodeDelete (id: string) {
  const { deleteElements } = useReactFlow();

  const handleDelete = useCallback(() => {
    Bridge.removeNode(id);
    deleteElements({ nodes: [{ id }] });
  }, [id, deleteElements]);

  return { handleDelete };
}

// ── useNodeCollapsed ─────────────────────────────────────────────────────────
/**
 * Manages collapsed/expanded state for a node.
 * When collapsed, the node shrinks to just its header bar.
 */
export function useNodeCollapsed (id: string, forceCollapsed?: boolean) {
  const { updateNode } = useReactFlow();
  const updateNodeInternals = useUpdateNodeInternals();
  const [collapsed, setCollapsed] = useState(false);

  // Respond to global fold/unfold toggle
  useEffect(() => {
    if (forceCollapsed === undefined) return;
    setCollapsed(forceCollapsed);
    updateNode(id, { style: { height: forceCollapsed ? 'auto' : undefined } });
    setTimeout(() => updateNodeInternals(id), 1);
  }, [forceCollapsed]);

  const toggleCollapsed = useCallback(() => {
    setCollapsed(c => {
      const next = !c;
      updateNode(id, { style: { height: next ? 'auto' : undefined } });
      setTimeout(() => updateNodeInternals(id), 1);
      return next;
    });
  }, [id, updateNode, updateNodeInternals]);

  return { collapsed, toggleCollapsed };
}

// ── NodeHeaderButton ──────────────────────────────────────────────────────────
/**
 * A small header button with consistent styling.
 * Use `danger` for the delete button (red hover).
 * Use `active` + `activeAccent` for toggle buttons (settings, pause etc.)
 */
export function NodeHeaderButton ({
  onClick, title, active = false, activeAccent, danger = false, onHint, children,
}: {
  onClick:       () => void;
  title?:        string;
  active?:       boolean;
  activeAccent?: string;
  danger?:       boolean;
  onHint?:       { onMouseEnter: () => void; onMouseLeave: () => void };
  children:      React.ReactNode;
}) {
  const accent = activeAccent ?? 'var(--accent)';
  return (
    <button
      onClick={onClick}
      title={title}
      style={{
        background:   active ? `color-mix(in srgb, ${accent} 20%, transparent)` : 'transparent',
        border:       `1px solid ${active ? accent : 'var(--border)'}`,
        borderRadius: 3,
        color:        active ? accent : 'var(--text-muted)',
        cursor:       'pointer',
        fontSize:     19,
        width:        22,
        height:       22,
        padding:      0,
        display:      'flex',
        alignItems:   'center',
        justifyContent: 'center',
        transition:   'color 0.15s, border-color 0.15s',
      }}
      onMouseEnter={e => {
        const b = e.currentTarget;
        if (danger) {
          b.style.color       = 'var(--danger)';
          b.style.borderColor = 'var(--danger)';
        } else if (!active) {
          b.style.color       = 'var(--text)';
          b.style.borderColor = 'var(--border-hi)';
        }
        onHint?.onMouseEnter?.();
      }}
      onMouseLeave={e => {
        const b = e.currentTarget;
        b.style.color       = active ? accent : 'var(--text-muted)';
        b.style.borderColor = active ? accent : 'var(--border)';
      }}
    >
      {children}
    </button>
  );
}

// ── nodeContainerStyle ────────────────────────────────────────────────────────
/** Standard node container style — border, shadow, selection highlight. */
export function nodeContainerStyle (
  accent: string, selected: boolean,
  opts?: { bg?: string; glow?: string }
): React.CSSProperties {
  const bg   = opts?.bg   ?? 'var(--surface2)';
  const glow = opts?.glow ?? `${accent}33`;
  return {
    background:   bg,
    border:       `1px solid ${selected ? accent : 'var(--border)'}`,
    borderTop:    `3px solid ${accent}`,
    borderRadius: 'var(--radius)',
    fontFamily:   "'JetBrains Mono', monospace",
    boxShadow:    selected
      ? `0 0 0 1px ${accent}, 0 8px 32px ${glow}`
      : '0 4px 16px rgba(0,0,0,.5)',
    transition:   'box-shadow .15s, border-color .15s',
    position:     'relative' as const,
  };
}

// ── Shared style constants ────────────────────────────────────────────────────
export const settingsPanelStyle: React.CSSProperties = {
  padding: '8px 10px',
  fontSize: 9,
  color: 'var(--text)',
  borderTop: '1px solid var(--border)',
};

export const sectionDividerStyle: React.CSSProperties = {
  borderTop: '1px solid var(--border)',
  marginTop: 4,
  paddingTop: 4,
};

// ── NodeCollapseArrow ─────────────────────────────────────────────────────────
/** Reusable collapse/expand arrow — used in all custom node headers. */
export function NodeCollapseArrow ({ collapsed, accent }: { collapsed: boolean; accent: string }) {
  return (
    <span style={{ color: accent, opacity: 0.7, display: 'inline-block',
      transform: collapsed ? 'rotate(0deg)' : 'rotate(90deg)',
      transition: 'transform 0.2s' }}>
      <svg width="8" height="10" viewBox="0 0 8 10" style={{ display: 'block' }}>
        <polygon points="0,0 8,5 0,10" fill="currentColor" />
      </svg>
    </span>
  );
}

// ── NodeHeader ────────────────────────────────────────────────────────────────
/**
 * Standard node header bar: title on the left, action buttons on the right.
 * Pass extra buttons (pause, clear etc.) via the `children` slot — they appear
 * between the title and the built-in settings + delete buttons.
 */
export function NodeHeader ({
  title, accent, showSettings, onToggleSettings, onDelete, collapsed, onToggleCollapsed, children,
}: {
  title:              string;
  accent:             string;
  showSettings:       boolean;
  onToggleSettings:   () => void;
  onDelete:           () => void;
  collapsed?:         boolean;
  onToggleCollapsed?: () => void;
  children?:          React.ReactNode;
}) {
  const { setHint } = useContext(HintContext);
  const nodeHint = NODE_HINTS[title] ?? null;

  return (
    <div
      onDoubleClick={onToggleCollapsed}
      onMouseEnter={e => {
        // Only set node hint if entering from outside the header (not from a child button)
        if (!e.currentTarget.contains(e.relatedTarget as Node) && nodeHint)
          setHint(nodeHint);
      }}
      onMouseLeave={e => {
        // Only clear if leaving the header entirely (not moving to a child button)
        if (!e.currentTarget.contains(e.relatedTarget as Node))
          setHint(null);
      }}
      style={{
        padding:        '6px 8px',
        borderBottom:   collapsed ? 'none' : '1px solid var(--border)',
        display:        'flex',
        alignItems:     'center',
        justifyContent: 'space-between',
        gap:            4,
        cursor:         onToggleCollapsed ? 'pointer' : 'default',
        userSelect:     'none',
      }}>
      {/* Collapse indicator + Title */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
        {onToggleCollapsed && (
          <span
            onMouseEnter={() => setHint(BUTTON_HINTS.fold)}
            onMouseLeave={() => setHint(null)}
            style={{
              fontSize:   14,
              color:      accent,
              opacity:    0.7,
              transition: 'transform 0.2s',
              display:    'inline-block',
              transform:  collapsed ? 'rotate(0deg)' : 'rotate(90deg)',
            }}>
              <svg width="8" height="10" viewBox="0 0 8 10" style={{ display:'block' }}>
                <polygon points="0,0 8,5 0,10" fill="currentColor" />
              </svg>
            </span>
        )}
        <div style={{
          fontSize:     11,
          fontWeight:   700,
          color:        accent,
          letterSpacing:'0.1em',
          fontFamily:   "'Syne', sans-serif",
        }}>
          {title}
        </div>
      </div>

      {/* Action buttons */}
      <div style={{ display: 'flex', gap: 4 }}>
        {/* Extra node-specific buttons (pause, clear, etc.) */}
        {children}

        {/* Settings */}
        <NodeHeaderButton
          onClick={onToggleSettings}
         
          active={showSettings}
          activeAccent={accent}
          onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}
        >
          <Settings size={14} />
        </NodeHeaderButton>

        {/* Delete */}
        <NodeHeaderButton onClick={onDelete} danger
          onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.deleteNode), onMouseLeave: () => setHint(null) }}
        >
          <X size={14} />
        </NodeHeaderButton>
      </div>
    </div>
  );
}

// ── SettingsPanelHeader ───────────────────────────────────────────────────────
/**
 * Reusable header row for settings panels — title + R (reset) + ✕ (close) buttons.
 */
export function SettingsPanelHeader ({ title, onReset, onClose }: {
  title:   string;
  onReset: () => void;
  onClose: () => void;
}) {
  const { setHint } = useContext(HintContext);
  return (
    <div style={{ display: 'flex', justifyContent: 'space-between',
                  alignItems: 'center', marginBottom: 8 }}>
      <span style={{ fontSize: 9, fontWeight: 700, letterSpacing: '0.1em',
                     color: 'var(--text-muted)', textTransform: 'uppercase' }}>
        {title}
      </span>
      <div style={{ display: 'flex', gap: 4 }}>
        <NodeHeaderButton onClick={onReset}
          onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.reset), onMouseLeave: () => setHint(null) }}><span style={{ fontSize: 11, fontWeight: 700 }}>R</span></NodeHeaderButton>
        <NodeHeaderButton onClick={onClose} danger
          onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.close), onMouseLeave: () => setHint(null) }}><X size={13} /></NodeHeaderButton>
      </div>
    </div>
  );
}
