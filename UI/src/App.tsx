import { useCallback, useEffect, useRef, useState, useContext, DragEvent, ReactNode } from 'react';
import { flushSync } from 'react-dom';
import { DawContext } from './DawContext';
import {
  ReactFlow,
  ReactFlowProvider,
  Background,
  Controls,
  BackgroundVariant,
  Connection,
  EdgeChange,
  NodeChange,
  addEdge,
  applyEdgeChanges,
  applyNodeChanges,
  Node,
  Edge,
  useReactFlow,
  useUpdateNodeInternals,
  reconnectEdge,
} from '@xyflow/react';
import '@xyflow/react/dist/style.css';

import { Bridge, FileState, AudioSettings, GraphState, RawNode, RawConnection, PortActivityEntry, PaxParamInfo, PaxInfo, FragmentData, UndoState } from './Bridge';
import GenericNode, { NodeData } from './GenericNode';
import MidiMonitorNode,      { MidiMonitorNodeData }      from './MidiMonitorNode';
import AudioMonitorNode,   { AudioMonitorNodeData }   from './AudioMonitorNode';
import MidiKeyboardNode,  { MidiKeyboardNodeData }  from './MidiKeyboardNode';
import { DmxMonitorNode, DmxMonitorNodeData } from './DmxMonitorNode';
import { DmxConsoleNode } from './DmxConsoleNode';
import { ArtNetMonitorNode } from './ArtNetMonitorNode';
import { ArtNetConsoleNode } from './ArtNetConsoleNode';
import { OscMonitorNode, OscMonitorNodeData } from './OscMonitorNode';
import { UdpMonitorNode, UdpMonitorNodeData } from './UdpMonitorNode';
import { MqttMonitorNode, MqttMonitorNodeData } from './MqttMonitorNode';
import { MqttConsoleNode, MqttConsoleNodeData } from './MqttConsoleNode';
import PreferencesPanel, { GraphPreferences, loadPrefs, savePrefs } from './PreferencesPanel';
import { HintProvider, HintContext, BUTTON_HINTS, PORT_HINTS, EDGE_HINTS } from './HintPanel';
import { Menu, ChevronsDownUp, ChevronsUpDown, Settings, ChevronLeft } from 'lucide-react';
import Sidebar from './Sidebar';
import SpectrumyserNode from './SpectrumyserNode';
import EnvelopeNode     from './EnvelopeNode';
import { _paxInfoMap } from './NodeUtils';

// ── Node type registry ────────────────────────────────────────────────────────
const nodeTypes = { custom: GenericNode, midiMonitor: MidiMonitorNode, audioMonitor: AudioMonitorNode, midiKeyboard: MidiKeyboardNode, spectrumyser: SpectrumyserNode, envelope: EnvelopeNode, dmxMonitor: DmxMonitorNode, dmxConsole: DmxConsoleNode, artNetMonitor: ArtNetMonitorNode, artNetConsole: ArtNetConsoleNode, oscMonitor: OscMonitorNode, udpMonitor: UdpMonitorNode, mqttMonitor: MqttMonitorNode, mqttConsole: MqttConsoleNode };

// ── Conversion helpers ────────────────────────────────────────────────────────
// _paxInfoMap lives in NodeUtils.tsx (not declared here) — GenericNode.tsx
// also needs to read it (for colourCategory), and App.tsx already imports
// GenericNode to register it as a node type, so declaring it here would
// create a circular import. NodeUtils.tsx is a lower-level shared utility
// file both already depend on safely.

