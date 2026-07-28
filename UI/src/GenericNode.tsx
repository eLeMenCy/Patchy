import { useCallback, useEffect, useRef, useState, useContext } from 'react';
import { HintContext, NODE_HINTS, BUTTON_HINTS } from './HintPanel';
import { DawContext } from './DawContext';
import { X } from 'lucide-react';
import { NodeProps } from '@xyflow/react';
import { Bridge, PaxParamInfo } from './Bridge';
import { useNodeDelete, NodeHeaderButton, useNodeCollapsed, useNodeSettings, NodeHandle, nodeContainerStyle, portColour, _paxInfoMap, detectPaxTheme, detectPaxTagPrefix } from './NodeUtils';

import { DeviceSelector, AudioDeviceSettingsPanel, ChannelSummary } from './AudioDeviceUI';
import { UdpPortSummary, UdpDeviceSettingsPanel } from './UdpDeviceUI';
import { OscDeviceSettingsPanel, OscPortSummary } from './OscDeviceUI';
import { MqttSubscribeSettingsPanel, MqttSubscribeSummary, MqttPublishSettingsPanel, MqttPublishSummary } from './MqttDeviceUI';
import { ArtNetDeviceSettingsPanel, ArtNetPortSummary } from './ArtNetDeviceUI';
import { DmxDeviceSelector, DmxSettingsPanel, DmxByteRateLabel } from './DmxDeviceUI';

export interface NodeData {
  label: string;
  nodeType: 1 | 2 | 3 | 4 | number;  // 1-4 built-in, higher = Pax
  paxName?: string;
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
  10: { accent: 'var(--osc)',    dim: 'var(--osc-dim)',    glow: 'var(--osc-glow)',    tag: 'OSC IN DEVICE'     },
  11: { accent: 'var(--osc)',    dim: 'var(--osc-dim)',    glow: 'var(--osc-glow)',    tag: 'OSC OUT DEVICE'    },
  12: { accent: 'var(--artnet)', dim: 'var(--artnet-dim)', glow: 'var(--artnet-glow)', tag: 'ARTNET IN DEVICE'  },
  13: { accent: 'var(--artnet)', dim: 'var(--artnet-dim)', glow: 'var(--artnet-glow)', tag: 'ARTNET OUT DEVICE' },
  14: { accent: 'var(--dmx)',    dim: 'var(--dmx-dim)',    glow: 'var(--dmx-glow)',    tag: 'DMX IN DEVICE'     },
  15: { accent: 'var(--dmx)',    dim: 'var(--dmx-dim)',    glow: 'var(--dmx-glow)',    tag: 'DMX OUT DEVICE'    },
  16: { accent: 'var(--dmx)',    dim: 'var(--dmx-dim)',    glow: 'var(--dmx-glow)',    tag: 'DMX MONITOR'       },
  17: { accent: 'var(--dmx)',    dim: 'var(--dmx-dim)',    glow: 'var(--dmx-glow)',    tag: 'DMX CONSOLE'       },
  18: { accent: 'var(--artnet)', dim: 'var(--artnet-dim)', glow: 'var(--artnet-glow)', tag: 'ARTNET MONITOR'    },
  19: { accent: 'var(--artnet)', dim: 'var(--artnet-dim)', glow: 'var(--artnet-glow)', tag: 'ARTNET CONSOLE'    },
  22: { accent: 'var(--mqtt)',   dim: 'var(--mqtt-dim)',   glow: 'var(--mqtt-glow)',   tag: 'MQTT SUBSCRIBE'   },
  23: { accent: 'var(--mqtt)',   dim: 'var(--mqtt-dim)',   glow: 'var(--mqtt-glow)',   tag: 'MQTT PUBLISH'     },
};
// Default theme for Pax nodes
const PAX_THEME = { accent: 'var(--av)', dim: 'var(--av-dim)', glow: 'var(--av-glow)', tag: 'PAX' };

