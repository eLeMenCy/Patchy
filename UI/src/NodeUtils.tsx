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
import { Bridge, PaxInfo } from './Bridge';
import { HintContext, NODE_HINTS, BUTTON_HINTS } from './HintPanel';

// Module-level Pax info map — populated (in App.tsx) when the Pax list
// arrives from the backend. Stores the full PaxInfo entry per paxName
// (not just params) so a placed node's colour auto-detection can look up
// colourCategory — a per-Pax-*type* property (from the registry), not
// per-node-*instance*. Lives here rather than in App.tsx or GenericNode.tsx
// directly: both need it, and App.tsx already imports GenericNode to
// register it as a node type, so declaring it in either would create a
// circular import. NodeUtils.tsx is a shared, lower-level file both
// already depend on safely.
export const _paxInfoMap = new Map<string, PaxInfo>();

// ── Hybrid/Converter colour auto-detection ────────────────────────────────────
// Rule agreed in conversation (see Architecture.md's "Per-port value typing
// mechanism" entry for the full design story): compare the set of port
// types on a node's input side against its output side. A type present on
// only one side means the node is converting that type — Converter. If
// every type mirrors on both sides: exactly one distinct type total gets
// its own native colour; more than one gets Hybrid. Fully automatic from
// the node's own declared ports — nothing a Pax author has to specify,
// unless the node is a pure source or pure sink (nothing to compare input
// against output at all) — the one case the rule genuinely can't resolve,
// where colourCategory (from the registry, looked up via paxName) is
// consulted instead.
//
// Exported (and living here, not in GenericNode.tsx) so both GenericNode.tsx
// (placed nodes) and Sidebar.tsx (the pre-placement listing) can use the
// exact same algorithm — moved here after finding the sidebar was still
// colouring by a fixed per-ngaType lookup, producing a real, visible
// mismatch against a placed node's genuinely auto-detected colour.
//
// ArtNet and plain DMX share PortType::DMX internally (see the backend's
// PaxValueType comment) — distinguished here the same way the backend
// does, by label text, so they're treated as genuinely different types
// for the mirroring comparison, not silently merged.
function portGroupKey (p: { type: string; label: string }): string {
  if (p.type === 'dmx' && p.label.toLowerCase().startsWith ('artdmx')) return 'artnet';
  return p.type;
}

const NATIVE_THEME_BY_KEY: Record<string, { accent: string; dim: string; glow: string }> = {
  midi:   { accent: 'var(--midi)',    dim: 'var(--midi-dim)',    glow: 'var(--midi-glow)' },
  audio:  { accent: 'var(--audio)',   dim: 'var(--audio-dim)',   glow: 'var(--audio-glow)' },
  osc:    { accent: 'var(--osc)',     dim: 'var(--osc-dim)',     glow: 'var(--osc-glow)' },
  dmx:    { accent: 'var(--dmx)',     dim: 'var(--dmx-dim)',     glow: 'var(--dmx-glow)' },
  artnet: { accent: 'var(--artnet)',  dim: 'var(--artnet-dim)',  glow: 'var(--artnet-glow)' },
  mqtt:   { accent: 'var(--mqtt)',    dim: 'var(--mqtt-dim)',    glow: 'var(--mqtt-glow)' },
  udp:    { accent: 'var(--udp)',     dim: 'var(--udp-dim)',     glow: 'var(--udp-glow)' },
  // Plain Generic Value gets its OWN native colour (--generic), distinct
  // from --value, which is Converter's colour specifically — see
  // index.css's --generic comment for why these can't share one colour.
  value:  { accent: 'var(--generic)', dim: 'var(--generic-dim)', glow: 'var(--generic-glow)' },
};

const HYBRID_THEME    = { accent: 'var(--av)',    dim: 'var(--av-dim)',    glow: 'var(--av-glow)' };
const CONVERTER_THEME = { accent: 'var(--value)', dim: 'var(--value-dim)', glow: 'var(--value-glow)' };