function rawToFlowNode(raw: RawNode, paxInfoMap?: Map<string, PaxInfo>): Node<NodeData | MidiMonitorNodeData | DmxMonitorNodeData | OscMonitorNodeData | UdpMonitorNodeData | MqttMonitorNodeData | MqttConsoleNodeData> {
  const isMidiMonitor    = raw.nodeType === 5;
  const isAudioMonitor   = raw.nodeType === 6;
  const isMidiKeyboard   = raw.nodeType === 7;
  const isDmxMonitor     = raw.nodeType === 16;
  const isDmxConsole     = raw.nodeType === 17;
  const isArtNetMonitor  = raw.nodeType === 18;
  const isArtNetConsole  = raw.nodeType === 19;
  const isOscMonitor     = raw.nodeType === 20;
  const isUdpMonitor     = raw.nodeType === 21;
  const isMqttMonitor    = raw.nodeType === 24;
  const isMqttConsole    = raw.nodeType === 25;
  return {
    id:       raw.id,
    type:     isMidiMonitor    ? 'midiMonitor'
            : isAudioMonitor   ? 'audioMonitor'
            : isMidiKeyboard   ? 'midiKeyboard'
            : isDmxMonitor     ? 'dmxMonitor'
            : isDmxConsole     ? 'dmxConsole'
            : isArtNetMonitor  ? 'artNetMonitor'
            : isArtNetConsole  ? 'artNetConsole'
            : isOscMonitor     ? 'oscMonitor'
            : isUdpMonitor     ? 'udpMonitor'
            : isMqttMonitor    ? 'mqttMonitor'
            : isMqttConsole    ? 'mqttConsole'
            : raw.paxName === 'Spectrumyser' ? 'spectrumyser'
            : raw.paxName === 'Envelope'     ? 'envelope' : 'custom',
    position: { x: raw.x, y: raw.y },
    data: isMidiMonitor
      ? { label: raw.label, nodeType: 5,  ports: raw.ports, settingsJson: raw.settingsJson } as MidiMonitorNodeData
      : isAudioMonitor
      ? { label: raw.label, nodeType: 6,  ports: raw.ports, settingsJson: raw.settingsJson } as AudioMonitorNodeData
      : isMidiKeyboard
      ? { label: raw.label, nodeType: 7,  ports: raw.ports, settingsJson: raw.settingsJson } as MidiKeyboardNodeData
      : isDmxMonitor
      ? { label: raw.label, nodeType: 16, ports: raw.ports, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isDmxConsole
      ? { label: raw.label, nodeType: 17, ports: raw.ports, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isArtNetMonitor
      ? { label: raw.label, nodeType: 18, ports: raw.ports, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isArtNetConsole
      ? { label: raw.label, nodeType: 19, ports: raw.ports, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isOscMonitor
      ? { label: raw.label, nodeType: 20, ports: raw.ports, settingsJson: raw.settingsJson } as OscMonitorNodeData
      : isUdpMonitor
      ? { label: raw.label, nodeType: 21, ports: raw.ports, settingsJson: raw.settingsJson } as UdpMonitorNodeData
      : isMqttMonitor
      ? { label: raw.label, nodeType: 24, ports: raw.ports, settingsJson: raw.settingsJson } as MqttMonitorNodeData
      : isMqttConsole
      ? { label: raw.label, nodeType: 25, ports: raw.ports, settingsJson: raw.settingsJson } as MqttConsoleNodeData
      : { label: raw.label, nodeType: raw.nodeType,
          ports: raw.ports, selectedDeviceId: raw.selectedDeviceId,
          paxName: raw.paxName,
          paxParams: raw.paxName ? (paxInfoMap?.get(raw.paxName)?.params ?? []) : [],
          settingsJson: raw.settingsJson } as NodeData,
  };
}

function rawToFlowEdge(raw: RawConnection): Edge {
  const src = raw.sourcePortId.toLowerCase();
  const cls = src.includes('audio')   ? 'edge-audio'
            : src.includes('osc')     ? 'edge-osc'
            : src.includes('artdmx')  ? 'edge-artnet'   // must come before 'dmx'
            : src.includes('dmx')     ? 'edge-dmx'
            : src.includes('mqtt')    ? 'edge-mqtt'
            : src.includes('udp')     ? 'edge-udp'
            : src.includes('value')   ? 'edge-value'   // Pax adapter/converter ports (Phase 4)
            : 'edge-midi';
  return {
    id:           raw.id,
    source:       raw.sourceNodeId,
    sourceHandle: raw.sourcePortId,
    target:       raw.targetNodeId,
    targetHandle: raw.targetPortId,
    className:    cls,
    style:        { strokeWidth: 2 },
  };
}

// ── VU colour helper ─────────────────────────────────────────────────────────
function rmsToColour (rms: number): string {
  // Convert linear RMS to dBFS, then map to colour
  // -60 dBFS → dark green,  -18 dBFS → green,  -6 dBFS → yellow,  0 dBFS → red
  if (rms <= 0) return 'rgb(20,80,20)';
  const db = 20 * Math.log10(rms);           // linear → dBFS
  const v  = Math.max(0, Math.min(1, (db + 60) / 60));  // -60..0 dBFS → 0..1

  if (v < 0.7) {
    // dark green → yellow-green (up to -18 dBFS)
    const t = v / 0.7;
    const r = Math.round(20  + t * 200);
    const g = Math.round(80  + t * 120);
    return `rgb(${r},${g},20)`;
  }
  // yellow-green → red (above -18 dBFS)
  const t = (v - 0.7) / 0.3;
  const r = 220;
  const g = Math.round(200 - t * 180);
  return `rgb(${r},${g},20)`;
}

function rmsToGlow (rms: number, col: string): string {
  const px = Math.round(4 + rms * 10);
  return `0 0 ${px}px ${col}`;
}

// ── Port activity — dynamic CSS injection ─────────────────────────────────────
// Maps a port/handle id string to its display colour, purely from label
// text (same convention as every other classifier in this file — ports
// are typed by their label, not a separate lookup). Module-level since
// both usePortActivityStyles (per-port Pax value flash) and the main App
// component (drag-preview line colour) need the exact same mapping.
function colourForHandleId (handleId: string): string {
  const h = handleId.toLowerCase();
  if (h.includes('audio'))   return 'var(--audio)';
  if (h.includes('artdmx'))  return 'var(--artnet)';  // must come before 'dmx'
  if (h.includes('osc'))     return 'var(--osc)';
  if (h.includes('dmx'))     return 'var(--dmx)';
  if (h.includes('mqtt'))    return 'var(--mqtt)';
  if (h.includes('udp'))     return 'var(--udp)';
  if (h.includes('value'))   return 'var(--value)';  // Pax adapter/converter ports (Phase 4)
  if (h.includes('midi'))    return 'var(--midi)';
  return 'var(--accent)';
}

function usePortActivityStyles (edges: any[], nodes: any[]) {
  const styleRef      = useRef<HTMLStyleElement | null>(null);
  const midiTimers    = useRef<Map<string, number>>(new Map());
  const udpTimers     = useRef<Map<string, number>>(new Map());
  const oscTimers     = useRef<Map<string, number>>(new Map());
  const artNetTimers  = useRef<Map<string, number>>(new Map());
  const dmxTimers     = useRef<Map<string, number>>(new Map());
  const mqttTimers    = useRef<Map<string, number>>(new Map());
  const paxValueTimers = useRef<Map<string, number>>(new Map());
  const audioLevels   = useRef<Map<string, number>>(new Map());
  const portRmsLevels = useRef<Map<string, number[]>>(new Map());
  // Current DMX channel level per node (0-1), from the backend's own
  // persisted "last real value" slot — see WebBridge.h's PortActivity.
  // No decay applied, unlike audioLevels: a DMX channel holds a value
  // rather than firing transient events, so it should read as steady,
  // not fade out between updates.
  const dmxValues     = useRef<Map<string, number>>(new Map());
  const edgeList      = useRef(edges);
  const nodesRef      = useRef(nodes);
  useEffect(() => { edgeList.current = edges; }, [edges]);
  useEffect(() => { nodesRef.current = nodes;  }, [nodes]);

  // Create style tag once
  useEffect(() => {
    const el = document.createElement('style');
    el.id = 'port-activity-styles';
    document.head.appendChild(el);
    styleRef.current = el;
    return () => el.remove();
  }, []);

  // Receive port activity data
  useEffect(() => {
    return Bridge.onPortActivity((entries: PortActivityEntry[]) => {
      const now = Date.now();
      entries.forEach(entry => {
        if (entry.midi > 0) {
          const nodeType = (nodesRef.current.find((n: any) => n.id === entry.id)?.data as any)?.nodeType;
          if (nodeType === 8 || nodeType === 9 || nodeType === 21) {
            udpTimers.current.set(entry.id, now + 80);
          } else if (nodeType === 10 || nodeType === 11 || nodeType === 20) {
            oscTimers.current.set(entry.id, now + 80);
          } else if (nodeType === 12 || nodeType === 13 || nodeType === 18 || nodeType === 19) {
            artNetTimers.current.set(entry.id, now + 80);
          } else if (nodeType === 14 || nodeType === 15 || nodeType === 16 || nodeType === 17) {
            dmxTimers.current.set(entry.id, now + 80);
          } else if (nodeType === 22 || nodeType === 23 || nodeType === 24 || nodeType === 25) {
            mqttTimers.current.set(entry.id, now + 80);
          } else if (nodeType >= 100) {
            // Pax node (ngaType = nodeType - 100) — never matches any of
            // the built-in nodeType checks above, so it used to fall
            // through to the generic MIDI bucket regardless of what its
            // actual declared port types are. Own bucket now, coloured
            // per-port in the CSS injection below rather than with one
            // fixed colour, since a Pax can have several differently
            // typed value ports on the same node.
            paxValueTimers.current.set(entry.id, now + 80);
          } else {
            midiTimers.current.set(entry.id, now + 80);
          }
        }
        const rms  = Math.min(Math.max(entry.l, entry.r) / 1000 * 4, 1.0);
        const prev = audioLevels.current.get(entry.id) ?? 0;
        audioLevels.current.set(entry.id, Math.max(rms, prev * 0.88));
        if (entry.portRms && entry.portRms.length > 1) {
          const prevPorts = portRmsLevels.current.get(entry.id) ?? [];
          portRmsLevels.current.set(entry.id, entry.portRms.map((v, i) =>
            Math.max(Math.min((v / 1000) * 4, 1.0), (prevPorts[i] ?? 0) * 0.88)));
        } else {
          portRmsLevels.current.delete(entry.id);
        }
        // Always record — harmless 0 for non-DMX nodes, never read for them
        // since the render block below gates on actual DMX node/port identity,
        // not on this map's presence.
        dmxValues.current.set(entry.id, (entry.dmxValue ?? 0) / 1000);
      });
    });
  }, []);

  // RAF render loop — injects CSS for all active nodes/edges
  useEffect(() => {
    let rafId: number;
    const render = () => {
      const style = styleRef.current;
      if (!style) { rafId = requestAnimationFrame(render); return; }
      const now = Date.now();

      // Decay levels each frame
      audioLevels.current.forEach((v, k) => audioLevels.current.set(k, v * 0.97));
      portRmsLevels.current.forEach((ports, k) =>
        portRmsLevels.current.set(k, ports.map(v => v * 0.97)));

      // Collect active node ids (audio + midi sources, including unconnected)
      const audioEdges = edgeList.current.filter(e => (e.sourceHandle ?? '').toLowerCase().includes('audio'));
      const midiEdges  = edgeList.current.filter(e => (e.sourceHandle ?? '').toLowerCase().includes('midi'));
      const udpEdges   = edgeList.current.filter(e => (e.sourceHandle ?? '').toLowerCase().includes('udp'));
      const mqttEdges  = edgeList.current.filter(e => (e.sourceHandle ?? '').toLowerCase().includes('mqtt'));
      const oscEdges   = edgeList.current.filter(e => (e.sourceHandle ?? '').toLowerCase().includes('osc'));
      const artNetEdges = edgeList.current.filter(e => (e.sourceHandle ?? '').toLowerCase().includes('artdmx'));
      const dmxEdges    = edgeList.current.filter(e => {
        const h = (e.sourceHandle ?? '').toLowerCase();
        return h.includes('dmx') && !h.includes('artdmx');
      });
      const audioSources = new Set<string>(audioEdges.map(e => e.source));
      audioLevels.current.forEach((rms, id) => { if (rms > 0.01) audioSources.add(id); });
      const midiSources  = new Set<string>(midiEdges.map(e => e.source));
      midiTimers.current.forEach((expiry, id) => { if (expiry > now) midiSources.add(id); });
      const udpSources   = new Set<string>(udpEdges.map(e => e.source));
      udpTimers.current.forEach((expiry, id) => { if (expiry > now) udpSources.add(id); });
      const oscSources   = new Set<string>(oscEdges.map(e => e.source));
      oscTimers.current.forEach((expiry, id) => { if (expiry > now) oscSources.add(id); });
      const artNetSources = new Set<string>(artNetEdges.map(e => e.source));
      artNetTimers.current.forEach((expiry, id) => { if (expiry > now) artNetSources.add(id); });
      const dmxSources    = new Set<string>(dmxEdges.map(e => e.source));
      // dmxTimers/dmxSources no longer drive DMX's own rendering (see the
      // dedicated continuous-intensity block after this loop) — a node
      // landing in allSources via this set is now a harmless no-op for it,
      // not incorrect; left in place rather than ripped out, since audio/
      // midi/etc still need allSources and this is the least-risk way to
      // keep that working unchanged.
      dmxTimers.current.forEach((expiry, id) => { if (expiry > now) dmxSources.add(id); });
      const mqttSources   = new Set<string>();
      mqttTimers.current.forEach((expiry, id) => { if (expiry > now) mqttSources.add(id); });
      const paxValueSources = new Set<string>();
      paxValueTimers.current.forEach((expiry, id) => { if (expiry > now) paxValueSources.add(id); });
      const allSources   = new Set([...audioSources, ...midiSources, ...udpSources, ...oscSources, ...artNetSources, ...dmxSources, ...mqttSources, ...paxValueSources]);

      let css = '';

      allSources.forEach(id => {
        const audioRms    = audioLevels.current.get(id) ?? 0;
        const isMidiFlash = (midiTimers.current.get(id) ?? 0) > now;
        const nodeAudioEdges = audioEdges.filter(e => e.source === id);
        const nodeMidiEdges  = midiEdges.filter(e => e.source === id);
        const col  = rmsToColour(audioRms);
        const glow = rmsToGlow(audioRms, col);

        // Audio OUT port dots (per-port RMS for multi-port nodes)
        const nodeData  = nodesRef.current.find(n => n.id === id);
        const ports: any[] = nodeData?.data?.ports ?? [];
        const audioPorts = ports.filter(p => p.direction === 'output' && p.type === 'audio')
                               .map(p => p.label as string);
        const portRmsList = portRmsLevels.current.get(id);
        (audioPorts.length ? audioPorts : ['Audio Out']).forEach((label, i) => {
          const pRms  = portRmsList ? (portRmsList[i] ?? 0) : audioRms;
          if (pRms < 0.01) return;
          const pCol  = rmsToColour(pRms);
          const pGlow = rmsToGlow(pRms, pCol);
          css += `[data-handleid="${id}_${label}_out"]{background:${pCol}!important;box-shadow:${pGlow}!important}`;
        });

        // Audio edges + target IN dots
        nodeAudioEdges.forEach(e => {
          css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${col}!important;filter:drop-shadow(0 0 3px ${col})}`;
          if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${col}!important;box-shadow:${glow}!important}`;
        });

        // Pax value-port flash (nodeType >= 100, paxValueTimers bucket) —
        // pragmatic middle ground agreed for this follow-up: true per-port
        // activity distinction would need new backend counters (not built
        // here), so instead flash every one of a node's typed non-audio
        // output ports together whenever the node has any activity, each
        // in its own correct colour rather than one fixed colour for the
        // whole node. Reuses colourForHandleId — same label-text-based
        // classification as every other port/edge colour in this file, so
        // a port's own id (which already encodes its correct type-specific
        // label thanks to the per-port-typing mechanism) gives the right
        // colour with no separate lookup needed. Audio ports excluded —
        // already handled by the RMS block above. DMX-typed ports also
        // excluded now — a DMX channel holds a continuous value rather
        // than firing discrete events, so it gets its own gradual-intensity
        // rendering below instead of this on/off flash treatment.
        const isPaxValueFlash = (paxValueTimers.current.get(id) ?? 0) > now;
        if (isPaxValueFlash) {
          const valuePorts = ports.filter(p => p.direction === 'output' && p.type !== 'audio' &&
            !((p.id as string).toLowerCase().includes('dmx') && !(p.id as string).toLowerCase().includes('artdmx')));
          valuePorts.forEach(p => {
            const pCol = colourForHandleId(p.id as string);
            css += `[data-handleid="${p.id}"]{background:${pCol}!important;box-shadow:0 0 10px ${pCol}!important;transition:none}`;
            edgeList.current.filter(e => e.sourceHandle === p.id).forEach(e => {
              css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${pCol}!important;filter:drop-shadow(0 0 4px ${pCol});transition:none}`;
              if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${pCol}!important;box-shadow:0 0 10px ${pCol}!important;transition:none}`;
            });
          });
        }

        // MIDI flash — written last so it wins over audio VU
        if (isMidiFlash) {
          const fc = '#B2EBF2';
          css += `[data-handleid="${id}_MIDI Out_out"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          nodeMidiEdges.forEach(e => {
            css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${fc}!important;filter:drop-shadow(0 0 4px ${fc});transition:none}`;
            if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          });
        }

        // UDP flash
        const isUdpFlash = (udpTimers.current.get(id) ?? 0) > now;
        if (isUdpFlash) {
          const nodeUdpEdges = udpEdges.filter(e => e.source === id);
          const fc = '#93c5fd';
          css += `[data-handleid="${id}_UDP Out_out"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          nodeUdpEdges.forEach(e => {
            css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${fc}!important;filter:drop-shadow(0 0 4px ${fc});transition:none}`;
            if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          });
        }

        // MQTT flash — now has its own mqttEdges collection (previously
        // reused udpEdges since both shared the generic "Value Out" label;
        // fixed alongside the cross-protocol port-typing gap — see
        // Architecture.md's locked decisions).
        const isMqttFlash = (mqttTimers.current.get(id) ?? 0) > now;
        if (isMqttFlash) {
          const nodeMqttEdges = mqttEdges.filter(e => e.source === id);
          const fc = '#fb7185'; // --mqtt coral/salmon
          css += `[data-handleid="${id}_MQTT Out_out"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          nodeMqttEdges.forEach(e => {
            css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${fc}!important;filter:drop-shadow(0 0 4px ${fc});transition:none}`;
            if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          });
        }

        // OSC flash
        const isOscFlash = (oscTimers.current.get(id) ?? 0) > now;
        if (isOscFlash) {
          const nodeOscEdges = oscEdges.filter(e => e.source === id);
          const fc = '#67e8f9'; // lighter cyan flash, distinct from base --osc colour
          css += `[data-handleid="${id}_OSC Out_out"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          nodeOscEdges.forEach(e => {
            css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${fc}!important;filter:drop-shadow(0 0 4px ${fc});transition:none}`;
            if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          });
        }

        // ArtNet flash
        const isArtNetFlash = (artNetTimers.current.get(id) ?? 0) > now;
        if (isArtNetFlash) {
          const nodeArtNetEdges = artNetEdges.filter(e => e.source === id);
          const fc = '#fef08a'; // lighter pale yellow flash, distinct from base --artnet
          css += `[data-handleid="${id}_ArtDMX Out_out"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          nodeArtNetEdges.forEach(e => {
            css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${fc}!important;filter:drop-shadow(0 0 4px ${fc});transition:none}`;
            if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          });
        }

      });

      // ── DMX intensity — continuous, not a discrete on/off flash ──────────
      // A DMX channel holds a value (like a dimmer sitting at some
      // brightness) rather than firing discrete events the way MIDI/OSC/
      // UDP/MQTT do, so this reads every node's current channel level
      // (dmxValues, updated every poll straight from the backend's own
      // persisted "last real value" slot — see WebBridge.h's PortActivity)
      // and renders gradual intensity from it directly. Deliberately NOT
      // gated by allSources/dmxTimers — a channel parked at a steady value
      // stops generating change-events after ~80ms, which would otherwise
      // make a steady-but-nonzero channel fade back to idle colour and
      // misleadingly look "off". Base DMX colour (--dmx, #fbbf24 =
      // 251,191,36) stays fixed; only alpha/glow size vary with the value —
      // matching the "intensity, not colour" behaviour asked for, as
      // distinct from Audio's own hue-shifting VU meter above.
      const DMX_RGB = '251,191,36';
      nodesRef.current.forEach((nd: any) => {
        const id = nd.id;
        const nodeType = nd.data?.nodeType;
        const isBuiltInDmx = nodeType === 14 || nodeType === 15 || nodeType === 16 || nodeType === 17;
        const nodePorts: any[] = nd.data?.ports ?? [];
        const dmxPaxPorts = nodeType >= 100
          ? nodePorts.filter(p => p.direction === 'output' && p.type !== 'audio' &&
              (p.id as string).toLowerCase().includes('dmx') &&
              !(p.id as string).toLowerCase().includes('artdmx'))
          : [];
        if (! isBuiltInDmx && dmxPaxPorts.length === 0) return;

        const v     = Math.max(0, Math.min(1, dmxValues.current.get(id) ?? 0));
        // Floor raised from 0.15 to 0.4 — at v=0 the old floor was nearly
        // invisible against the dark canvas background; still clearly
        // dimmer than a live channel, just no longer "gone".
        const alpha = 0.4 + v * 0.6;
        const bg    = `rgba(${DMX_RGB},${alpha})`;
        const px    = Math.round(4 + v * 10);
        const glow  = `0 0 ${px}px rgba(${DMX_RGB},${Math.min(1, alpha + 0.1)})`;

        const targetSelectors: string[] = isBuiltInDmx
          ? [`[data-handleid="${id}_DMX Out_out"]`]
          : dmxPaxPorts.map(p => `[data-handleid="${p.id}"]`);
        targetSelectors.forEach(sel => {
          css += `${sel}{background:${bg}!important;box-shadow:${glow}!important;transition:background 80ms linear,box-shadow 80ms linear}`;
        });

        const nodeDmxEdges = edgeList.current.filter(e => {
          if (e.source !== id) return false;
          const h = (e.sourceHandle ?? '').toLowerCase();
          return h.includes('dmx') && !h.includes('artdmx');
        });
        nodeDmxEdges.forEach(e => {
          css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${bg}!important;filter:drop-shadow(0 0 ${Math.round(4 + v * 6)}px rgba(${DMX_RGB},${alpha}));transition:stroke 80ms linear}`;
          if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${bg}!important;box-shadow:${glow}!important;transition:background 80ms linear,box-shadow 80ms linear}`;
        });
      });

      style.textContent = css;
      rafId = requestAnimationFrame(render);
    };
    rafId = requestAnimationFrame(render);
    return () => cancelAnimationFrame(rafId);
  }, []);
}