// ── Main node ─────────────────────────────────────────────────────────────────
function GenericNode({ id, data, selected }: NodeProps) {
  const nodeData = data as NodeData;
  // nodeType>=100 means addon
  const ngaType  = nodeData.nodeType >= 100 ? nodeData.nodeType - 100 : null;
  const paxColourCategory = nodeData.paxName ? _paxInfoMap.get(nodeData.paxName)?.colourCategory : undefined;
  const paxTag = (() => {
    if (ngaType === null) return null;
    const prefix = detectPaxTagPrefix (nodeData.ports, paxColourCategory);
    const labelUp = nodeData.label.toUpperCase();
    // Skip prepending the category if it already appears anywhere in the
    // label — not just as a strict prefix. "AV Passthrough" already
    // avoided "AV AV PASSTHROUGH" via startsWith, but "MQTT to Value"
    // has "Value" at the end, not the start, so a plain startsWith check
    // missed it and produced "VALUE MQTT TO VALUE".
    return labelUp.includes(prefix) ? labelUp : prefix + ' ' + labelUp;
  })();
  const theme    = ngaType !== null
    ? { ...detectPaxTheme (nodeData.ports, paxColourCategory), tag: paxTag! }
    : (THEME[nodeData.nodeType] ?? PAX_THEME);

  const inputs  = nodeData.ports.filter(p => p.direction === 'input');
  const outputs = nodeData.ports.filter(p => p.direction === 'output');

  const { handleDelete } = useNodeDelete(id);
  const { collapsed, toggleCollapsed } = useNodeCollapsed(id, (data as any)._forceCollapsed);
  const isPax = ngaType !== null;
  const isAudioDevice = nodeData.nodeType === 3 || nodeData.nodeType === 4;
  const isUdpDevice    = nodeData.nodeType === 8  || nodeData.nodeType === 9;
  const isOscDevice    = nodeData.nodeType === 10 || nodeData.nodeType === 11;
  const isMqttSubscribeDevice = nodeData.nodeType === 22;
  const isMqttPublishDevice = nodeData.nodeType === 23;
  const isArtNetDevice = nodeData.nodeType === 12 || nodeData.nodeType === 13;
  const isDmxDevice    = nodeData.nodeType === 14 || nodeData.nodeType === 15;
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

  // ── MQTT Subscribe settings state ───────────────────────────────────────────
  const [mqttHost, setMqttHost] = useState('');
  const [mqttPort, setMqttPort] = useState<number>(1883);
  const [mqttTopic, setMqttTopic] = useState('');
  const [mqttQos, setMqttQos] = useState<0 | 1 | 2>(1);
  const [mqttUsername, setMqttUsername] = useState('');
  const [mqttPassword, setMqttPassword] = useState('');

  useEffect(() => {
    if (!isMqttSubscribeDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setMqttHost(parsed?.mqttHost ?? '');
      setMqttPort(parsed?.mqttPort ?? 1883);
      setMqttTopic(parsed?.mqttTopic ?? '');
      setMqttQos((parsed?.mqttQos ?? 1) as 0 | 1 | 2);
      setMqttUsername(parsed?.mqttUsername ?? '');
      setMqttPassword(parsed?.mqttPassword ?? '');
    } catch {}
  }, [nodeData.settingsJson, isMqttSubscribeDevice]);

  // ── MQTT Publish settings state ─────────────────────────────────────────────
  const [mqttPubHost, setMqttPubHost] = useState('');
  const [mqttPubPort, setMqttPubPort] = useState<number>(1883);
  const [mqttPubTopic, setMqttPubTopic] = useState('');
  const [mqttPubQos, setMqttPubQos] = useState<0 | 1 | 2>(1);
  const [mqttPubRetain, setMqttPubRetain] = useState(false);
  const [mqttPubUsername, setMqttPubUsername] = useState('');
  const [mqttPubPassword, setMqttPubPassword] = useState('');

  useEffect(() => {
    if (!isMqttPublishDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setMqttPubHost(parsed?.mqttHost ?? '');
      setMqttPubPort(parsed?.mqttPort ?? 1883);
      setMqttPubTopic(parsed?.mqttTopic ?? '');
      setMqttPubQos((parsed?.mqttQos ?? 1) as 0 | 1 | 2);
      setMqttPubRetain(parsed?.mqttRetain ?? false);
      setMqttPubUsername(parsed?.mqttUsername ?? '');
      setMqttPubPassword(parsed?.mqttPassword ?? '');
    } catch {}
  }, [nodeData.settingsJson, isMqttPublishDevice]);

  // ArtNet state
  const [artNetUniverse,   setArtNetUniverse]   = useState<number>(0);
  const [artNetTargetHost, setArtNetTargetHost] = useState('');

  useEffect(() => {
    if (!isArtNetDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setArtNetUniverse(parsed?.artNetUniverse ?? 0);
      setArtNetTargetHost(parsed?.artNetTargetHost ?? '');
    } catch {}
  }, [nodeData.settingsJson, isArtNetDevice]);

  // Byte-rate label for ArtNet In nodes
  const [artNetByteRate, setArtNetByteRate] = useState<string>('');
  useEffect(() => {
    if (nodeData.nodeType !== 12) return;  // ArtNet In only
    const unsub = Bridge.onPortActivity((entries) => {
      const entry = entries.find(e => e.id === id);
      if (!entry) return;
      const bps = (entry.bytes ?? 0) * 30;
      if (bps === 0) { setArtNetByteRate(''); return; }
      setArtNetByteRate(bps >= 1024
        ? `${(bps / 1024).toFixed(1)} kB/s`
        : `${bps} B/s`);
    });
    return unsub;
  }, [id, nodeData.nodeType]);

  // DMX state
  const [dmxDevicePath, setDmxDevicePath] = useState('');
  const [dmxUniverse,   setDmxUniverse]   = useState(0);
  const [serialPorts,   setSerialPorts]   = useState<string[]>([]);

  useEffect(() => {
    if (!isDmxDevice) return;
    try {
      const parsed = nodeData.settingsJson ? JSON.parse(nodeData.settingsJson as string) : null;
      setDmxDevicePath(parsed?.dmxDevicePath ?? '');
      setDmxUniverse(parsed?.dmxUniverse ?? 0);
    } catch {}
  }, [nodeData.settingsJson, isDmxDevice]);

  // Subscribe to serial port list (shared across all DMX nodes)
  useEffect(() => {
    if (!isDmxDevice) return;
    const unsub = Bridge.onSerialPorts(setSerialPorts);
    return unsub;
  }, [isDmxDevice]);

  // Byte-rate label for DMX In nodes
  const [dmxByteRate, setDmxByteRate] = useState<string>('');
  const [dmxIsMk2,    setDmxIsMk2]    = useState(false);
  useEffect(() => {
    if (!isDmxDevice) return;
    const unsub = Bridge.onPortActivity((entries) => {
      const entry = entries.find(e => e.id === id);
      if (!entry) return;
      // isMk2 reported by both In and Out nodes
      if (entry.isMk2 !== undefined) setDmxIsMk2(entry.isMk2);
      if (nodeData.nodeType !== 14) return;  // byte-rate for In only
      const bps = (entry.bytes ?? 0) * 30;
      if (bps === 0) { setDmxByteRate(''); return; }
      setDmxByteRate(bps >= 1024
        ? `${(bps / 1024).toFixed(1)} kB/s`
        : `${bps} B/s`);
    });
    return unsub;
  }, [id, nodeData.nodeType, isDmxDevice]);

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
      // Was: all-or-nothing on saved.length === paxParams.length — any
      // mismatch (e.g. a param added/removed since this settingsJson was
      // last written) silently discarded EVERY saved value, not just the
      // new one. Now maps per-index so existing values survive and only
      // genuinely-missing indices fall back to their own default.
      const vals = paxParams.map((p, i) => (saved && saved[i] !== undefined) ? saved[i] : p.defaultValue);
      setParamValues(vals);
      // Restore param values to C++ Pax
      vals.forEach((v, i) => Bridge.setPaxParameter(id, i, v));
    }
  }, [paxParams.length]);

  // Sync paramValues when settingsJson changes externally (e.g. undo/redo).
  useEffect(() => {
    if (paxParams.length === 0) return;
    const raw = nodeData.settingsJson
      ? (() => { try { return JSON.parse(nodeData.settingsJson) as number[]; } catch { return null; } })()
      : null;
    // Was: `if (!vals || vals.length !== paxParams.length) return;` — a
    // settingsJson snapshot with a different length than the Pax's
    // CURRENT param count (e.g. an older undo entry from before a
    // parameter was added) silently no-op'd this entire sync, leaving
    // paramValues — and therefore every displayed control, not just the
    // new one — stuck out of sync with the graph's actual restored state.
    // Same per-index defaulting as the mount-time effect above instead.
    const vals = paxParams.map((p, i) => (raw && raw[i] !== undefined) ? raw[i] : p.defaultValue);
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
        minWidth:   (isUdpDevice || isOscDevice || isMqttSubscribeDevice || isMqttPublishDevice || isArtNetDevice || isDmxDevice) ? 310 : Math.max(theme.tag.length * 10 + 80, 220),
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

        {/* MQTT Subscribe settings button */}
        {isMqttSubscribeDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={toggleSettings}
              active={showSettings}
              activeAccent="var(--mqtt)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
            {(!mqttHost || !mqttTopic) && !showSettings && (
              <div style={{
                position: 'absolute', top: -3, right: -3,
                width: 7, height: 7, borderRadius: '50%',
                background: '#ef5350', pointerEvents: 'none',
              }} />
            )}
          </div>
        )}
        {/* MQTT Publish settings button */}
        {isMqttPublishDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={toggleSettings}
              active={showSettings}
              activeAccent="var(--mqtt)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
            {(!mqttPubHost || !mqttPubTopic) && !showSettings && (
              <div style={{
                position: 'absolute', top: -3, right: -3,
                width: 7, height: 7, borderRadius: '50%',
                background: '#ef5350', pointerEvents: 'none',
              }} />
            )}
          </div>
        )}
        {/* ArtNet device settings button */}
        {isArtNetDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={toggleSettings}
              active={showSettings}
              activeAccent="var(--artnet)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
            {!artNetTargetHost && nodeData.nodeType === 13 && !showSettings && (
              <div style={{
                position: 'absolute', top: -3, right: -3,
                width: 7, height: 7, borderRadius: '50%',
                background: '#ef5350', pointerEvents: 'none',
              }} />
            )}
          </div>
        )}

        {/* DMX device settings button */}
        {isDmxDevice && (
          <div style={{ position: 'relative', display: 'inline-flex' }}>
            <NodeHeaderButton
              onClick={toggleSettings}
              active={showSettings}
              activeAccent="var(--dmx)"
              onHint={{ onMouseEnter: () => setHint(BUTTON_HINTS.settings), onMouseLeave: () => setHint(null) }}>
              <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
                <circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/>
              </svg>
            </NodeHeaderButton>
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
          <UdpPortSummary port={udpPort} mode={udpMode} targetHost={udpTargetHost} multicastAddr={udpMulticastAddr} byteRate={udpByteRate} onClick={toggleSettings} />
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
          <OscPortSummary port={oscPort} targetHost={oscTargetHost} oscAddress={oscAddress} byteRate={oscByteRate} onClick={toggleSettings} />
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
        {isMqttSubscribeDevice && (<>
          <MqttSubscribeSummary host={mqttHost} port={mqttPort} topic={mqttTopic} onClick={toggleSettings} />
          {showSettings && (
            <MqttSubscribeSettingsPanel
              nodeId={id}
              host={mqttHost}
              port={mqttPort}
              topic={mqttTopic}
              qos={mqttQos}
              username={mqttUsername}
              password={mqttPassword}
              onClose={closeSettings}
            />
          )}
        </>)}
        {isMqttPublishDevice && (<>
          <MqttPublishSummary host={mqttPubHost} port={mqttPubPort} topic={mqttPubTopic} onClick={toggleSettings} />
          {showSettings && (
            <MqttPublishSettingsPanel
              nodeId={id}
              host={mqttPubHost}
              port={mqttPubPort}
              topic={mqttPubTopic}
              qos={mqttPubQos}
              retain={mqttPubRetain}
              username={mqttPubUsername}
              password={mqttPubPassword}
              onClose={closeSettings}
            />
          )}
        </>)}
        {isArtNetDevice && (<>
          <ArtNetPortSummary universe={artNetUniverse} targetHost={artNetTargetHost} byteRate={artNetByteRate} onClick={toggleSettings} />
          {showSettings && (
            <ArtNetDeviceSettingsPanel
              nodeId={id}
              nodeType={nodeData.nodeType as 12 | 13}
              universe={artNetUniverse}
              targetHost={artNetTargetHost}
              onClose={closeSettings}
            />
          )}
        </>)}
        {isDmxDevice && (<>
          <DmxByteRateLabel byteRate={dmxByteRate} />
          <DmxDeviceSelector
            nodeId={id}
            devicePath={dmxDevicePath}
            universe={dmxUniverse}
            serialPorts={serialPorts}
          />
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginTop: 3 }}>
            <span style={{ fontSize: 9, color: 'var(--text-muted)', opacity: 0.6 }}>
              uni {dmxUniverse}
            </span>
            <span
              onClick={() => Bridge.listSerialPorts()}
              className="nodrag"
              style={{ fontSize: 9, color: 'var(--text-muted)', cursor: 'pointer', opacity: 0.6 }}
              onMouseEnter={e => { (e.currentTarget as HTMLSpanElement).style.opacity = '1'; }}
              onMouseLeave={e => { (e.currentTarget as HTMLSpanElement).style.opacity = '0.6'; }}
            >
              ↺ refresh
            </span>
          </div>
          {showSettings && (
            <DmxSettingsPanel
              nodeId={id}
              nodeType={nodeData.nodeType as 14 | 15}
              devicePath={dmxDevicePath}
              universe={dmxUniverse}
              isMk2={dmxIsMk2}
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
              ) : p.name === 'DMX Channel' ? (
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
                <span style={{ fontSize: 9, color: 'var(--text-muted)' }}>{p.name}</span>
                <input type="number"
                  className="nodrag"
                  min={p.min} max={p.max} step={1}
                  value={Math.round(paramValues[i] !== undefined ? paramValues[i] : p.defaultValue)}
                  onChange={e => {
                    const raw = parseInt(e.target.value, 10);
                    if (isNaN(raw)) return;
                    onParamChange(i, Math.min(p.max, Math.max(p.min, raw)));
                  }}
                  onBlur={() => Bridge.commitNodeSettings(id)}
                  onKeyDown={e => { if (e.key === 'Enter') { (e.target as HTMLInputElement).blur(); } }}
                  onMouseEnter={() => setHint({ title: p.name, body: `1-based DMX channel this node's output writes to (1-${p.max}).` })}
                  onMouseLeave={() => setHint(null)}
                  style={{
                    width: 44, fontSize: 9, textAlign: 'right',
                    background: 'var(--surface)', color: theme.accent,
                    border: '1px solid var(--border)', borderRadius: 3,
                    padding: '2px 4px',
                  }}
                />
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
                 
                  style={{ width: '100%', display: 'block', ['--thumb-color' as any]: theme.accent }}
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
              {/* label + value - only for non-binary, non-DMX-Channel params (DMX Channel renders its own label) */}
              {!(p.step >= 1 && p.min === 0 && p.max === 1) && p.name !== 'DMX Channel' && (
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
          colour={isUdpDevice ? 'var(--udp)' : isOscDevice ? 'var(--osc)' : isMqttSubscribeDevice ? 'var(--mqtt)' : isMqttPublishDevice ? 'var(--mqtt)' : isArtNetDevice ? 'var(--artnet)' : isDmxDevice ? 'var(--dmx)' : portColour(p.type)}
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
          colour={isUdpDevice ? 'var(--udp)' : isOscDevice ? 'var(--osc)' : isMqttSubscribeDevice ? 'var(--mqtt)' : isMqttPublishDevice ? 'var(--mqtt)' : isArtNetDevice ? 'var(--artnet)' : isDmxDevice ? 'var(--dmx)' : portColour(p.type)}
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