// colourCategory override values — mirrors PAX_COLOURCAT_* in PaxAPI.h.
// Only ever consulted for the pure-source/pure-sink case above; never a
// general override, so it can't make an already-clear node's colour
// misrepresent what it actually does.
const COLOURCAT_THEME: Record<number, { accent: string; dim: string; glow: string }> = {
  0: NATIVE_THEME_BY_KEY.midi,
  1: NATIVE_THEME_BY_KEY.audio,
  2: HYBRID_THEME,
  3: CONVERTER_THEME,
  4: NATIVE_THEME_BY_KEY.osc,
  5: NATIVE_THEME_BY_KEY.dmx,
  6: NATIVE_THEME_BY_KEY.mqtt,
  7: NATIVE_THEME_BY_KEY.udp,
  8: NATIVE_THEME_BY_KEY.artnet,
};

// colourCategory override values, as category KEYS (not theme objects) —
// used by detectPaxCategoryKey below. 'generic' is deliberately its own
// key, distinct from 'value' (portGroupKey's raw type string) — a node's
// PORT can be type 'value' (generic), but the node's overall CATEGORY,
// once detected as a single mirrored generic type, is called 'generic'
// throughout the UI (sidebar section name, tag word) to avoid confusion
// with 'value' meaning something more specific elsewhere.
const COLOURCAT_KEY: Record<number, string> = {
  0: 'midi', 1: 'audio', 2: 'hybrid', 3: 'converter',
  4: 'osc', 5: 'dmx', 6: 'mqtt', 7: 'udp', 8: 'artnet',
};

/** The single source of truth for Hybrid-vs-Converter-vs-native-type
 * detection — returns one of: midi, audio, hybrid, converter, osc, dmx,
 * artnet, mqtt, udp, generic. detectPaxTheme, detectPaxTagPrefix, and
 * Sidebar.tsx's section grouping all derive from this one function
 * rather than each re-implementing the same input/output-set comparison —
 * refactored into this shape while fixing the sidebar's section grouping
 * to match a node's actual detected category instead of its raw
 * (pre-detection) ngaType. */
export function detectPaxCategoryKey (
  ports: { type: string; label: string; direction: string }[],
  colourCategory: number | undefined
): string {
  const inputKeys  = new Set (ports.filter (p => p.direction === 'input').map (portGroupKey));
  const outputKeys = new Set (ports.filter (p => p.direction === 'output').map (portGroupKey));
  const allKeys     = new Set ([...inputKeys, ...outputKeys]);
  const isPureSourceOrSink = inputKeys.size === 0 || outputKeys.size === 0;

  const asCategoryKey = (k: string) => (k === 'value' ? 'generic' : k);

  if (isPureSourceOrSink) {
    if (colourCategory !== undefined && colourCategory >= 0 && COLOURCAT_KEY[colourCategory])
      return COLOURCAT_KEY[colourCategory];
    if (allKeys.size === 1) return asCategoryKey ([...allKeys][0]);
    return 'hybrid';
  }

  const allMirror = [...allKeys].every (k => inputKeys.has (k) && outputKeys.has (k));
  if (! allMirror) return 'converter';
  if (allKeys.size === 1) return asCategoryKey ([...allKeys][0]);
  return 'hybrid';
}

const THEME_BY_CATEGORY_KEY: Record<string, { accent: string; dim: string; glow: string }> = {
  midi: NATIVE_THEME_BY_KEY.midi, audio: NATIVE_THEME_BY_KEY.audio,
  osc: NATIVE_THEME_BY_KEY.osc, dmx: NATIVE_THEME_BY_KEY.dmx,
  artnet: NATIVE_THEME_BY_KEY.artnet, mqtt: NATIVE_THEME_BY_KEY.mqtt,
  udp: NATIVE_THEME_BY_KEY.udp, generic: NATIVE_THEME_BY_KEY.value,
  hybrid: HYBRID_THEME, converter: CONVERTER_THEME,
};

export function detectPaxTheme (
  ports: { type: string; label: string; direction: string }[],
  colourCategory: number | undefined
): { accent: string; dim: string; glow: string } {
  return THEME_BY_CATEGORY_KEY[detectPaxCategoryKey (ports, colourCategory)] ?? HYBRID_THEME;
}