// ── Inner component (needs useReactFlow hook) ─────────────────────────────────
// ── Burger menu row helpers ─────────────────────────────────────────────────
function MenuRow ({ label, shortcut, enabled = true, onClick, onMouseEnter, trailing }: {
  label: string;
  shortcut?: string;
  enabled?: boolean;
  onClick?: () => void;
  onMouseEnter?: () => void;
  trailing?: ReactNode;
}) {
  return (
    <div
      onClick={enabled ? onClick : undefined}
      onMouseEnter={e => { onMouseEnter?.(); if (enabled) e.currentTarget.style.background = 'var(--surface)'; }}
      onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
      style={{
        padding: '6px 14px', fontSize: 11,
        color: enabled ? 'var(--text)' : 'var(--text-muted)',
        cursor: enabled ? 'pointer' : 'default',
        fontFamily: "'JetBrains Mono', monospace",
        display: 'flex', justifyContent: 'space-between', alignItems: 'center',
        gap: 24, opacity: enabled ? 1 : 0.45,
      }}
    >
      <span>{label}</span>
      {trailing ?? (shortcut && <span style={{ color: 'var(--text-muted)', fontSize: 10 }}>{shortcut}</span>)}
    </div>
  );
}

function MenuDivider() {
  return <div style={{ borderTop: '1px solid var(--border)', margin: '4px 0' }} />;
}

