import { memo, useCallback, useEffect, useRef, useState, useContext } from 'react';
import { HintContext, NODE_HINTS, BUTTON_HINTS } from './HintPanel';
import { DawContext } from './DawContext';
import { X } from 'lucide-react';
import { NodeProps } from '@xyflow/react';
import { Bridge, AddonParamInfo } from './Bridge';
import { useNodeDelete, NodeHeaderButton, useNodeCollapsed, NodeHandle } from './NodeUtils';

import { NodeSelect } from './NodeSelect';

export interface NodeData {
  label: string;
  nodeType: 1 | 2 | 3 | 4 | number;  // 1-4 built-in, higher = addon
  selectedDeviceId?: string;
  ports: {
    id: string;
    label: string;
    type: 'midi' | 'audio';
    direction: 'input' | 'output';
  }[];
  addonParams?: AddonParamInfo[];
  settingsJson?: string;
  [key: string]: unknown;
}

// ── Colour theme per node type ────────────────────────────────────────────────
const THEME: Record<number, { accent: string; dim: string; glow: string; tag: string }> = {
  1: { accent: 'var(--midi)',  dim: 'var(--midi-dim)',  glow: 'var(--midi-glow)',  tag: 'MIDI IN DEVICE'   },
  2: { accent: 'var(--midi)',  dim: 'var(--midi-dim)',  glow: 'var(--midi-glow)',  tag: 'MIDI OUT DEVICE'  },
  3: { accent: 'var(--audio)', dim: 'var(--audio-dim)', glow: 'var(--audio-glow)', tag: 'AUDIO IN DEVICE'  },
  4: { accent: 'var(--audio)', dim: 'var(--audio-dim)', glow: 'var(--audio-glow)', tag: 'AUDIO OUT DEVICE' },
};
// Default theme for addon nodes
const ADDON_THEME = { accent: 'var(--av)', dim: 'var(--av-dim)', glow: 'var(--av-glow)', tag: 'PLUGIN' };


// ── Device selector combobox (unified for MIDI and Audio) ────────────────────
function DeviceSelector ({ nodeId, nodeType, selectedDeviceId }: {
  nodeId:           string;
  nodeType:         1 | 2 | 3 | 4;
  selectedDeviceId?: string;
}) {
  const isMidi = nodeType <= 2;
  const accent = isMidi ? 'var(--midi)' : 'var(--audio)';
  const { isStandalone, dawLoopbackEnabled } = useContext(DawContext);
  const { setHint } = useContext(HintContext);
  const [devices,       setDevices]       = useState<Array<{ id: string; name: string }>>([]);
  const [selectedValue, setSelectedValue] = useState<string>(selectedDeviceId ?? '');
  const [claimed,       setClaimed]       = useState<Map<string, { deviceId: string; nodeType: number }>>(new Map());

  // Sync prop → local state on graph restore
  useEffect(() => { setSelectedValue(selectedDeviceId ?? ''); }, [selectedDeviceId]);

  useEffect(() => {
    const unsubClaimed = Bridge.onClaimedDevices(map => setClaimed(new Map(map)));
    const unsubDevices = isMidi
      ? Bridge.onMidiDevices(list => setDevices(nodeType === 2 ? list.midiOutDevices : list.midiInDevices))
      : Bridge.onAudioDevices(list => setDevices(nodeType === 4 ? list.audioOutDevices : list.audioInDevices));
    return () => { unsubDevices(); unsubClaimed(); };
  }, [nodeType, isMidi]);

  // Grey out devices claimed by nodes of the same direction
  const takenByOthers = new Set<string>();
  claimed.forEach((claim, nId) => {
    if (nId !== nodeId && claim.nodeType === nodeType && claim.deviceId)
      takenByOthers.add(claim.deviceId);
  });

  const isDawLocked = (d: { id: string }) =>
    d.id === 'DAW' && nodeType === 4 && !isStandalone && !dawLoopbackEnabled;
  const dawHint = { title: 'DAW Loopback Locked 🔒', body: 'Routing audio back to the DAW track risks a feedback loop.\nEnable "DAW loopback" in Preferences → Graph to unlock.' };
  const opts = devices.map(d => ({
    id:       d.id,
    name:     isDawLocked(d) ? 'DAW  🔒' : d.name,
    disabled: takenByOthers.has(d.id) || isDawLocked(d),
    warning:  isDawLocked(d),
    hint:     isDawLocked(d) ? dawHint : undefined,
  }));
  const paramKey = isMidi ? 'midiDeviceId' : 'audioDeviceId';

  return (
    <NodeSelect
      value={selectedValue}
      onChange={v => { setSelectedValue(v); Bridge.setNodeParam(nodeId, paramKey, v, nodeType); }}
      options={opts}
      disabled={devices.length === 0}
      accent={accent}
      onOptionHover={h => setHint(h)}
    />
  );
}

