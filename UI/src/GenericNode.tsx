import { memo, useCallback, useEffect, useRef, useState, useContext } from 'react';
import { HintContext, NODE_HINTS, BUTTON_HINTS } from './HintPanel';
import { DawContext } from './DawContext';
import { X } from 'lucide-react';
import { NodeProps } from '@xyflow/react';
import { Bridge, PaxParamInfo } from './Bridge';
import { useNodeDelete, NodeHeaderButton, useNodeCollapsed, useNodeSettings, NodeHandle, nodeContainerStyle, SettingsPanelHeader, Checkbox } from './NodeUtils';

import { NodeSelect } from './NodeSelect';

export interface NodeData {
  label: string;
  nodeType: 1 | 2 | 3 | 4 | number;  // 1-4 built-in, higher = Pax
  selectedDeviceId?: string;
  ports: {
    id: string;
    label: string;
    type: 'midi' | 'audio' | 'osc' | 'dmx' | 'mqtt' | 'udp' | 'value';
    direction: 'input' | 'output';
  }[];
  paxParams?: PaxParamInfo[];
  settingsJson?: string;
  [key: string]: unknown;
}

// ── Colour theme per node type ────────────────────────────────────────────────
const THEME: Record<number, { accent: string; dim: string; glow: string; tag: string }> = {
  1:  { accent: 'var(--midi)',  dim: 'var(--midi-dim)',  glow: 'var(--midi-glow)',  tag: 'MIDI IN DEVICE'   },
  2:  { accent: 'var(--midi)',  dim: 'var(--midi-dim)',  glow: 'var(--midi-glow)',  tag: 'MIDI OUT DEVICE'  },
  3:  { accent: 'var(--audio)', dim: 'var(--audio-dim)', glow: 'var(--audio-glow)', tag: 'AUDIO IN DEVICE'  },
  4:  { accent: 'var(--audio)', dim: 'var(--audio-dim)', glow: 'var(--audio-glow)', tag: 'AUDIO OUT DEVICE' },
  8:  { accent: 'var(--udp)',   dim: 'var(--udp-dim)',   glow: 'var(--udp-glow)',   tag: 'UDP IN DEVICE'    },
  9:  { accent: 'var(--udp)',   dim: 'var(--udp-dim)',   glow: 'var(--udp-glow)',   tag: 'UDP OUT DEVICE'   },
  10: { accent: 'var(--osc)',   dim: 'var(--osc-dim)',   glow: 'var(--osc-glow)',   tag: 'OSC IN DEVICE'    },
  11: { accent: 'var(--osc)',   dim: 'var(--osc-dim)',   glow: 'var(--osc-glow)',   tag: 'OSC OUT DEVICE'   },
};
// Default theme for Pax nodes
const PAX_THEME = { accent: 'var(--av)', dim: 'var(--av-dim)', glow: 'var(--av-glow)', tag: 'PAX' };


// ── Device selector combobox (unified for MIDI and Audio) ────────────────────
function DeviceSelector ({ nodeId, nodeType, selectedDeviceId }: {
  nodeId:           string;
  nodeType:         1 | 2 | 3 | 4;
  selectedDeviceId?: string;
}) {
  const isMidi = nodeType <= 2;
  const accent = isMidi ? 'var(--midi)' : 'var(--audio)';
  const { isStandalone, dawLoopbackEnabled, dawHostEnabled } = useContext(DawContext);
  const { setHint } = useContext(HintContext);
  const [devices, setDevices] = useState<Array<{ id: string; name: string }>>([]);
  const [claimed, setClaimed] = useState<Map<string, { deviceId: string; nodeType: number }>>(new Map());

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

  const isDawLocked = (d: { id: string; dawHost?: boolean }) =>
    d.id === 'DAW' && nodeType === 4 && !isStandalone && !dawLoopbackEnabled;
  const dawHint = { title: 'DAW Loopback Locked 🔒', body: 'Routing audio back to the DAW track risks a feedback loop.\nEnable "DAW loopback" in Preferences → Graph to unlock.' };
  const isDawHostLocked = (d: { id: string; dawHost?: boolean }) => !!d.dawHost && !isStandalone && !dawHostEnabled;
  const dawHostHint = { title: 'DAW Host Device ⚠', body: 'This is your DAW\'s own virtual audio device.\nUsing it may cause signal doubling or unexpected behaviour.\nUse the "DAW" option instead for proper routing.' };
  const opts = devices.map(d => ({
    id:       d.id,
    name:     isDawLocked(d) ? 'DAW  🔒' : isDawHostLocked(d) ? `${d.name}  🔒` : d.name,
    disabled: takenByOthers.has(d.id) || isDawLocked(d) || isDawHostLocked(d),
    warning:  isDawLocked(d) || isDawHostLocked(d),
    hint:     isDawLocked(d) ? dawHint : isDawHostLocked(d) ? dawHostHint : undefined,
  }));
  const paramKey = isMidi ? 'midiDeviceId' : 'audioDeviceId';

  return (
    <NodeSelect
      value={selectedDeviceId ?? ''}
      onChange={v => Bridge.setNodeParam(nodeId, paramKey, v, nodeType)}
      options={opts}
      disabled={devices.length === 0}
      accent={accent}
      onOptionHover={h => setHint(h)}
    />
  );
}