// ── Main canvas ────────────────────────────────────────────────────────────
function FlowCanvas() {
  const { isStandalone } = useContext(DawContext);
  const [nodes, setNodes] = useState<Node<any>[]>([]);
  const [edges, setEdges] = useState<Edge[]>([]);
  const wrapperRef = useRef<HTMLDivElement>(null);
  const { setHint } = useContext(HintContext);
  const { screenToFlowPosition, setViewport, updateNode, getNodes, deleteElements } = useReactFlow();
  const pendingDrop     = useRef<{ dropX: number; dropY: number } | null>(null);
  const knownNodeIds    = useRef<Set<string>>(new Set());
  const burgerBtnRef    = useRef<HTMLButtonElement>(null);
  const prefsBtnRef     = useRef<HTMLButtonElement>(null);
  const updateNodeInternals = useUpdateNodeInternals();
  const [prefs, setPrefs]       = useState<GraphPreferences>(loadPrefs);
  const [showPrefs, setShowPrefs] = useState(false);
  const [allCollapsed, setAllCollapsed] = useState(false);
  const [fileState, setFileState] = useState<FileState>({ fileName: 'Untitled', hasFile: false });
  const [undoState, setUndoState] = useState<UndoState>({ canUndo: false, canRedo: false });
  const [audioSettings, setAudioSettings] = useState<AudioSettings | null>(null);
  const [showFileMenu, setShowFileMenu] = useState(false);
  const [burgerSubmenu, setBurgerSubmenu] = useState<'file' | 'edit' | null>(null);
  const [pendingFragment, setPendingFragment] = useState<FragmentData | null>(null);
  const ghostPos = useRef<{ x: number; y: number }>({ x: 0, y: 0 });
  const ghostRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    return Bridge.onFileState(setFileState);
  }, []);

  useEffect(() => {
    return Bridge.onUndoState(setUndoState);
  }, []);

  // Keep ReactFlow node data in sync when slider updates settingsJson
  useEffect(() => {
    return Bridge.onNodeSettings((nodeId, settingsJson) => {
      setNodes(prev => prev.map(n =>
        n.id === nodeId
          ? { ...n, data: { ...n.data, settingsJson } }
          : n
      ));
    });
  }, [setNodes]);

  useEffect(() => {
    return Bridge.onAudioSettings(setAudioSettings);
  }, []);

  // Receive fragment from C++ after import file pick — enter ghost mode
  useEffect(() => {
    return Bridge.onFragmentReady((fragment) => {
      setPendingFragment(fragment);
    });
  }, []);

  useEffect(() => {
    if (!showFileMenu) { setBurgerSubmenu(null); return; }
    const close = (e: MouseEvent) => {
      if (burgerBtnRef.current?.contains(e.target as Element)) return;
      setShowFileMenu(false);
      setBurgerSubmenu(null);
    };
    document.addEventListener('mousedown', close);
    return () => document.removeEventListener('mousedown', close);
  }, [showFileMenu]);

  const toggleAllCollapsed = useCallback(() => {
    setAllCollapsed(prev => {
      const next = !prev;
      setNodes(ns => ns.map(n => ({ ...n, data: { ...n.data, _forceCollapsed: next } })));
      return next;
    });
  }, [setNodes]);

  // Port dot and edge hover hints via event delegation
  useEffect(() => {
    const handleMouseOver = (e: MouseEvent) => {
      const target = e.target as HTMLElement;

      // Port dot
      const handle = target.closest('.react-flow__handle') as HTMLElement | null;
      if (handle) {
        const hid = handle.getAttribute('data-handleid') ?? '';
        const isMidi  = hid.toLowerCase().includes('midi');
        const isAudio = hid.toLowerCase().includes('audio');
        const isIn    = hid.endsWith('_in');
        if (isMidi)  setHint(isIn ? PORT_HINTS['midi-in']  : PORT_HINTS['midi-out']);
        if (isAudio) setHint(isIn ? PORT_HINTS['audio-in'] : PORT_HINTS['audio-out']);
        return;
      }

      // Edge
      const edge = target.closest('.react-flow__edge') as HTMLElement | null;
      if (edge) {
        const cls = edge.className ?? '';
        if (cls.includes('edge-midi'))   setHint(EDGE_HINTS['midi']);
        else if (cls.includes('edge-audio')) setHint(EDGE_HINTS['audio']);
        else if (cls.includes('edge-mixed')) setHint(EDGE_HINTS['av']);
        else if (cls.includes('edge-osc'))   setHint(EDGE_HINTS['osc']   ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-dmx'))   setHint(EDGE_HINTS['dmx']   ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-mqtt'))  setHint(EDGE_HINTS['mqtt']  ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-udp'))   setHint(EDGE_HINTS['udp']   ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-value')) setHint(EDGE_HINTS['value'] ?? EDGE_HINTS['midi']);
        return;
      }
    };
    const handleMouseOut = (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      if (target.closest('.react-flow__handle') || target.closest('.react-flow__edge'))
        setHint(null);
    };
    document.addEventListener('mouseover', handleMouseOver);
    document.addEventListener('mouseout',  handleMouseOut);
    return () => {
      document.removeEventListener('mouseover', handleMouseOver);
      document.removeEventListener('mouseout',  handleMouseOut);
    };
  }, [setHint]);

  // Keyboard shortcuts: Cmd+Z = undo, Cmd+Shift+Z = redo
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      const cmd = e.metaKey;  // Mac only — Cmd+Z, not Ctrl+Z
      if (!cmd) return;
      if (e.key === 'z' || e.key === 'Z') {
        e.preventDefault();
        if (e.shiftKey) Bridge.redo();
        else            Bridge.undo();
      }
      // Also handle synthetic key events forwarded from C++ via onKeyEvent
      if (e.key === 'undo') { e.preventDefault(); Bridge.undo(); }
      if (e.key === 'redo') { e.preventDefault(); Bridge.redo(); }
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, []);

  // Keyboard shortcut: F to fold/unfold all
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'f' && !e.metaKey && !e.ctrlKey &&
          !(document.activeElement instanceof HTMLInputElement) &&
          !(document.activeElement instanceof HTMLTextAreaElement))
        toggleAllCollapsed();
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [toggleAllCollapsed]);
  usePortActivityStyles(edges, nodes);


  // ── Sync from JUCE model ─────────────────────────────────────────────────
  useEffect(() => {
    const unsubGraph = Bridge.onGraphUpdate((state: GraphState) => {
      flushSync(() => {
        setNodes(prev => {
          const styleMap = new Map(prev.map(n => [n.id, n.style]));
          return state.nodes.map(raw => {
            const node = rawToFlowNode(raw, _paxInfoMap);
            const existing = styleMap.get(raw.id);
            if (existing) node.style = { ...node.style, ...existing };
            return node;
          });
        });
        setEdges(state.connections.map(rawToFlowEdge));
      });

      // Update node internals so edge endpoints snap to port dots after load
      setTimeout(() => {
        state.nodes.forEach(n => updateNodeInternals(n.id));
      }, 50);

      // Restore saved viewport if present
      if (state.viewportZoom && state.viewportZoom > 0) {
        setViewport({
          x:    state.viewportX    ?? 0,
          y:    state.viewportY    ?? 0,
          zoom: state.viewportZoom ?? 1,
        });
      }

      // Claimed devices are rebuilt in Bridge.ts onGraphUpdate — no echo needed.
    });


    Bridge.onPaxList((paxItems) => {
      paxItems.forEach(a => _paxInfoMap.set(a.name, a));
    });
    Bridge.ready();
    return () => unsubGraph();
  }, []);

  // ── Node / edge change handlers ──────────────────────────────────────────
  const onNodesChange = useCallback((changes: NodeChange[]) => {
    setNodes(ns => applyNodeChanges(changes, ns) as Node<any>[]);
    for (const c of changes) {
      if (c.type === 'position' && c.position)
        Bridge.moveNode(c.id, c.position.x, c.position.y);
      // dimensions fires when ReactFlow measures a node — only centre if it's new
      if (c.type === 'dimensions' && c.dimensions?.width && pendingDrop.current
          && !(knownNodeIds.current.has((c as any).id))) {
        const { dropX, dropY } = pendingDrop.current;
        const id = (c as any).id;
        const w = c.dimensions.width;
        const centredX = dropX - w / 2;
        updateNode(id, { position: { x: centredX, y: dropY } });
        Bridge.moveNode(id, centredX, dropY);
        knownNodeIds.current.add(id);
        pendingDrop.current = null;
      } else if (c.type === 'dimensions') {
        // Track all existing nodes
        knownNodeIds.current.add((c as any).id);
      }
    }
  }, [updateNode]);

  const onEdgesChange = useCallback((changes: EdgeChange[]) => {
    setEdges(es => applyEdgeChanges(changes, es));
    for (const c of changes) {
      if (c.type === 'remove')
        Bridge.removeConnection(c.id);
    }
  }, []);


  // onReconnectEnd fires when the drag ends WITHOUT landing on a valid port —
  // i.e. the user dropped the edge onto empty canvas to disconnect it.
  const onReconnectEnd = useCallback(
    (_event: MouseEvent | TouchEvent, edge: Edge, handleType: string) => {
      // handleType is 'source' or 'target' — either way the edge is broken
      // Remove it from both the local state and the C++ model
      Bridge.removeConnection(edge.id);
      setEdges(es => es.filter(e => e.id !== edge.id));
    },
    [],
  );

  // ── Drag-in-progress connection line colour ──────────────────────────────
  // The line shown while dragging a NEW connection (before it's dropped)
  // defaults to a fixed accent colour via connectionLineStyle. This makes
  // it reflect the source port's own protocol colour instead, matching the
  // node being dragged from — same colour-by-type classification used for
  // finished edges (see rawToFlowEdge), kept as its own small function here
  // rather than refactored into a shared one, to keep this addition isolated.
  const [connectionLineColour, setConnectionLineColour] = useState('var(--accent)');

  const onConnectStart = useCallback((_event: MouseEvent | TouchEvent, params: { nodeId: string | null; handleId: string | null; handleType: string | null }) => {
    if (params.handleId) setConnectionLineColour(colourForHandleId(params.handleId));
  }, []);

  const onConnectEnd = useCallback(() => {
    setConnectionLineColour('var(--accent)');
  }, []);

  // ── Connection validation ─────────────────────────────────────────────────
  const isValidConnection = useCallback((connection: Connection | Edge): boolean => {
    const { sourceHandle, targetHandle } = connection;
    if (!sourceHandle || !targetHandle) return false;

    // Extract port type from handle ID — format: {nodeId}_{Label}_{in|out}
    // Label examples: "Audio In", "OSC Out", "UDP Out", "MQTT In", "ArtDMX In", "MIDI Out"
    const getPortType = (handle: string): string => {
      const h = handle.toLowerCase();
      if (h.includes('audio'))   return 'audio';
      if (h.includes('artdmx'))  return 'artnet';  // must come before 'dmx'
      if (h.includes('osc'))     return 'osc';
      if (h.includes('dmx'))     return 'dmx';
      if (h.includes('mqtt'))    return 'mqtt';
      if (h.includes('udp'))     return 'udp';
      if (h.includes('value'))   return 'value';  // generic Pax adapter/converter ports (Phase 4)
      return 'midi';
    };

    const srcType  = getPortType(sourceHandle);
    const dstType  = getPortType(targetHandle);
    const srcIsOut = sourceHandle.endsWith('_out');
    const dstIsIn  = targetHandle.endsWith('_in');

    // Strict: types must match exactly, direction must be out→in
    return srcType === dstType && srcIsOut && dstIsIn;
  }, []);
  // ── Reconnect (drag existing edge endpoint to a new port) ─────────────────
  // Zoom inversion deferred — ReactFlow native gesture handling used for now.

  // Bring selected nodes to front — always wins over any other z-index
  const onSelectionChange = useCallback(({ nodes: selected }: { nodes: any[] }) => {
    const selectedIds = new Set(selected.map((n: any) => n.id));
    getNodes().forEach(n => {
      const z = n.style?.zIndex ?? 0;
      const hasSettings = z === 9999 || z === 10000;
      if (selectedIds.has(n.id)) {
        // Selected: absolute top — 10000 beats any settings panel on other nodes
        updateNode(n.id, { style: { zIndex: 10000 } });
      } else if (hasSettings) {
        // Not selected but has settings open: drop to 9999 (settings visible but behind selected)
        updateNode(n.id, { style: { zIndex: 9999 } });
      } else {
        updateNode(n.id, { style: { zIndex: 0 } });
      }
    });
  }, [getNodes, updateNode]);

  // ReactFlow v12: onReconnect fires when the drag ends on a valid new target.
  // We remove the old connection and add the new one.
  const onReconnect = useCallback(
    (oldEdge: Edge, newConnection: Connection) => {
      // Remove old connection from C++ model
      Bridge.removeConnection(oldEdge.id);
      // Add the new connection if valid
      if (isValidConnection(newConnection)) {
        Bridge.addConnection(
          newConnection.source,
          newConnection.sourceHandle!,
          newConnection.target,
          newConnection.targetHandle!,
        );
      }
      setEdges(es => reconnectEdge(oldEdge, newConnection, es));
    },
    [isValidConnection],
  );


  const onConnect = useCallback((connection: Connection) => {
    if (!isValidConnection(connection)) return;
    Bridge.addConnection(
      connection.source,
      connection.sourceHandle!,
      connection.target,
      connection.targetHandle!,
    );
    const src = connection.sourceHandle?.toLowerCase() ?? '';
    const edgeCls = src.includes('audio')  ? 'edge-audio'
                  : src.includes('artdmx') ? 'edge-artnet'  // must come before 'dmx'
                  : src.includes('osc')    ? 'edge-osc'
                  : src.includes('dmx')    ? 'edge-dmx'
                  : src.includes('mqtt')   ? 'edge-mqtt'
                  : src.includes('udp')    ? 'edge-udp'
                  : src.includes('value')  ? 'edge-value'
                  : 'edge-midi';
    setEdges(es => addEdge({
      ...connection,
      className: edgeCls,
      style: { strokeWidth: 2 },
    }, es));
  }, [isValidConnection]);

  // ── Drag-and-drop ─────────────────────────────────────────────────────────
  //
  // IMPORTANT: both onDragOver and onDrop must be on the wrapper div AND
  // forwarded to <ReactFlow> so the whole canvas area accepts drops.
  // screenToFlowPosition(clientX, clientY) handles pan/zoom automatically.

  const onDragOver = useCallback((e: DragEvent<HTMLDivElement>) => {
    e.preventDefault();
    e.stopPropagation();
    e.dataTransfer.dropEffect = 'copy';
    return false;
  }, []);

  const onDrop = useCallback((e: DragEvent<HTMLDivElement>) => {
    e.preventDefault();
    e.stopPropagation();

    const raw = e.dataTransfer.getData('text/plain');
    if (!raw) return;

    // Data is JSON { nodeType, paxName, ngaType? }
    // For Pax: nodeType=0 (sentinel), ngaType=1/2/3/4 (PAX MIDI/Audio/AV/Value)
    // For built-ins: nodeType=1-4, paxName=''
    let nodeType: number = 1;
    let paxName = '';
    let ngaType: number = 0;
    try {
      const parsed = JSON.parse(raw);
      nodeType   = parsed.nodeType   as number;
      paxName = parsed.paxName ?? '';
      ngaType    = parsed.ngaType    ?? 0;
    } catch {
      nodeType = parseInt(raw, 10);
    }

    // Send ngaType as nodeType to C++ for addons so ports are correct,
    // but offset by 100 to guarantee no collision with built-in types 1-4.
    // C++ checks paxName first, so the actual value only matters for port layout.
    const cppNodeType = paxName ? (100 + ngaType) : nodeType;

    const position = screenToFlowPosition({ x: e.clientX, y: e.clientY });
    pendingDrop.current = { dropX: position.x, dropY: position.y };
    Bridge.addNode(cppNodeType, position.x, position.y, paxName);
  }, [screenToFlowPosition]);

  // ── Ghost overlay: track mouse while fragment pending ──────────────────
  useEffect(() => {
    if (!pendingFragment) return;

    const onMouseMove = (e: MouseEvent) => {
      ghostPos.current = { x: e.clientX, y: e.clientY };
      if (ghostRef.current) {
        ghostRef.current.style.left = e.clientX + 'px';
        ghostRef.current.style.top  = e.clientY + 'px';
      }
    };
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setPendingFragment(null);
    };
    const onContextMenu = (e: MouseEvent) => {
      e.preventDefault();
      setPendingFragment(null);
    };

    window.addEventListener('mousemove',   onMouseMove);
    window.addEventListener('keydown',     onKeyDown);
    window.addEventListener('contextmenu', onContextMenu);
    return () => {
      window.removeEventListener('mousemove',   onMouseMove);
      window.removeEventListener('keydown',     onKeyDown);
      window.removeEventListener('contextmenu', onContextMenu);
    };
  }, [pendingFragment]);

  // Drop the fragment at the current cursor position
  const dropFragment = useCallback(() => {
    if (!pendingFragment || !wrapperRef.current) return;
    const flowPos = screenToFlowPosition({ x: ghostPos.current.x, y: ghostPos.current.y });

    flushSync(() => {
      setNodes(prev => [
        ...prev,
        ...pendingFragment.nodes.map(raw => rawToFlowNode(
          { ...raw, x: flowPos.x + raw.x, y: flowPos.y + raw.y },
          _paxInfoMap
        )),
      ]);
      setEdges(prev => [
        ...prev,
        ...pendingFragment.connections.map(rawToFlowEdge),
      ]);
    });

    // Tell C++ to add the nodes + connections to the audio graph
    Bridge.importFragmentNodes(
      pendingFragment.nodes.map(n => ({ ...n, x: flowPos.x + n.x, y: flowPos.y + n.y })),
      pendingFragment.connections
    );

    setPendingFragment(null);
  }, [pendingFragment, screenToFlowPosition, setNodes, setEdges]);

  // ── Render ────────────────────────────────────────────────────────────────
  return (
    <div
      ref={wrapperRef}
      style={{ flex: 1, height: '100%', position: 'relative',
               cursor: pendingFragment ? 'crosshair' : undefined }}
      onDragOver={onDragOver}
      onDrop={onDrop}
      onClick={pendingFragment ? dropFragment : undefined}
    >
      <ReactFlow
        onPaneClick={() => { setShowFileMenu(false); setShowPrefs(false); }}
        onNodeClick={() => { setShowFileMenu(false); setShowPrefs(false); }}
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        onConnectStart={onConnectStart}
        onConnectEnd={onConnectEnd}
        onReconnect={onReconnect}
        onReconnectEnd={onReconnectEnd}
        isValidConnection={isValidConnection}
        defaultViewport={{ x: 0, y: 0, zoom: 1 }}
        minZoom={0.05}
        maxZoom={2}
        deleteKeyCode="Delete"
        style={{ background: 'var(--bg)' }}
        connectionLineStyle={{ stroke: connectionLineColour, strokeWidth: 2, strokeDasharray: '6 3' }}
        defaultEdgeOptions={{ style: { strokeWidth: 2 } }}
        onDragOver={onDragOver}
        onDrop={onDrop}
        onMoveEnd={(_, vp) => Bridge.setViewport(vp.x, vp.y, vp.zoom)}
        onSelectionChange={onSelectionChange}

      >
        <Background variant={BackgroundVariant.Dots} gap={24} size={1.2} color="var(--border)" />
        <Controls style={{ bottom: 16, right: 16, left: 'auto' }} />

        {/* File menu + Fold/Unfold + Preferences buttons */}
        <div style={{ position: 'absolute', top: 12, right: 12, zIndex: 10, display: 'flex', gap: 6 }}>

          {/* ☰ Hamburger file menu */}
          <div style={{ position: 'relative' }}>
            <button
              ref={burgerBtnRef}
              onMouseDown={e => { e.stopPropagation(); setShowPrefs(false); setShowFileMenu(v => !v); }}
              onClick={e => e.stopPropagation()}
              onMouseEnter={() => setHint({ title: 'Menu', body: 'File and Edit operations.' })}
              onMouseLeave={() => setHint(null)}
              style={{
                background:   showFileMenu ? 'var(--surface2)' : 'transparent',
                border:       '1px solid var(--border)',
                borderRadius: 'var(--radius)',
                color:        showFileMenu ? 'var(--text)' : 'var(--text-dim)',
                cursor:       'pointer',
                fontSize:     19,
                width:        26, height: 26,
                display:      'flex', alignItems: 'center', justifyContent: 'center',
                transition:   'background 0.15s, color 0.15s',
              }}><Menu size={19} /></button>
            {showFileMenu && (
              <div
                onMouseDown={e => e.stopPropagation()}
                style={{
                  position: 'absolute', top: 34, right: 0,
                  background: 'var(--surface2)', border: '1px solid var(--border)',
                  borderRadius: 'var(--radius)', padding: '4px 0',
                  boxShadow: '0 8px 32px rgba(0,0,0,.6)',
                  minWidth: 120, zIndex: 100,
                }}>
                {/* ── Top-level: File / Edit ── */}
                <MenuRow
                  label="File"
                  enabled
                  onMouseEnter={() => setBurgerSubmenu('file')}
                  trailing={<ChevronLeft size={12} style={{ color: 'var(--text-muted)' }} />}
                />
                <MenuRow
                  label="Edit"
                  enabled
                  onMouseEnter={() => setBurgerSubmenu('edit')}
                  trailing={<ChevronLeft size={12} style={{ color: 'var(--text-muted)' }} />}
                />
                {fileState.hasFile && (
                  <div style={{ padding: '4px 14px 2px', fontSize: 9, color: 'var(--text-muted)',
                                fontFamily: "'JetBrains Mono', monospace",
                                borderTop: '1px solid var(--border)', marginTop: 2 }}>
                    {fileState.fileName}.patchy
                  </div>
                )}

                {/* ── File submenu (opens to the left) ── */}
                {burgerSubmenu === 'file' && (
                  <div
                    onMouseDown={e => e.stopPropagation()}
                    style={{
                      position: 'absolute', top: 0, right: '100%', marginRight: 4,
                      background: 'var(--surface2)', border: '1px solid var(--border)',
                      borderRadius: 'var(--radius)', padding: '4px 0',
                      boxShadow: '0 8px 32px rgba(0,0,0,.6)',
                      minWidth: 170, zIndex: 100,
                    }}>
                    <MenuRow label="New"      shortcut="⌘N"  onClick={() => { Bridge.fileNew();    setShowFileMenu(false); }} />
                    <MenuRow label="Open…"    shortcut="⌘O"  onClick={() => { Bridge.fileOpen();   setShowFileMenu(false); }} />
                    <MenuRow label={fileState.hasFile ? 'Save' : 'Save…'} shortcut="⌘S" onClick={() => { Bridge.fileSave(); setShowFileMenu(false); }} />
                    <MenuRow label="Save As…" shortcut="⌘⇧S" onClick={() => { Bridge.fileSaveAs(); setShowFileMenu(false); }} />
                    <MenuDivider />
                    {(() => {
                      const selectedNodes = getNodes().filter(n => n.selected);
                      const hasSelection  = selectedNodes.length > 0;
                      const handleExport  = () => {
                        if (!hasSelection) return;
                        const ids = selectedNodes.map(n => n.id);
                        const NODE_LABELS: Record<number, string> = {
                          1: 'MidiIn', 2: 'MidiOut', 3: 'AudioIn', 4: 'AudioOut',
                          5: 'MidiMonitor', 6: 'AudioMonitor', 7: 'Keyboard',
                        };
                        const names  = selectedNodes.map(n => {
                          const d = n.data as { nodeType?: number; paxName?: string; label?: string };
                          return d.paxName || NODE_LABELS[d.nodeType ?? 0] || d.label || 'Node';
                        });
                        const unique    = [...new Set(names)];
                        const suggested = unique.slice(0, 3).join('_') + (unique.length > 3 ? '_etc' : '');
                        Bridge.exportSelection(ids, suggested);
                        setShowFileMenu(false);
                      };
                      return (
                        <>
                          <MenuRow
                            label="Export…"
                            enabled={hasSelection}
                            onClick={handleExport}
                            trailing={
                              <span style={{ color: 'var(--text-muted)', fontSize: 10 }}>
                                {hasSelection ? `${selectedNodes.length} node${selectedNodes.length !== 1 ? 's' : ''}` : 'select nodes'}
                              </span>
                            }
                          />
                          <MenuRow label="Import…" onClick={() => { Bridge.importFragment(); setShowFileMenu(false); }} />
                        </>
                      );
                    })()}
                  </div>
                )}

                {/* ── Edit submenu (opens to the left) ── */}
                {burgerSubmenu === 'edit' && (
                  <div
                    onMouseDown={e => e.stopPropagation()}
                    style={{
                      position: 'absolute', top: 26, right: '100%', marginRight: 4,
                      background: 'var(--surface2)', border: '1px solid var(--border)',
                      borderRadius: 'var(--radius)', padding: '4px 0',
                      boxShadow: '0 8px 32px rgba(0,0,0,.6)',
                      minWidth: 150, zIndex: 100,
                    }}>
                    <MenuRow label="Undo" shortcut="⌘Z"   enabled={undoState.canUndo} onClick={() => { Bridge.undo(); setShowFileMenu(false); }} />
                    <MenuRow label="Redo" shortcut="⌘⇧Z" enabled={undoState.canRedo} onClick={() => { Bridge.redo(); setShowFileMenu(false); }} />
                    <MenuDivider />
                    <MenuRow label="Cut"    shortcut="⌘X" enabled={false} />
                    <MenuRow label="Copy"   shortcut="⌘C" enabled={false} />
                    <MenuRow label="Paste"  shortcut="⌘V" enabled={false} />
                    <MenuDivider />
                    {(() => {
                      const selectedNodes = getNodes().filter(n => n.selected);
                      const hasSelection  = selectedNodes.length > 0;
                      return (
                        <MenuRow
                          label="Delete"
                          shortcut="⌫"
                          enabled={hasSelection}
                          onClick={() => {
                            selectedNodes.forEach(n => Bridge.removeNode(n.id));
                            deleteElements({ nodes: selectedNodes.map(n => ({ id: n.id })) });
                            setShowFileMenu(false);
                          }}
                        />
                      );
                    })()}
                  </div>
                )}
              </div>
            )}
          </div>
          <button
            onClick={toggleAllCollapsed}
           
            onMouseEnter={() => setHint(BUTTON_HINTS.foldAll)}
            onMouseLeave={() => setHint(null)}
            style={{
              background:   'transparent',
              border:       '1px solid var(--border)',
              borderRadius: 'var(--radius)',
              color:        allCollapsed ? 'var(--accent)' : 'var(--text-dim)',
              cursor:       'pointer',
              fontSize:     19,
              width:        26, height: 26,
              display:      'flex', alignItems: 'center', justifyContent: 'center',
              transition:   'background 0.15s, color 0.15s',
            }}
          >
            {allCollapsed ? <ChevronsUpDown size={19} /> : <ChevronsDownUp size={19} />}
          </button>
        <div style={{ position: 'relative' }}>
          <div style={{ position: 'relative' }}>
            <button
              ref={prefsBtnRef}
              onMouseDown={e => { e.stopPropagation(); setShowFileMenu(false); setShowPrefs(v => !v); }}
              onClick={e => e.stopPropagation()}
             
              onMouseEnter={() => setHint(BUTTON_HINTS.preferences)}
              onMouseLeave={() => setHint(null)}
              style={{
                background:   showPrefs ? 'var(--surface2)' : 'transparent',
                border:       '1px solid var(--border)',
                borderRadius: 'var(--radius)',
                color:        showPrefs ? 'var(--accent)' : 'var(--text-dim)',
                cursor:       'pointer',
                fontSize:     19,
                width:        26, height: 26,
                display:      'flex', alignItems: 'center', justifyContent: 'center',
                transition:   'background 0.15s, color 0.15s',
              }}
            >
              <Settings size={19} />
            </button>
            {showPrefs && (
              <PreferencesPanel
                prefs={prefs}
                onChange={setPrefs}
                isStandalone={isStandalone}
                audioSettings={audioSettings}
                onClose={() => setShowPrefs(false)}
                excludeRef={prefsBtnRef}
              />
            )}
          </div>
        </div>
        </div>

        {nodes.length === 0 && (
          <div style={{
            position: 'absolute', top: '50%', left: '50%',
            transform: 'translate(-50%,-50%)',
            textAlign: 'center', pointerEvents: 'none',
            color: 'var(--border-hi)', fontFamily: "'Syne', sans-serif",
          }}>
            <div style={{ fontSize: 48, marginBottom: 12, opacity: 0.4 }}>◎</div>
            <div style={{ fontSize: 15, letterSpacing: '0.12em' }}>DRAG NODES FROM THE PANEL</div>
            <div style={{ fontSize: 11, marginTop: 8, color: 'var(--text-muted)', letterSpacing: '0.06em' }}>
              then connect ports of the same type
            </div>
          </div>
        )}
      </ReactFlow>
      {/* ── Fragment ghost overlay ── */}
      {pendingFragment && (
        <div
          ref={ghostRef}
          style={{
            position:       'fixed',
            pointerEvents:  'none',
            transform:      'translate(-50%, -50%)',
            left: ghostPos.current.x,
            top:  ghostPos.current.y,
            width:  Math.max(pendingFragment.width  * 0.5, 120),
            height: Math.max(pendingFragment.height * 0.5, 50),
            border:         '2px dashed var(--accent)',
            borderRadius:   'var(--radius)',
            background:     'rgba(120,80,255, 0.08)',
            display:        'flex',
            flexDirection:  'column',
            alignItems:     'center',
            justifyContent: 'center',
            gap:            4,
            zIndex:         9999,
            backdropFilter: 'blur(2px)',
          }}
        >
          <span style={{ fontSize: 11, color: 'var(--accent)',
                         fontFamily: "'JetBrains Mono', monospace",
                         fontWeight: 700 }}>
            {pendingFragment.nodes.length} node{pendingFragment.nodes.length !== 1 ? 's' : ''}
          </span>
          <span style={{ fontSize: 9, color: 'var(--text-muted)',
                         fontFamily: "'JetBrains Mono', monospace" }}>
            click to place · esc to cancel
          </span>
        </div>
      )}
    </div>
  );
}

// ── Root export ───────────────────────────────────────────────────────────────
export default function App() {
  const [isStandalone, setIsStandalone] = useState(false);
  const [dawLoopbackEnabled, setDawLoopback] = useState(false);
  const [dawHostEnabled, setDawHost]         = useState(false);
  const dawInRef  = useRef<HTMLDivElement>(null);
  const dawOutRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    return Bridge.onStandaloneMode(setIsStandalone);
  }, []);

  return (
    <DawContext.Provider value={{ isStandalone, dawLoopbackEnabled, setDawLoopback, dawHostEnabled, setDawHost }}>
      <HintProvider>
        <div style={{ display: 'flex', width: '100%', height: '100%' }}>
          <ReactFlowProvider>
      <Sidebar />
            <FlowCanvas />
          </ReactFlowProvider>
        </div>
      </HintProvider>
    </DawContext.Provider>
  );
}