// ── Main node ─────────────────────────────────────────────────────────────────
function GenericNode({ id, data, selected }: NodeProps) {
  const nodeData = data as NodeData;
  // nodeType>=100 means addon: extract ngaType = nodeType-100 for theming
  const ngaType  = nodeData.nodeType >= 100 ? nodeData.nodeType - 100 : null;
  const addonTag = (() => {
    if (ngaType === null) return null;
    const prefix = ({ 1: 'MIDI', 2: 'AUDIO', 3: 'AV' } as Record<number,string>)[ngaType] ?? 'PLUGIN';
    const labelUp = nodeData.label.toUpperCase();
    // Avoid doubling the prefix (e.g. "AV Passthrough" → "AV PASSTHROUGH" not "AV AV PASSTHROUGH")
    const body = labelUp.startsWith(prefix + ' ') ? labelUp.slice(prefix.length + 1) : labelUp;
    return prefix + ' ' + body;
  })();
  const theme    = ngaType !== null
    ? ({ 1: { accent: 'var(--midi)',  dim: 'var(--midi-dim)',  glow: 'var(--midi-glow)',  tag: addonTag! },
          2: { accent: 'var(--audio)', dim: 'var(--audio-dim)', glow: 'var(--audio-glow)', tag: addonTag! },
          3: { accent: 'var(--av)',    dim: 'var(--av-dim)',    glow: 'var(--av-glow)',    tag: addonTag! },
        }[ngaType] ?? { ...ADDON_THEME, tag: addonTag! })
    : (THEME[nodeData.nodeType] ?? ADDON_THEME);

  const inputs  = nodeData.ports.filter(p => p.direction === 'input');
  const outputs = nodeData.ports.filter(p => p.direction === 'output');

  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const isAddon = ngaType !== null;
  const { setHint } = useContext(HintContext);
  const portBodyRef = useRef<HTMLDivElement>(null);
  const addonParams = (data.addonParams ?? []) as AddonParamInfo[];
  const [paramValues, setParamValues] = useState<number[]>([]);

  // Sync paramValues when addonParams arrive (may come after mount)
  // Also restore saved values from settingsJson if available
  useEffect(() => {
    if (addonParams.length > 0 && paramValues.length === 0) {
      const saved = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson) as number[] : null;
      const vals = saved && saved.length === addonParams.length
        ? saved
        : addonParams.map(p => p.defaultValue);
      setParamValues(vals);
      // Restore param values to C++ addon
      vals.forEach((v, i) => Bridge.setAddonParameter(id, i, v));
    }
  }, [addonParams.length]);

  const onParamChange = useCallback((index: number, value: number) => {
    setParamValues(prev => {
      const next = [...prev]; next[index] = value;
      Bridge.setNodeSettings(id, next);
      return next;
    });
    Bridge.setAddonParameter(id, index, value);
  }, [id]);
  const [customName, setCustomName] = useState('');
  const onNameChange = useCallback((name: string) => {
    setCustomName(name);
    Bridge.setNodeLabel(id, name);
  }, [id]);

  return (
    <div
      style={{
        minWidth:     Math.max(theme.tag.length * 10 + 80, 220),
        background:   'var(--surface)',
        border:       `1px solid ${selected ? theme.accent : 'var(--border)'}`,
        borderTop:    `3px solid ${theme.accent}`,
        borderRadius: 'var(--radius)',
        boxShadow:    selected
          ? `0 0 0 1px ${theme.accent}, 0 8px 32px ${theme.glow}`
          : `0 4px 16px rgba(0,0,0,.5)`,
        transition:   'box-shadow .15s, border-color .15s',
        position:     'relative',
        userSelect:   'none',
      }}
    >
      {/* Header */}
      <div
        onDoubleClick={toggleCollapsed}
        onMouseEnter={e => {
          if (!e.currentTarget.contains(e.relatedTarget as Node)) {
            const addonName = nodeData.addonName as string | undefined;
            const h = (addonName ? NODE_HINTS[addonName] : null) ?? NODE_HINTS[String(theme.tag ?? '')] ?? null;
            if (h) setHint(h);
          }
        }}
        onMouseLeave={e => {
          if (!e.currentTarget.contains(e.relatedTarget as Node)) setHint(null);
        }}
        style={{
          padding:        '8px 10px 6px',
          borderBottom:   collapsed ? 'none' : '1px solid var(--border)',
          display:        'flex',
          alignItems:     'center',
          justifyContent: 'space-between',
          gap:            6,
          cursor:         'pointer',
          userSelect:     'none',
        }}>
        <div style={{ minWidth: 0, flex: 1, display: 'flex', alignItems: 'center', gap: 4 }}>
          <span style={{
            fontSize:   12,
            color:      theme.accent,
            opacity:    0.7,
            display:    'inline-block',
            transform:  collapsed ? 'rotate(0deg)' : 'rotate(90deg)',
            transition: 'transform 0.2s',
          }}>
              <svg width="8" height="10" viewBox="0 0 8 10" style={{ display:'block' }}>
                <polygon points="0,0 8,5 0,10" fill="currentColor" />
              </svg>
            </span>
          <div style={{
            fontSize:      '11px',
            fontWeight:    700,
            color:         theme.accent,
            letterSpacing: '0.1em',
            fontFamily:    "'Syne', sans-serif",
            whiteSpace:    'nowrap',
          }}>
            {customName || theme.tag}
          </div>
        </div>

        {/* Addon name input */}
        {isAddon && (
          <input type="text" value={customName}
            placeholder={theme.tag}
            onChange={e => onNameChange(e.target.value)}
            className="nodrag"
            onMouseDown={e => e.stopPropagation()}
            onPointerDown={e => e.stopPropagation()}
            style={{ background:'transparent', border:'none', borderBottom:'1px solid var(--border)',
                     color:'var(--text-muted)', fontSize:10, borderRadius:0, outline:'none',
                     padding:'1px 4px', fontFamily:"'JetBrains Mono', monospace",
                     width:100, minWidth:0 }} />
        )}

        {/* Delete button */}
        <NodeHeaderButton onClick={handleDelete} danger
          onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.deleteNode), onMouseLeave: () => setHint(null) }}><X size={14} /></NodeHeaderButton>
      </div>

      {!collapsed && <>
      {/* Device selector — only renders for device nodes, provides portBodyRef anchor */}
      <div ref={portBodyRef} style={{ padding: !isAddon ? '8px 10px' : '0',
                                         minHeight: isAddon && addonParams.length === 0 ? 32 : undefined }}>
        {(nodeData.nodeType === 1 || nodeData.nodeType === 2) && (
          <DeviceSelector
            nodeId={id}
            nodeType={nodeData.nodeType as 1 | 2}
            selectedDeviceId={nodeData.selectedDeviceId}
          />
        )}
        {(nodeData.nodeType === 3 || nodeData.nodeType === 4) && (
          <DeviceSelector
            nodeId={id}
            nodeType={nodeData.nodeType as 3 | 4}
            selectedDeviceId={nodeData.selectedDeviceId}
          />
        )}
      </div>

      {/* Addon parameter sliders — outside port body so padding works correctly */}
      {isAddon && addonParams.length > 0 && (
        <div className="nodrag" style={{ padding: '8px 10px 6px',
                                         borderTop: '1px solid var(--border)' }}>
          <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 2 }}>
            <NodeHeaderButton
              onClick={() => {
                const defaults = addonParams.map(p => p.defaultValue);
                setParamValues(defaults);
                Bridge.setNodeSettings(id, defaults);
                defaults.forEach((v, i) => Bridge.setAddonParameter(id, i, v));
              }}
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.reset), onMouseLeave: () => setHint(null) }}><span style={{ fontSize: 11, fontWeight: 700 }}>R</span></NodeHeaderButton>
          </div>
          {addonParams.map((p, i) => (
            <div key={i} style={{ marginBottom: 0, paddingTop: 8 }}>
              {/* Binary toggle for 0/1 integer params */}
              {p.step >= 1 && p.min === 0 && p.max === 1 ? (
                <div style={{ display: 'flex', justifyContent: 'space-between',
                              alignItems: 'center' }}>
                  <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{p.name}</span>
                  <div style={{ display: 'flex', gap: 4 }}>
                    {['Off', 'On'].map((label, val) => (
                      <div key={val}
                        onClick={() => onParamChange(i, val)}
                        style={{
                          fontSize:     9,
                          padding:      '2px 6px',
                          borderRadius: 3,
                          cursor:       'pointer',
                          border:       `1px solid ${(paramValues[i] ?? p.defaultValue) === val ? 'var(--av)' : 'var(--border)'}`,
                          color:        (paramValues[i] ?? p.defaultValue) === val ? 'var(--av)' : 'var(--text-muted)',
                          background:   (paramValues[i] ?? p.defaultValue) === val ? 'rgba(251,146,60,0.15)' : 'transparent',
                        }}>
                        {label}
                      </div>
                    ))}
                  </div>
                </div>
              ) : (
              <div style={{ position: 'relative' }}>
                <input type="range"
                  min={p.min} max={p.max}
                  step={(p.max - p.min) / 1000}
                  value={paramValues[i] !== undefined ? paramValues[i] : p.defaultValue}
                  onChange={e => {
                    let v = parseFloat(e.target.value);
                    if (p.step >= 1) v = Math.round(v);
                    onParamChange(i, v);
                  }}
                  onDoubleClick={() => onParamChange(i, p.defaultValue)}
                  onMouseEnter={() => setHint({ title: p.name, body: `Range: ${p.min} to ${p.max}${p.step >= 1 ? ' (integer)' : ''}.
Double-click to reset to default (${p.defaultValue}).` })}
                  onMouseLeave={() => setHint(null)}
                 
                  style={{ width: '100%', display: 'block' }}
                />
                {p.defaultValue > p.min && p.defaultValue < p.max && (() => {
                  const thumbW = 10;
                  const pct    = (p.defaultValue - p.min) / (p.max - p.min);
                  const left   = `calc(${pct * 100}% - ${pct * thumbW - thumbW / 2}px)`;
                  return (
                    <div style={{
                      position:      'absolute',
                      bottom:        -4,
                      left,
                      transform:     'translateX(-50%)',
                      width:         2,
                      height:        5,
                      background:    theme.accent,
                      borderRadius:  1,
                      pointerEvents: 'none',
                      opacity:       0.7,
                    }} />
                  );
                })()}
              </div>
              )}
              {/* label + value - only for non-binary params */}
              {!(p.step >= 1 && p.min === 0 && p.max === 1) && (
              <div style={{ display: 'flex', justifyContent: 'space-between',
                            fontSize: 9, marginTop: 9 }}>
                <span style={{ color: 'var(--text-muted)' }}>{p.name}</span>
                <span style={{ color: 'var(--text-dim)' }}>
                  {(() => {
                    const v = paramValues[i] !== undefined ? paramValues[i] : p.defaultValue;
                    const isDb = p.name.toLowerCase().includes('db');
                    if (p.step >= 1) return (v > 0 ? '+' : '') + Math.round(v).toString() + (isDb ? ' dB' : '');
                    if (isDb) return (v >= 0 ? '+' : '') + v.toFixed(1) + ' dB';
                    return v.toFixed(2);
                  })()}
                </span>
              </div>
              )}
            </div>
          ))}
        </div>
      )}
      </>}

      {/* Input handles */}
      {inputs.map((p, i) => (
        <NodeHandle key={p.id}
          nodeId={id} label={p.label} direction="in"
          colour={p.type === 'midi' ? 'var(--midi)' : 'rgb(20,80,20)'}
          index={i} total={inputs.length}
          offset={isAddon ? 6 : 8}
          portBodyRef={portBodyRef}
          portId={p.id}
        />
      ))}

      {/* Output handles */}
      {outputs.map((p, i) => (
        <NodeHandle key={p.id}
          nodeId={id} label={p.label} direction="out"
          colour={p.type === 'midi' ? 'var(--midi)' : 'rgb(20,80,20)'}
          index={i} total={outputs.length}
          offset={isAddon ? 6 : 8}
          portBodyRef={portBodyRef}
          portId={p.id}
        />
      ))}
    </div>
  );
}

export default memo(GenericNode);