// ── Audio device channel settings panel ──────────────────────────────────────
function AudioDeviceSettingsPanel ({ nodeId, nodeType, selectedChannels, deviceChannelCount, selectedDeviceId, warning, onClose }: {
  nodeId:             string;
  nodeType:           3 | 4;
  selectedChannels:   number[];
  deviceChannelCount: number;
  selectedDeviceId:   string | undefined;
  warning:            boolean;
  onClose:            () => void;
}) {
  const accent = 'var(--audio)';

  const toggle = (ch: number) => {
    const next = selectedChannels.includes(ch)
      ? selectedChannels.filter(c => c !== ch)
      : [...selectedChannels, ch].sort((a, b) => a - b);
    // Always keep at least one channel selected
    if (next.length === 0) return;
    Bridge.setNodeParam(nodeId, 'audioDeviceChannels', JSON.stringify(next), nodeType);
  };

  const isOut = nodeType === 4;
  const title = isOut ? 'Audio OUT Channels' : 'Audio IN Channels';

  return (
    <div
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onPointerUp={e => e.stopPropagation()}
      onClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6,
        width: deviceChannelCount > 32 ? 320 : deviceChannelCount > 16 ? 260 : 200, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title={title}
        onReset={() => Bridge.setNodeParam(nodeId, 'audioDeviceChannels', JSON.stringify([0, 1]), nodeType)}
        onClose={onClose}
      />
      {warning && (
        <div style={{
          fontSize: 9, color: '#ef5350', background: 'rgba(239,83,80,0.1)',
          border: '1px solid rgba(239,83,80,0.3)', borderRadius: 3,
          padding: '4px 6px', marginBottom: 6,
        }}>
          Channel selection reset — previous channels not available on this device.
        </div>
      )}
      {deviceChannelCount <= 0 || !selectedDeviceId ? (
        <div style={{ fontSize: 10, color: 'var(--text-muted)', marginTop: 8 }}>
          No device selected
        </div>
      ) : (
        <>
          <div style={{
            fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
            textTransform: 'uppercase', marginTop: 8, marginBottom: 6,
          }}>
            Select channels
          </div>
          <div style={{
            display: 'grid',
            gridTemplateColumns: deviceChannelCount > 32 ? '1fr 1fr 1fr 1fr'
                                : deviceChannelCount > 16 ? '1fr 1fr 1fr'
                                : '1fr 1fr',
            gap: 4,
          }}>
            {Array.from({ length: deviceChannelCount }, (_, i) => (
              <label key={i} style={{
                display: 'flex', alignItems: 'center', gap: 5,
                fontSize: 10, color: 'var(--text-dim)', cursor: 'pointer',
              }}>
                <Checkbox
                  checked={selectedChannels.includes(i)}
                  onChange={() => toggle(i)}
                  accent={accent}
                />
                Ch {i + 1}
              </label>
            ))}
          </div>
          {selectedChannels.length === 0 && (
            <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
              At least one channel required
            </div>
          )}
        </>
      )}
    </div>
  );
}

// ── Channel summary label ─────────────────────────────────────────────────────
function ChannelSummary ({ channels }: { channels: number[] }) {
  if (channels.length === 0) return null;
  const label = channels.map(c => `Ch ${c + 1}`).join(', ');
  return (
    <div style={{
      fontSize: 9, color: 'var(--text-muted)', textAlign: 'center',
      marginBottom: 3, letterSpacing: '0.05em',
    }}>
      {label}
    </div>
  );
}