// Converter has no prefix — the fuchsia colour already conveys "this
// converts"; unlike the type-specific prefixes below, a "CONVERTER" word
// adds width without adding information the colour doesn't already carry.
const TAG_PREFIX_BY_CATEGORY_KEY: Record<string, string> = {
  midi: 'MIDI', audio: 'AUDIO', hybrid: 'HYBRID', converter: '',
  osc: 'OSC', dmx: 'DMX', artnet: 'ARTNET', mqtt: 'MQTT', udp: 'UDP',
  generic: 'VALUE',
};

export function detectPaxTagPrefix (
  ports: { type: string; label: string; direction: string }[],
  colourCategory: number | undefined
): string {
  const key = detectPaxCategoryKey (ports, colourCategory);
  return TAG_PREFIX_BY_CATEGORY_KEY[key] ?? 'PLUGIN';
}


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
                    cursor: 'pointer', userSelect: 'none' }}
           onPointerDown={e => e.stopPropagation()}
           onClick={e => e.stopPropagation()}>
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
/** Persists settings panel open state across graph updates (survives undo/redo) */
const _settingsOpen = new Map<string, boolean>();

/**
 * Manages settings panel open/close state, including elevating the node's
 * z-index via ReactFlow's updateNode so the settings panel always appears
 * above other nodes.
 */
export function useNodeSettings (id: string) {
  const { updateNode } = useReactFlow();
  const [showSettings, setShowSettings] = useState(() => _settingsOpen.get(id) ?? false);

  const openSettings = useCallback(() => {
    _settingsOpen.set(id, true);
    setShowSettings(true);
    updateNode(id, { style: { zIndex: 9999 } });
  }, [id, updateNode]);

  const closeSettings = useCallback(() => {
    _settingsOpen.set(id, false);
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

// ── isLikelyCompleteHost ────────────────────────────────────────────────────
// Checks whether a host string looks syntactically complete enough to be
// worth attempting a connection to — either a valid IPv4 address, or a
// hostname-shaped string with no leading/trailing/double dots. Deliberately
// permissive on hostnames (can't fully validate without a real DNS lookup),
// but this catches the overwhelmingly common case: partial states typed
// character-by-character while entering an IP (e.g. "127.", "192.168.")
// which would otherwise reach a blocking DNS resolution and freeze the whole
// graph (confirmed upstream libmosquitto behaviour for MQTT — see
// Architecture.md's MQTT locked decisions). Used by every {Protocol}DeviceUI
// settings panel with a host-shaped field (UDP, OSC, MQTT, ArtNet).
export function isLikelyCompleteHost(host: string): boolean {
  if (!host) return false;
  if (host.startsWith('.') || host.endsWith('.') || host.includes('..')) return false;

  const ipv4Match = host.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if (ipv4Match) {
    return ipv4Match.slice(1).every(octet => {
      const n = parseInt(octet, 10);
      return n >= 0 && n <= 255;
    });
  }

  // Not a complete IPv4 shape — if it contains only digits and dots, it's
  // a partial IP being typed (e.g. "127", "192.168"), not a hostname yet.
  if (/^[\d.]+$/.test(host)) return false;

  // Otherwise treat as a hostname-shaped string (e.g. "mybroker.local",
  // "localhost") — permissive, since hostnames vary widely in valid form.
  return true;
}

// ── portColour ───────────────────────────────────────────────────────────────
// Maps a classified port type string (see App.tsx's getPortType/
// colourForHandleId) to its display colour. Used as a fallback wherever a
// port's type is known generically rather than via a specific protocol's
// own accent colour.
export function portColour (type: string): string {
  switch (type) {
    case 'audio': return 'rgb(20,80,20)';
    case 'osc':   return 'var(--osc)';
    case 'dmx':   return 'var(--dmx)';
    case 'mqtt':  return 'var(--mqtt)';
    case 'udp':   return 'var(--udp)';
    case 'value': return 'var(--value)';
    default:      return 'var(--midi)';
  }
}