// ── UDP port summary label ────────────────────────────────────────────────────
function UdpPortSummary ({ port, mode, multicastAddr, byteRate, onClick }: { port: number; mode: 0 | 1 | 2; multicastAddr: string; byteRate: string; onClick: () => void }) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  if (!port) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        No port set
      </div>
    );
  }
  const modeTag = mode === 1 ? ` · multicast${multicastAddr ? ` · ${multicastAddr}` : ''}` : mode === 2 ? ' · broadcast' : '';
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>:{port}{modeTag}</span>
      {byteRate && <span style={{ color: 'var(--udp)', opacity: 0.85 }}>{byteRate}</span>}
    </div>
  );
}

// ── UDP IN/OUT settings panel ────────────────────────────────────────────────
function UdpDeviceSettingsPanel ({ nodeId, nodeType, port, mode, targetHost, multicastAddr, onClose }: {
  nodeId:         string;
  nodeType:       8 | 9;
  port:           number;
  mode:           0 | 1 | 2;
  targetHost:     string;
  multicastAddr:  string;
  onClose:        () => void;
}) {
  const isOut  = nodeType === 9;
  const title  = isOut ? 'UDP OUT Settings' : 'UDP IN Settings';
  const accent = 'var(--udp)';

  const commit = (next: { port?: number; mode?: 0 | 1 | 2; targetHost?: string; multicastAddr?: string }) => {
    Bridge.setUdpSettings(
      nodeId,
      next.port ?? port,
      next.mode ?? mode,
      next.targetHost ?? targetHost,
      next.multicastAddr ?? multicastAddr,
    );
  };

  const inputStyle: React.CSSProperties = {
    width: '100%', fontSize: 10, padding: '3px 6px', marginTop: 2,
    background: 'var(--surface)', border: '1px solid var(--border)',
    borderRadius: 3, color: 'var(--text-dim)',
    fontFamily: "'JetBrains Mono', monospace",
  };

  return (
    <div
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onPointerUp={e => e.stopPropagation()}
      onClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6,
        width: 200, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader title={title} onReset={() => commit({ port: 0, mode: 0, targetHost: '', multicastAddr: '' })} onClose={onClose} />

      <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
        Port
      </div>
      <input
        type="number" min={1} max={65535} value={port || ''}
        placeholder="e.g. 9000"
        onChange={e => commit({ port: parseInt(e.target.value, 10) || 0 })}
        style={inputStyle}
      />

      <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
        Mode
      </div>
      <div style={{ display: 'flex', gap: 4 }}>
        {(['Unicast', 'Multicast', 'Broadcast'] as const).map((label, i) => (
          <button key={label}
            onClick={() => commit({ mode: i as 0 | 1 | 2 })}
            style={{
              flex: 1, fontSize: 9, padding: '4px 2px', borderRadius: 3,
              border: '1px solid ' + (mode === i ? accent : 'var(--border)'),
              background: mode === i ? 'var(--surface)' : 'transparent',
              color: mode === i ? accent : 'var(--text-muted)',
              cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            }}>
            {label}
          </button>
        ))}
      </div>

      {isOut && mode === 0 && (
        <>
          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
            Target Host
          </div>
          <input
            type="text" value={targetHost} placeholder="192.168.1.50"
            onChange={e => commit({ targetHost: e.target.value })}
            style={inputStyle}
          />
        </>
      )}

      {mode === 1 && (
        <>
          <div style={{ fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em', textTransform: 'uppercase', marginTop: 8, marginBottom: 4 }}>
            Multicast Group
          </div>
          <input
            type="text" value={multicastAddr} placeholder="239.0.0.1"
            onChange={e => commit({ multicastAddr: e.target.value })}
            style={inputStyle}
          />
        </>
      )}

      {!port && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set a port to activate
        </div>
      )}
    </div>
  );
}

// ── OSC IN/OUT settings panel ────────────────────────────────────────────────
function OscDeviceSettingsPanel ({ nodeId, nodeType, port, targetHost, oscAddress, onClose }: {
  nodeId:      string;
  nodeType:    10 | 11;
  port:        number;
  targetHost:  string;
  oscAddress:  string;
  onClose:     () => void;
}) {
  const isOut = nodeType === 11;
  const title = isOut ? 'OSC OUT Settings' : 'OSC IN Settings';
  const accent = 'var(--osc)';

  const commit = (next: { port?: number; targetHost?: string; oscAddress?: string }) => {
    Bridge.setOscSettings(
      nodeId,
      next.port        ?? port,
      next.targetHost  ?? targetHost,
      next.oscAddress  ?? oscAddress,
    );
  };

  const inputStyle: React.CSSProperties = {
    width: '100%', fontSize: 10, padding: '3px 6px', marginTop: 2,
    background: 'var(--surface)', border: '1px solid var(--border)',
    borderRadius: 3, color: 'var(--text-dim)',
    fontFamily: "'JetBrains Mono', monospace",
  };

  const labelStyle: React.CSSProperties = {
    fontSize: 9, color: 'var(--text-muted)', letterSpacing: '0.1em',
    textTransform: 'uppercase', marginTop: 8, marginBottom: 4,
  };

  return (
    <div
      className="nodrag"
      onMouseDown={e => e.stopPropagation()}
      onMouseUp={e => e.stopPropagation()}
      onPointerDown={e => e.stopPropagation()}
      onPointerUp={e => e.stopPropagation()}
      onClick={e => e.stopPropagation()}
      style={{
        position: 'absolute', top: 0, left: '100%', marginLeft: 6,
        width: 200, background: 'var(--surface2)',
        border: '1px solid var(--border-hi)', borderRadius: 'var(--radius)',
        padding: '10px 12px', zIndex: 1000,
        boxShadow: '0 8px 32px rgba(0,0,0,.6)',
        fontFamily: "'JetBrains Mono', monospace",
        userSelect: 'none',
      }}>
      <SettingsPanelHeader
        title={title}
        onReset={() => commit({ port: 0, targetHost: '', oscAddress: '/patchy' })}
        onClose={onClose}
      />

      <div style={labelStyle}>Port</div>
      <input
        type="number" min={1} max={65535} value={port || ''}
        placeholder="e.g. 8000"
        onChange={e => commit({ port: parseInt(e.target.value, 10) || 0 })}
        style={inputStyle}
      />

      {isOut && (<>
        <div style={labelStyle}>Target Host</div>
        <input
          type="text" value={targetHost} placeholder="192.168.1.50"
          onChange={e => commit({ targetHost: e.target.value })}
          style={inputStyle}
        />

        <div style={labelStyle}>OSC Address</div>
        <input
          type="text" value={oscAddress} placeholder="/patchy"
          onChange={e => commit({ oscAddress: e.target.value || '/patchy' })}
          style={inputStyle}
        />
        <div style={{ fontSize: 9, color: accent, marginTop: 4, opacity: 0.7 }}>
          Must start with /
        </div>
      </>)}

      {!port && (
        <div style={{ fontSize: 9, color: '#ef5350', marginTop: 6 }}>
          Set a port to activate
        </div>
      )}
    </div>
  );
}

// ── OSC port summary label ────────────────────────────────────────────────────
function OscPortSummary ({ port, oscAddress, byteRate, onClick }: {
  port:       number;
  oscAddress: string;
  byteRate:   string;
  onClick:    () => void;
}) {
  const baseStyle: React.CSSProperties = {
    fontSize: 9, marginBottom: 3, letterSpacing: '0.05em',
    cursor: 'pointer', borderRadius: 3, padding: '2px 4px',
    transition: 'background .12s',
    display: 'flex', justifyContent: 'space-between', alignItems: 'center',
  };
  if (!port) {
    return (
      <div
        className="nodrag"
        onClick={onClick}
        style={{ ...baseStyle, color: '#ef5350', justifyContent: 'center' }}
        onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'rgba(239,83,80,.12)'; }}
        onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
      >
        No port set
      </div>
    );
  }
  return (
    <div
      className="nodrag"
      onClick={onClick}
      style={{ ...baseStyle, color: 'var(--text-muted)' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.background = 'var(--surface)'; }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.background = 'transparent'; }}
    >
      <span style={{ flex: 1, textAlign: 'center' }}>:{port}{oscAddress && oscAddress !== '/patchy' ? ` · ${oscAddress}` : ''}</span>
      {byteRate && <span style={{ color: 'var(--osc)', opacity: 0.85 }}>{byteRate}</span>}
    </div>
  );
}

// ── Port colour by type ───────────────────────────────────────────────────────
function portColour (type: string): string {
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

// ── Main node ─────────────────────────────────────────────────────────────────
function GenericNode({ id, data, selected }: NodeProps) {
  const nodeData = data as NodeData;
  // nodeType>=100 means addon: extract ngaType = nodeType-100 for theming
  const ngaType  = nodeData.nodeType >= 100 ? nodeData.nodeType - 100 : null;
  const paxTag = (() => {
    if (ngaType === null) return null;
    const prefix = ({ 1: 'MIDI', 2: 'AUDIO', 3: 'AV' } as Record<number,string>)[ngaType] ?? 'PLUGIN';
    const labelUp = nodeData.label.toUpperCase();
    // Avoid doubling the prefix (e.g. "AV Passthrough" → "AV PASSTHROUGH" not "AV AV PASSTHROUGH")
    const body = labelUp.startsWith(prefix + ' ') ? labelUp.slice(prefix.length + 1) : labelUp;
    return prefix + ' ' + body;
  })();
  const theme    = ngaType !== null
    ? ({ 1: { accent: 'var(--midi)',  dim: 'var(--midi-dim)',  glow: 'var(--midi-glow)',  tag: paxTag! },
          2: { accent: 'var(--audio)', dim: 'var(--audio-dim)', glow: 'var(--audio-glow)', tag: paxTag! },
          3: { accent: 'var(--av)',    dim: 'var(--av-dim)',    glow: 'var(--av-glow)',    tag: paxTag! },
        }[ngaType] ?? { ...PAX_THEME, tag: paxTag! })
    : (THEME[nodeData.nodeType] ?? PAX_THEME);

  const inputs  = nodeData.ports.filter(p => p.direction === 'input');
  const outputs = nodeData.ports.filter(p => p.direction === 'output');

  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const isPax = ngaType !== null;
  const isAudioDevice = nodeData.nodeType === 3 || nodeData.nodeType === 4;
  const isUdpDevice   = nodeData.nodeType === 8 || nodeData.nodeType === 9;
  const isOscDevice   = nodeData.nodeType === 10 || nodeData.nodeType === 11;
  const { setHint } = useContext(HintContext);
  const portBodyRef = useRef<HTMLDivElement>(null);

  // ── Audio device channel state ──────────────────────────────────────────────
  const { showSettings, toggleSettings, closeSettings } = useNodeSettings(id);
  const [selectedChannels, setSelectedChannels] = useState<number[]>(() => {
    if (!isAudioDevice) return [0, 1];
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson) : null;
      return Array.isArray(parsed?.selectedChannels) ? parsed.selectedChannels : [0, 1];
    } catch { return [0, 1]; }
  });
  const [deviceChannelCount, setDeviceChannelCount] = useState<number>(() => {
    if (!isAudioDevice) return 2;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson) : null;
      return parsed?.deviceChannelCount ?? 2;
    } catch { return 2; }
  });

  // Sync selectedChannels and deviceChannelCount when settingsJson changes (device open / undo / redo)
  useEffect(() => {
    if (!isAudioDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setSelectedChannels(Array.isArray(parsed?.selectedChannels) ? parsed.selectedChannels : [0, 1]);
      setDeviceChannelCount(parsed?.deviceChannelCount ?? 2);
    } catch {
      setSelectedChannels([0, 1]);
      setDeviceChannelCount(2);
    }
  }, [nodeData.settingsJson, isAudioDevice]);

  // ── UDP IN/OUT settings state ───────────────────────────────────────────────
  const [udpPort, setUdpPort] = useState<number>(0);
  const [udpMode, setUdpMode] = useState<0 | 1 | 2>(0); // 0=Unicast 1=Multicast 2=Broadcast
  const [udpTargetHost, setUdpTargetHost] = useState('');
  const [udpMulticastAddr, setUdpMulticastAddr] = useState('');

  useEffect(() => {
    if (!isUdpDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setUdpPort(parsed?.udpPort ?? 0);
      setUdpMode((parsed?.udpMode ?? 0) as 0 | 1 | 2);
      setUdpTargetHost(parsed?.udpTargetHost ?? '');
      setUdpMulticastAddr(parsed?.udpMulticastAddr ?? '');
    } catch {}
  }, [nodeData.settingsJson, isUdpDevice]);

  // Byte-rate label for UDP In nodes — subscribe to 30Hz port activity, compute B/s
  const [udpByteRate, setUdpByteRate] = useState<string>('');
  useEffect(() => {
    if (nodeData.nodeType !== 8) return;  // UDP In only
    const unsub = Bridge.onPortActivity((entries) => {
      const entry = entries.find(e => e.id === id);
      if (!entry) return;
      const bps = (entry.bytes ?? 0) * 30;  // 30Hz poll → bytes/sec
      if (bps === 0) { setUdpByteRate(''); return; }
      setUdpByteRate(bps >= 1024
        ? `${(bps / 1024).toFixed(1)} kB/s`
        : `${bps} B/s`);
    });
    return unsub;
  }, [id, nodeData.nodeType]);

  // ── OSC IN/OUT settings state ───────────────────────────────────────────────
  const [oscPort, setOscPort] = useState<number>(0);
  const [oscTargetHost, setOscTargetHost] = useState('');
  const [oscAddress, setOscAddress] = useState('/patchy');

  useEffect(() => {
    if (!isOscDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setOscPort(parsed?.oscPort ?? 0);
      setOscTargetHost(parsed?.oscTargetHost ?? '');
      setOscAddress(parsed?.oscAddress ?? '/patchy');
    } catch {}
  }, [nodeData.settingsJson, isOscDevice]);

  // Byte-rate label for OSC In nodes
  const [oscByteRate, setOscByteRate] = useState<string>('');
  useEffect(() => {
    if (nodeData.nodeType !== 10) return;  // OSC In only
    const unsub = Bridge.onPortActivity((entries) => {
      const entry = entries.find(e => e.id === id);
      if (!entry) return;
      const bps = (entry.bytes ?? 0) * 30;
      if (bps === 0) { setOscByteRate(''); return; }
      setOscByteRate(bps >= 1024
        ? `${(bps / 1024).toFixed(1)} kB/s`
        : `${bps} B/s`);
    });
    return unsub;
  }, [id, nodeData.nodeType]);

  // Track device channel count from the device list (fallback)
  const selectedDeviceIdRef = useRef(nodeData.selectedDeviceId);
  selectedDeviceIdRef.current = nodeData.selectedDeviceId;
  const nodeTypeRef = useRef(nodeData.nodeType);
  nodeTypeRef.current = nodeData.nodeType;

  // Warn + reset when device changes and current channel selection is out of range
  const [channelWarning, setChannelWarning] = useState(false);
  const selectedChannelsRef = useRef(selectedChannels);
  selectedChannelsRef.current = selectedChannels;

  useEffect(() => {
    if (!isAudioDevice) return;
    const unsub = Bridge.onAudioDeviceChanged((changedNodeId) => {
      if (changedNodeId !== id) return;
      const unsubOnce = Bridge.onAudioDevices(list => {
        const devs = nodeTypeRef.current === 4 ? list.audioOutDevices : list.audioInDevices;
        const dev = devs.find(d => d.id === selectedDeviceIdRef.current);
        if (dev?.channelCount == null) { unsubOnce(); return; }
        const maxCh = dev.channelCount;
        const invalid = selectedChannelsRef.current.some(c => c >= maxCh);
        if (invalid) {
          setChannelWarning(true);
          const reset = [...new Set([0, Math.min(1, maxCh - 1)])];
          Bridge.setNodeParam(id, 'audioDeviceChannels', JSON.stringify(reset), nodeTypeRef.current as 3 | 4);
        }
        unsubOnce();
      });
    });
    return unsub;
  }, [isAudioDevice, id]);
  const paxParams = (data.paxParams ?? []) as PaxParamInfo[];
  const [paramValues, setParamValues] = useState<number[]>([]);

  // Sync paramValues when paxParams arrive (may come after mount)
  // Also restore saved values from settingsJson if available
  useEffect(() => {
    if (paxParams.length > 0 && paramValues.length === 0) {
      const saved = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson) as number[] : null;
      const vals = saved && saved.length === paxParams.length
        ? saved
        : paxParams.map(p => p.defaultValue);
      setParamValues(vals);
      // Restore param values to C++ Pax
      vals.forEach((v, i) => Bridge.setPaxParameter(id, i, v));
    }
  }, [paxParams.length]);

  // Sync paramValues when settingsJson changes externally (e.g. undo/redo).
  useEffect(() => {
    if (paxParams.length === 0) return;
    const vals = nodeData.settingsJson
      ? (() => { try { return JSON.parse(nodeData.settingsJson) as number[]; } catch { return null; } })()
      : paxParams.map(p => p.defaultValue);
    if (!vals || vals.length !== paxParams.length) return;
    setParamValues(prev => {
      if (prev.length === vals.length && prev.every((v, i) => v === vals[i])) return prev;
      vals.forEach((v, i) => Bridge.setPaxParameter(id, i, v));
      return vals;
    });
  }, [nodeData.settingsJson, paxParams.length]);

  const onParamChange = useCallback((index: number, value: number) => {
    setParamValues(prev => {
      const next = [...prev]; next[index] = value;
      Bridge.setNodeSettings(id, next);
      return next;
    });
    Bridge.setPaxParameter(id, index, value);
  }, [id]);
  const [customName, setCustomName] = useState('');
  const onNameChange = useCallback((name: string) => {
    setCustomName(name);
    Bridge.setNodeLabel(id, name);
  }, [id]);

  return (
    <div
      style={{
        minWidth:   (isUdpDevice || isOscDevice) ? 310 : Math.max(theme.tag.length * 10 + 80, 220),
        userSelect: 'none',
        ...nodeContainerStyle(theme.accent, !!selected, { bg: 'var(--surface)', glow: theme.glow }),
      }}
    >
      {/* Header */}
      <div
        onDoubleClick={toggleCollapsed}
        onMouseEnter={e => {
          if (!e.currentTarget.contains(e.relatedTarget as Node)) {
            const paxName = nodeData.paxName as string | undefined;
            const h = (paxName ? NODE_HINTS[paxName] : null) ?? NODE_HINTS[String(theme.tag ?? '')] ?? null;
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

        {/* Pax name input */}
        {isPax && (
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

        {/* Audio device settings button */}
        {isAudioDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={() => { toggleSettings(); setChannelWarning(false); }}
              active={showSettings}
              activeAccent="var(--audio)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
            {channelWarning && !showSettings && (
              <div style={{
                position: 'absolute', top: -3, right: -3,
                width: 7, height: 7, borderRadius: '50%',
                background: '#ef5350', pointerEvents: 'none',
              }} />
            )}
          </div>
        )}

        {/* UDP device settings button */}
        {isUdpDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={toggleSettings}
              active={showSettings}
              activeAccent="var(--udp)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
            {!udpPort && !showSettings && (
              <div style={{
                position: 'absolute', top: -3, right: -3,
                width: 7, height: 7, borderRadius: '50%',
                background: '#ef5350', pointerEvents: 'none',
              }} />
            )}
          </div>
        )}

        {/* OSC device settings button */}
        {isOscDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={toggleSettings}
              active={showSettings}
              activeAccent="var(--osc)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
            {!oscPort && !showSettings && (
              <div style={{
                position: 'absolute', top: -3, right: -3,
                width: 7, height: 7, borderRadius: '50%',
                background: '#ef5350', pointerEvents: 'none',
              }} />
            )}
          </div>
        )}

        {/* Delete button */}
        <NodeHeaderButton onClick={handleDelete} danger
          onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.deleteNode), onMouseLeave: () => setHint(null) }}><X size={14} /></NodeHeaderButton>
      </div>

      {!collapsed && <>
      {/* Device selector — only renders for device nodes, provides portBodyRef anchor */}
      <div ref={portBodyRef} style={{ padding: !isPax ? '8px 10px' : '0',
                                         minHeight: isPax && paxParams.length === 0 ? 32 : undefined,
                                         position: 'relative' }}>
        {(nodeData.nodeType === 1 || nodeData.nodeType === 2) && (
          <DeviceSelector
            nodeId={id}
            nodeType={nodeData.nodeType as 1 | 2}
            selectedDeviceId={nodeData.selectedDeviceId}
          />
        )}
        {(nodeData.nodeType === 3 || nodeData.nodeType === 4) && (<>
          {nodeData.selectedDeviceId && <ChannelSummary channels={selectedChannels} />}
          <DeviceSelector
            nodeId={id}
            nodeType={nodeData.nodeType as 3 | 4}
            selectedDeviceId={nodeData.selectedDeviceId}
          />
          {showSettings && (
            <AudioDeviceSettingsPanel
              nodeId={id}
              nodeType={nodeData.nodeType as 3 | 4}
              selectedChannels={selectedChannels}
              deviceChannelCount={deviceChannelCount}
              selectedDeviceId={nodeData.selectedDeviceId}
              warning={channelWarning}
              onClose={closeSettings}
            />
          )}
        </>)}
        {isUdpDevice && (<>
          <UdpPortSummary port={udpPort} mode={udpMode} multicastAddr={udpMulticastAddr} byteRate={udpByteRate} onClick={toggleSettings} />
          {showSettings && (
            <UdpDeviceSettingsPanel
              nodeId={id}
              nodeType={nodeData.nodeType as 8 | 9}
              port={udpPort}
              mode={udpMode}
              targetHost={udpTargetHost}
              multicastAddr={udpMulticastAddr}
              onClose={closeSettings}
            />
          )}
        </>)}
        {isOscDevice && (<>
          <OscPortSummary port={oscPort} oscAddress={oscAddress} byteRate={oscByteRate} onClick={toggleSettings} />
          {showSettings && (
            <OscDeviceSettingsPanel
              nodeId={id}
              nodeType={nodeData.nodeType as 10 | 11}
              port={oscPort}
              targetHost={oscTargetHost}
              oscAddress={oscAddress}
              onClose={closeSettings}
            />
          )}
        </>)}
      </div>

      {/* Pax parameter sliders — outside port body so padding works correctly */}
      {isPax && paxParams.length > 0 && (
        <div className="nodrag" style={{ padding: '8px 10px 6px',
                                         borderTop: '1px solid var(--border)' }}>
          <div style={{ display: 'flex', justifyContent: 'flex-end', marginBottom: 2 }}>
            <NodeHeaderButton
              onClick={() => {
                const defaults = paxParams.map(p => p.defaultValue);
                setParamValues(defaults);
                Bridge.setNodeSettings(id, defaults);
                defaults.forEach((v, i) => Bridge.setPaxParameter(id, i, v));
                Bridge.commitNodeSettings(id);
              }}
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.reset), onMouseLeave: () => setHint(null) }}><span style={{ fontSize: 11, fontWeight: 700 }}>R</span></NodeHeaderButton>
          </div>
          {paxParams.map((p, i) => (
            <div key={i} style={{ marginBottom: 0, paddingTop: 8 }}>
              {/* Binary toggle for 0/1 integer params */}
              {p.step >= 1 && p.min === 0 && p.max === 1 ? (
                <div style={{ display: 'flex', justifyContent: 'space-between',
                              alignItems: 'center' }}>
                  <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{p.name}</span>
                  <div style={{ display: 'flex', gap: 4 }}>
                    {['Off', 'On'].map((label, val) => (
                      <div key={val}
                        onClick={() => { onParamChange(i, val); Bridge.commitNodeSettings(id); }}
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
                  onMouseUp={() => Bridge.commitNodeSettings(id)}
                  onKeyUp={() => Bridge.commitNodeSettings(id)}
                  onDoubleClick={() => { onParamChange(i, p.defaultValue); Bridge.commitNodeSettings(id); }}
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
          colour={isUdpDevice ? 'var(--udp)' : isOscDevice ? 'var(--osc)' : portColour(p.type)}
          index={i} total={inputs.length}
          offset={isPax ? 6 : 8}
          portBodyRef={portBodyRef}
          portId={p.id}
        />
      ))}

      {/* Output handles */}
      {outputs.map((p, i) => (
        <NodeHandle key={p.id}
          nodeId={id} label={p.label} direction="out"
          colour={isUdpDevice ? 'var(--udp)' : isOscDevice ? 'var(--osc)' : portColour(p.type)}
          index={i} total={outputs.length}
          offset={isPax ? 6 : 8}
          portBodyRef={portBodyRef}
          portId={p.id}
        />
      ))}
    </div>
  );
}

export default GenericNode;

// This line intentionally left blank — appending portColour helper below
