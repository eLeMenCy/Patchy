import { useCallback, useEffect, useMemo, useRef, useState, useContext, DragEvent, ReactNode } from 'react';
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

import { Bridge, RecentFile, FileMissing, FileState, AudioSettings, GraphState, RawNode, RawConnection, RawPort, PortActivityEntry, PaxParamInfo, PaxInfo, FragmentData, UndoState, AudioPlayerStatus, RawWrap, PanicState } from './Bridge';
import { WrapNode, WrapFrame, WrapRail, WrapRouteEdge, WrapPort, WrapPax, WrapActions, WrapBuiltin, setMaxWrapFaders, FRAME_PAD, FRAME_HEAD, FRAME_PORT_TOP, FRAME_PORT_GAP } from './WrapNode';
import GenericNode, { NodeData } from './GenericNode';
import MidiMonitorNode,      { MidiMonitorNodeData }      from './MidiMonitorNode';
import AudioMonitorNode,   { AudioMonitorNodeData }   from './AudioMonitorNode';
import AudioPlayerNode,    { AudioPlayerNodeData }    from './AudioPlayerNode';
import Toasts from './Toasts';   // v0.0.958
import MidiFilePlayerNode, { MidiFilePlayerNodeData } from './MidiFilePlayerNode';   // v0.0.954
import { setKbdFaderPrefs, fadersOfSettings, kbdChannel, sendKbdFader, maxValue } from './KbdFaders';   // v0.0.956
import MidiChMatrixNode,   { MidiChMatrixNodeData }   from './MidiChMatrixNode';
import MidiMorpherNode,    { MidiMorpherNodeData }    from './MidiMorpherNode';
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
import ChannelFilterPaxNode from './ChannelFilterPaxNode';
import EnvelopeNode     from './EnvelopeNode';
import AudioToDmxNode from './AudioToDmxNode';
import AudioPeakToOscNode from './AudioPeakToOscNode';
import GhostNode from './GhostNode';   // v0.0.941
import { _paxInfoMap, detectPaxTheme } from './NodeUtils';

// ── Node type registry ────────────────────────────────────────────────────────
const edgeTypes = { wrapRoute: WrapRouteEdge };   // v0.0.934 — a wire drawn through open-wrap frame ports
const nodeTypes = { custom: GenericNode, midiMonitor: MidiMonitorNode, audioMonitor: AudioMonitorNode, audioPlayer: AudioPlayerNode, midiFilePlayer: MidiFilePlayerNode, midiChMatrix: MidiChMatrixNode, midiMorpher: MidiMorpherNode, midiKeyboard: MidiKeyboardNode, spectrumyser: SpectrumyserNode, channelFilterPax: ChannelFilterPaxNode, envelope: EnvelopeNode, audioToDmx: AudioToDmxNode, audioPeakToOsc: AudioPeakToOscNode, dmxMonitor: DmxMonitorNode, dmxConsole: DmxConsoleNode, artNetMonitor: ArtNetMonitorNode, artNetConsole: ArtNetConsoleNode, oscMonitor: OscMonitorNode, udpMonitor: UdpMonitorNode, mqttMonitor: MqttMonitorNode, mqttConsole: MqttConsoleNode, wrap: WrapNode, wrapFrame: WrapFrame, wrapRail: WrapRail, ghost: GhostNode };

// ── Conversion helpers ────────────────────────────────────────────────────────
// _paxInfoMap lives in NodeUtils.tsx (not declared here) — GenericNode.tsx
// also needs to read it (for colourCategory), and App.tsx already imports
// GenericNode to register it as a node type, so declaring it here would
// create a circular import. NodeUtils.tsx is a lower-level shared utility
// file both already depend on safely.

// v0.0.941 — set once the Pax list has arrived (it's pushed before the graph),
// so a Pax node is only shown as a ghost when its Pax is really not installed.
let _paxListLoaded = false;
let _paxScanModeSent = false;   // v0.0.958

function rawToFlowNode(raw: RawNode, paxInfoMap?: Map<string, PaxInfo>): Node<NodeData | MidiMonitorNodeData | DmxMonitorNodeData | OscMonitorNodeData | UdpMonitorNodeData | MqttMonitorNodeData | MqttConsoleNodeData | MidiChMatrixNodeData | MidiMorpherNodeData> {
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
  const isAudioPlayer    = raw.nodeType === 26;
  const isMidiChMatrix   = raw.nodeType === 27;
  const isMidiMorpher    = raw.nodeType === 28;
  const isMidiFilePlayer = raw.nodeType === 29;   // v0.0.954
  const isGhost          = !!raw.paxName && _paxListLoaded && !(paxInfoMap?.has(raw.paxName) ?? true);
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
            : isAudioPlayer    ? 'audioPlayer'
            : isMidiChMatrix   ? 'midiChMatrix'
            : isMidiMorpher    ? 'midiMorpher'
            : isMidiFilePlayer ? 'midiFilePlayer'
            : isGhost          ? 'ghost'      // v0.0.941 — Pax not installed
            : raw.paxName === 'Spectrumyser' ? 'spectrumyser'
            : raw.paxName === 'Channel Filter' ? 'channelFilterPax'
            : raw.paxName === 'Envelope'     ? 'envelope'
            : raw.paxName === 'Audio to DMX' ? 'audioToDmx'
            : raw.paxName === 'Audio Peak to OSC' ? 'audioPeakToOsc' : 'custom',
    position: { x: raw.x, y: raw.y },
    data: isMidiMonitor
      ? { label: raw.label, nodeType: 5,  ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as MidiMonitorNodeData
      : isAudioMonitor
      ? { label: raw.label, nodeType: 6,  ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as AudioMonitorNodeData
      : isMidiKeyboard
      ? { label: raw.label, nodeType: 7,  ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as MidiKeyboardNodeData
      : isDmxMonitor
      ? { label: raw.label, nodeType: 16, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isDmxConsole
      ? { label: raw.label, nodeType: 17, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isArtNetMonitor
      ? { label: raw.label, nodeType: 18, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isArtNetConsole
      ? { label: raw.label, nodeType: 19, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as DmxMonitorNodeData
      : isOscMonitor
      ? { label: raw.label, nodeType: 20, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as OscMonitorNodeData
      : isUdpMonitor
      ? { label: raw.label, nodeType: 21, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as UdpMonitorNodeData
      : isMqttMonitor
      ? { label: raw.label, nodeType: 24, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as MqttMonitorNodeData
      : isMqttConsole
      ? { label: raw.label, nodeType: 25, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as MqttConsoleNodeData
      : isAudioPlayer
      ? { label: raw.label, nodeType: 26, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as AudioPlayerNodeData
      : isMidiChMatrix
      ? { label: raw.label, nodeType: 27, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson, customName: raw.customName } as MidiChMatrixNodeData
      : isMidiFilePlayer
      ? { label: raw.label, nodeType: 29, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson } as MidiFilePlayerNodeData
      : isMidiMorpher
      ? { label: raw.label, nodeType: 28, ports: raw.ports, disabled: raw.disabled, settingsJson: raw.settingsJson, customName: raw.customName } as MidiMorpherNodeData
      : { label: raw.label, nodeType: raw.nodeType,
          ports: raw.ports, selectedDeviceId: raw.selectedDeviceId,
          paxName: raw.paxName,
          paxParams: raw.paxName ? (paxInfoMap?.get(raw.paxName)?.params ?? []) : [],
          disabled: raw.disabled, settingsJson: raw.settingsJson,
          customName: raw.customName } as NodeData,   // Phase 6 rename, 2026-09-25
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

// ── Wraps (v0.0.930) ────────────────────────────────────────────────────────
// A wrap is UI / project state only (see WrapNode.tsx). These helpers turn
// the real nodes / edges into what ReactFlow draws. The ids of the drawn-only
// nodes carry a prefix so every handler can tell them from real nodes.
const WRAP_PREFIX  = 'wrap:';
const FRAME_PREFIX = 'wrapframe:';
const RAIL_PREFIX  = 'wraprail:';   // v0.0.937 — the Enter view's port rails
const isWrapViewId = (id: string) => id.startsWith(WRAP_PREFIX) || id.startsWith(FRAME_PREFIX) || id.startsWith(RAIL_PREFIX);
// Handle id = {nodeId}_{Label}_{in|out}; node ids are 32-hex, no underscore
const realNodeOfHandle = (handle: string) => handle.split('_')[0];

// Inner handles of the edges that cross the border of a set of nodes
function wrapBoundaryHandles (members: Set<string>, edges: Edge[]): string[] {
  const out: string[] = [];
  for (const e of edges) {
    const s = members.has(e.source), t = members.has(e.target);
    if (s && !t && e.sourceHandle) out.push(e.sourceHandle);
    if (t && !s && e.targetHandle) out.push(e.targetHandle);
  }
  return out;
}

// Face ports = the ones remembered at wrap/fold time + any border crossing now
function wrapFacePorts (p: RawWrap, nodes: Node<any>[], edges: Edge[]): WrapPort[] {
  const members = new Set(p.nodeIds);
  const byId    = new Map(nodes.map(n => [n.id, n]));
  const seen    = new Set<string>();
  const res: WrapPort[] = [];
  for (const h of [...p.ports, ...wrapBoundaryHandles(members, edges)]) {
    if (seen.has(h)) continue;
    seen.add(h);
    const n = byId.get(realNodeOfHandle(h));
    if (!n || !members.has(n.id)) continue;
    const list = n.data?.ports as RawPort[] | undefined;
    const rp   = list?.find(q => q.id === h);
    if (list && !rp) continue;                       // port no longer exists
    res.push({
      handleId: h,
      label:    rp?.label ?? h.split('_')[1] ?? h,
      nodeName: (n.data?.customName as string) || (n.data?.label as string) || '',
      dir:      h.endsWith('_in') ? 'in' : 'out',
      colour:   colourForHandleId(h),
    });
  }
  // v0.0.934 — top to bottom by their inner node (then left to right): the
  // folded box and the open frame show the ports in the same order
  const pos = (h: string) => byId.get(realNodeOfHandle(h))?.position ?? { x: 0, y: 0 };
  return res.sort((a, b) => pos(a.handleId).y - pos(b.handleId).y || pos(a.handleId).x - pos(b.handleId).x);
}

// v0.0.934 — an edge class / resting colour from a handle id (same mapping
// as rawToFlowEdge and index.css)
function edgeClassForHandle (h: string): string {
  const s = h.toLowerCase();
  return s.includes('audio') ? 'edge-audio' : s.includes('osc') ? 'edge-osc' : s.includes('artdmx') ? 'edge-artnet'
       : s.includes('dmx') ? 'edge-dmx' : s.includes('mqtt') ? 'edge-mqtt' : s.includes('udp') ? 'edge-udp'
       : s.includes('value') ? 'edge-value' : 'edge-midi';
}
const EDGE_REST_COLOUR: Record<string, string> = {
  'edge-audio': 'rgb(20,80,20)', 'edge-osc': 'var(--osc)', 'edge-artnet': 'var(--artnet)', 'edge-dmx': 'var(--dmx)',
  'edge-mqtt': 'var(--mqtt)', 'edge-udp': 'var(--udp)', 'edge-value': 'var(--value)', 'edge-midi': 'var(--midi)',
};

// Label gutter of an open frame side: wide enough for its widest port label
// (8 px mono ≈ 5 px a character) so inner nodes never sit under a label
const frameGutter = (ports: WrapPort[]) =>
  Math.max(FRAME_PAD, ...ports.map(q => Math.ceil((q.label.length + 3 + q.nodeName.length) * 5) + 22));

// A folded wrap's ports are its inner ports (same handle ids), so the real
// node of a connection end is read from the handle id.
function toRealConnection (c: Connection): Connection {
  return {
    ...c,
    source: isWrapViewId(c.source) && c.sourceHandle ? realNodeOfHandle(c.sourceHandle) : c.source,
    target: isWrapViewId(c.target) && c.targetHandle ? realNodeOfHandle(c.targetHandle) : c.target,
  };
}

// v0.0.931 — a Pax's values: settingsJson is a plain array, one value per
// parameter (missing ones = default), same reading as GenericNode.
// v0.0.933 — the bespoke Pax nodes keep their own layouts; this table says
// where each parameter sits in their array (`slot`), which slots are UI-only
// and must be kept (`extras`: slot → default), and which parameters apply
// right now (`shown`). Must match the node components' own restore effects.
// v0.0.933 (follow-up) — also: `choices` = named buttons for a parameter
// (the Pax API only gives numbers; names as on the nodes), `pairs` = Low/High
// parameters drawn as one two-head slider, `zoom` = settings slots holding
// the node's own display range for those pairs (UI-only; edited on the node).
interface PaxLayout {
  slot:     (i: number) => number;
  extras?:  Record<number, number>;
  shown?:   (i: number, values: number[]) => boolean;
  choices?: Record<number, string[]>;
  pairs?:   (count: number) => [number, number][];
  zoom?:    [number, number];
}
const PAX_LAYOUTS: Record<string, PaxLayout> = {
  // AudioToDmxNode: params 0-4 at 0-4, Damping (param 5) at 7; 5-6 = zoom
  'Audio to DMX':      { slot: i => i === 5 ? 7 : i, extras: { 5: 20, 6: 20000 }, zoom: [5, 6],
                         choices: { 0: ['RMS', 'Freq'] }, pairs: () => [[2, 3]],
                         shown: (i, v) => (i !== 2 && i !== 3) || Math.round(v[0]) === 1 },
  // AudioPeakToOscNode: params 0-7 at 0-7; 8-9 = zoom
  'Audio Peak to OSC': { slot: i => i, extras: { 8: 20, 9: 20000 }, zoom: [8, 9],
                         choices: { 0: ['Whole', 'Freq'], 1: ['RMS', 'Peak'], 6: ['Change', 'Rate'] }, pairs: () => [[3, 4]],
                         shown: (i, v) => (i !== 3 && i !== 4) || Math.round(v[0]) === 1 },
  // SpectrumyserNode: [bandCount, lo0, hi0, lo1, hi1, …] — only active bands
  'Spectrumyser':      { slot: i => i, shown: (i, v) => i === 0 || Math.floor((i - 1) / 2) < Math.round(v[0] ?? 0),
                         pairs: count => Array.from({ length: Math.floor((count - 1) / 2) }, (_, b) => [1 + 2 * b, 2 + 2 * b] as [number, number]) },
  // EnvelopeNode, ChannelFilterPaxNode: one value per parameter, as generic
  'Envelope':          { slot: i => i, choices: { 0: ['Amp', 'Band'] }, pairs: () => [[6, 7]],
                         shown: (i, v) => (i !== 6 && i !== 7) || Math.round(v[0]) === 1 },
  'Channel Filter':    { slot: i => i, choices: { 1: ['Filter', 'Force'] } },
};
const layoutOf = (n: Node<any>): PaxLayout => PAX_LAYOUTS[n.data?.paxName as string] ?? { slot: i => i };

function rawSettings (n: Node<any>): unknown[] | null {
  try { const r = n.data?.settingsJson ? JSON.parse(n.data.settingsJson) : null; return Array.isArray(r) ? r : null; }
  catch { return null; }
}

function paxValues (n: Node<any>): number[] {
  const params = (n.data?.paxParams ?? []) as PaxParamInfo[];
  const raw = rawSettings(n);
  const { slot } = layoutOf(n);
  return params.map((p, i) => (raw && typeof raw[slot(i)] === 'number') ? raw[slot(i)] as number : p.defaultValue);
}

// The full settings array to store after one parameter changed — every
// parameter at its slot, UI-only slots kept (or their default)
function paxSettingsWith (n: Node<any>, index: number, value: number): number[] {
  const params = (n.data?.paxParams ?? []) as PaxParamInfo[];
  const raw = rawSettings(n) ?? [];
  const { slot, extras = {} } = layoutOf(n);
  const vals = paxValues(n);
  vals[index] = value;
  const out: number[] = [];
  Object.entries(extras).forEach(([k, d]) => { const v = raw[+k]; out[+k] = typeof v === 'number' ? v : d; });
  params.forEach((_, i) => { out[slot(i)] = vals[i]; });
  for (let k = 0; k < out.length; k++) if (typeof out[k] !== 'number') out[k] = typeof raw[k] === 'number' ? raw[k] as number : 0;
  return out;
}

// A Pax node's own colour (same detection as GenericNode's header)
const paxNodeAccent = (n: Node<any>) =>
  detectPaxTheme(n.data.ports ?? [], _paxInfoMap.get(n.data.paxName)?.colourCategory).accent;

// The Pax inside a wrap — generic ones and (v0.0.933) the bespoke ones,
// through PAX_LAYOUTS
// v0.0.949 — built-in nodes in a wrap whose controls the wrap's ⚙ panel shows
// (batch 1: Audio Player, DMX / ArtNet Console), top to bottom
// v0.0.951 — nodes with a ⏻ (Disable / Enable): every Pax and every built-in
// except the monitors, the consoles and the MIDI Keyboard
const NO_POWER = new Set([5, 6, 7, 16, 17, 18, 19, 20, 21, 24, 25]);
const canDisable = (n: Node<any>) => !!n.data?.paxName || !NO_POWER.has(n.data?.nodeType);

function wrapBuiltinsOf (p: RawWrap, nodes: Node<any>[]): WrapBuiltin[] {
  const ids = new Set(p.nodeIds);
  const kindOf = (t: number): WrapBuiltin['kind'] | null =>
    t === 26 ? 'player' : t === 17 ? 'dmxConsole' : t === 19 ? 'artNetConsole'
    : t === 2 ? 'midiOut' : t === 28 ? 'morpher' : t === 29 ? 'midiPlayer' : t === 7 ? 'keyboard' : null;   // v0.0.956 + Keyboard faders   // v0.0.951 — + MIDI Out, Morpher (its ⏻); v0.0.954 MIDI File Player
  const accentOf: Record<WrapBuiltin['kind'], string> = { player: 'var(--audio)', dmxConsole: 'var(--dmx)', artNetConsole: 'var(--artnet)',
                                                          midiOut: 'var(--midi)', morpher: 'var(--midi)', midiPlayer: 'var(--midi)', keyboard: 'var(--midi)' };
  return nodes
    .filter(n => ids.has(n.id) && kindOf(n.data?.nodeType) !== null)
    .sort((a, b) => a.position.y - b.position.y || a.position.x - b.position.x)
    .map(n => {
      const kind = kindOf(n.data.nodeType)!;
      let settings: Record<string, unknown> = {};
      try { settings = n.data.settingsJson ? JSON.parse(n.data.settingsJson as string) : {}; } catch { /* keep {} */ }
      return { nodeId: n.id, kind, accent: accentOf[kind], settings,
               disabled: !!n.data.disabled, canDisable: canDisable(n),
               name: (n.data.customName as string) || (settings.customName as string) || (n.data.label as string) || kind };
    })
    .filter(b => b.kind !== 'keyboard' || fadersOfSettings(b.settings).length > 0);   // v0.0.956 — a Keyboard shows only with faders
}

// v0.0.951 — the wrap's ⏻: is any member with a ⏻ on, and how many have one
function wrapPowerOf (p: RawWrap, nodes: Node<any>[]) {
  const ids = new Set(p.nodeIds);
  const members = nodes.filter(n => ids.has(n.id) && canDisable(n));
  return { anyOn: members.some(n => !n.data.disabled), count: members.length };
}

function wrapPaxOf (p: RawWrap, nodes: Node<any>[]): WrapPax[] {
  const ids = new Set(p.nodeIds);
  return nodes
    .filter(n => ids.has(n.id) && n.data?.paxName && (n.data?.paxParams ?? []).length > 0)
    .sort((a, b) => a.position.y - b.position.y || a.position.x - b.position.x)
    .map(n => ({
      nodeId: n.id,
      name:   (n.data.customName as string) || (n.data.paxName as string) || (n.data.label as string) || 'Pax',
      params: n.data.paxParams as PaxParamInfo[],
      values: paxValues(n),
      shown:  (() => { const v = paxValues(n), f = layoutOf(n).shown;
                       return (n.data.paxParams as PaxParamInfo[]).map((_, i) => f ? f(i, v) : true); })(),
      choices: layoutOf(n).choices ?? {},
      pairs:   layoutOf(n).pairs?.((n.data.paxParams as PaxParamInfo[]).length) ?? [],
      zoom:    (() => { const z = layoutOf(n).zoom, raw = rawSettings(n);
                        if (!z) return undefined;
                        const lo = raw?.[z[0]], hi = raw?.[z[1]];
                        return [typeof lo === 'number' ? lo : 20, typeof hi === 'number' ? hi : 20000] as [number, number]; })(),
      accent: paxNodeAccent(n),
      disabled: !!n.data.disabled,   // v0.0.951 — its ⏻
      // live values = what the Pax sends out: its output wires' colour
      // (same label-based mapping as the edges); several output types →
      // the node's own colour
      liveColour: (() => {
        const outs = ((n.data.ports ?? []) as RawPort[]).filter(q => q.direction === 'output').map(q => colourForHandleId(q.id));
        return outs.length > 0 && outs.every(c => c === outs[0]) ? outs[0] : paxNodeAccent(n);
      })(),
    }))
    // two Pax of the same name in one wrap: "Amp", "Amp 2" (top to bottom)
    .map((x, k, all) => {
      const same = all.slice(0, k).filter(y => y.name === x.name).length;
      return same > 0 ? { ...x, name: `${x.name} ${same + 1}` } : x;
    });
}

// Top-left / bottom-right of a wrap's members on the canvas
function wrapBounds (ids: string[], nodes: Node<any>[]) {
  const set = new Set(ids);
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const n of nodes) {
    if (!set.has(n.id)) continue;
    const w = n.measured?.width  ?? n.width  ?? 220;
    const h = n.measured?.height ?? n.height ?? 120;
    x0 = Math.min(x0, n.position.x);     y0 = Math.min(y0, n.position.y);
    x1 = Math.max(x1, n.position.x + w); y1 = Math.max(y1, n.position.y + h);
  }
  return x0 === Infinity ? null : { x0, y0, x1, y1 };
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
  // MidiChMatrixNode's own channel-flash feature — needs per-CHANNEL
  // timestamps, unlike every other map above (one expiry per node only).
  // node id -> { in: {channel -> expiry}, out: {channel -> expiry} }.
  const matrixChannelTimers = useRef<Map<string, { in: Map<number, number>; out: Map<number, number> }>>(new Map());
  const audioLevels   = useRef<Map<string, number>>(new Map());
  const portRmsLevels = useRef<Map<string, number[]>>(new Map());
  // Current DMX channel level per node (0-1), from the backend's own
  // persisted "last real value" slot — see WebBridge.h's PortActivity.
  // No decay applied, unlike audioLevels: a DMX channel holds a value
  // rather than firing transient events, so it should read as steady,
  // not fade out between updates.
  const dmxValues     = useRef<Map<string, number>>(new Map());
  // Last CSS string actually written to the DOM (added 2026-08-30, 2nd
  // pass) — see the comparison right before style.textContent is assigned,
  // further down in the render loop below, for the full reasoning.
  const lastAppliedCss = useRef<string>('');
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
            // Pax node (paxType = nodeType - 100) — never matches any of
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
        // MidiChMatrixNode's own per-channel flash — independent of
        // entry.midi above, since this node never touches the base
        // class's own recordMidiActivity() counter at all, only its own
        // two dedicated bitmasks.
        if (entry.inChMask || entry.outChMask) {
          let bucket = matrixChannelTimers.current.get(entry.id);
          if (! bucket) {
            bucket = { in: new Map(), out: new Map() };
            matrixChannelTimers.current.set(entry.id, bucket);
          }
          for (let ch = 0; ch < 16; ch++) {
            if (entry.inChMask  & (1 << ch)) bucket.in.set(ch, now + 80);
            if (entry.outChMask & (1 << ch)) bucket.out.set(ch, now + 80);
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

        const v = Math.max(0, Math.min(1, dmxValues.current.get(id) ?? 0));

        // Genuinely reverted 2026-08-30 (2nd pass) — a v<=0 skip was tried
        // here first, but the user correctly caught that it lost the
        // intended "dim floor glow at rest" look entirely (a v=0 node fell
        // back to NodeHandle's own solid, fully-opaque default colour, and
        // the edge fell back to ReactFlow's own plain default stroke —
        // both a jarring, hard jump away from the dim, translucent look a
        // barely-nonzero value shows). The right fix for the real
        // performance bug (see the note further down, right before
        // style.textContent is actually assigned) is to keep generating
        // this CSS exactly as before, unconditionally, for every v
        // including 0 — the floor alpha below already makes idle look
        // correctly dim — and instead only skip the expensive DOM write
        // itself when the fully-built CSS string hasn't actually changed
        // since the last frame. That fixes the real cost (a static,
        // unchanging string needs writing once, then never again) without
        // touching the visual behaviour at all.

        // Floor raised from 0.15 to 0.4 — at v=0 the old floor was nearly
        // invisible against the dark canvas background; still clearly
        // dimmer than a live channel, just no longer "gone".
        const alpha = 0.4 + v * 0.6;
        const bg    = `rgba(${DMX_RGB},${alpha})`;
        const px    = Math.round(4 + v * 10);
        const glow  = `0 0 ${px}px rgba(${DMX_RGB},${Math.min(1, alpha + 0.1)})`;

        // Includes the DMX In handle too, added 2026-08-30 (3rd pass) —
        // the user caught that a DmxMonitorNode's own upstream/input port
        // dot stayed permanently solid, unconnected or not. This block had
        // only ever targeted "DMX Out", never "DMX In" — for a pass-
        // through node like DmxMonitorNode, the input port never had any
        // CSS rule applied to it at all, in either version of today's
        // earlier fixes, so it always fell back to NodeHandle's own
        // solid, static default. Harmless no-op for isBuiltInDmx nodes
        // that don't actually have a "DMX In" handle at all (e.g.
        // DmxConsoleNode, output-only by design) — a selector matching no
        // element in the DOM simply does nothing. Same v as the output
        // side, since a pass-through node's own input and output share
        // the identical value by construction (DmxMonitorNode.h's own
        // outputValues[0] mirror is computed directly from inputDmxFrame).
        const targetSelectors: string[] = isBuiltInDmx
          ? [`[data-handleid="${id}_DMX Out_out"]`, `[data-handleid="${id}_DMX In_in"]`]
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

      // ArtNet's own continuous-intensity treatment, added 2026-08-30 —
      // migrated from "flash" to match DMX's own (see the note above this
      // whole DMX block for the full "intensity vs still-flashes" history)
      // per the user's own explicit request. Deliberately a separate,
      // parallel loop rather than folded into the DMX one above — the
      // two protocols share the same PAX_TYPE_DMX/PortActivity::dmxValue
      // field and the same --dmx accent colour by design (see
      // NodeUtils.tsx's own portColour, which doesn't distinguish them
      // at all), so this reuses dmxValues.current directly rather than
      // needing any separate data plumbing — but the node types, handle
      // labels ("ArtDMX Out"/"ArtDMX In", not "DMX Out"/"DMX In"), and
      // edge-filter condition (the mirror image of the DMX one — ArtNet
      // edges specifically, not DMX-excluding-ArtNet) are different
      // enough that keeping them as two clearly separate blocks reads
      // more plainly than one block branching on which protocol it's
      // currently handling. The old artNetTimers/artNetEdges/
      // isArtNetFlash "flash" mechanism above is left in place rather
      // than removed, matching this project's own established practice
      // for superseded-but-harmless code (see dmxTimers/dmxSources'
      // own precedent) — it's simply never triggered for ArtNet any
      // more, since this block's own CSS rule always wins by being
      // generated after it in the same combined string.
      nodesRef.current.forEach((nd: any) => {
        const id = nd.id;
        const nodeType = nd.data?.nodeType;
        const isBuiltInArtNet = nodeType === 12 || nodeType === 13 || nodeType === 18 || nodeType === 19;
        const nodePorts: any[] = nd.data?.ports ?? [];
        const artNetPaxPorts = nodeType >= 100
          ? nodePorts.filter(p => p.direction === 'output' && p.type !== 'audio' &&
              (p.id as string).toLowerCase().includes('artdmx'))
          : [];
        if (! isBuiltInArtNet && artNetPaxPorts.length === 0) return;

        const v = Math.max(0, Math.min(1, dmxValues.current.get(id) ?? 0));

        const alpha = 0.4 + v * 0.6;
        const bg    = `rgba(${DMX_RGB},${alpha})`;
        const px    = Math.round(4 + v * 10);
        const glow  = `0 0 ${px}px rgba(${DMX_RGB},${Math.min(1, alpha + 0.1)})`;

        const targetSelectors: string[] = isBuiltInArtNet
          ? [`[data-handleid="${id}_ArtDMX Out_out"]`, `[data-handleid="${id}_ArtDMX In_in"]`]
          : artNetPaxPorts.map(p => `[data-handleid="${p.id}"]`);
        targetSelectors.forEach(sel => {
          css += `${sel}{background:${bg}!important;box-shadow:${glow}!important;transition:background 80ms linear,box-shadow 80ms linear}`;
        });

        const nodeArtNetEdgesIntensity = edgeList.current.filter(e => {
          if (e.source !== id) return false;
          const h = (e.sourceHandle ?? '').toLowerCase();
          return h.includes('artdmx');
        });
        nodeArtNetEdgesIntensity.forEach(e => {
          css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${bg}!important;filter:drop-shadow(0 0 ${Math.round(4 + v * 6)}px rgba(${DMX_RGB},${alpha}));transition:stroke 80ms linear}`;
          if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${bg}!important;box-shadow:${glow}!important;transition:background 80ms linear,box-shadow 80ms linear}`;
        });
      });

      // The real performance fix (2026-08-30, 2nd pass) belongs here, not
      // in how the CSS above gets built — style.textContent forces a full
      // document style/layout recalculation on every assignment, even to
      // an identical string, since this is raw DOM manipulation with no
      // React diffing involved at all. An idle, unchanging DMX node (or
      // any other idle source) now builds the exact same css string every
      // single frame — comparing against the last string actually written
      // means that, once settled, an unchanging scene costs one real DOM
      // write, then zero more until something genuinely changes, while
      // fully preserving every visual — including the dim floor glow at
      // rest a v<=0 skip here would have removed (see that skip's own
      // revert note further up).
      // v0.0.934 — prefix match: an open wrap draws a border-crossing wire as
      // two (the real id outside, "<id>~in" inside its frame) and its frame
      // ports carry "<handle>" and "<handle>~in" — both halves flash with the
      // signal. Ids are UUIDs / end in _in|_out, so a prefix can't hit
      // another element.
      css = css.split('[data-id="').join('[data-id^="').split('[data-handleid="').join('[data-handleid^="');
      if (css !== lastAppliedCss.current)
      {
        style.textContent = css;
        lastAppliedCss.current = css;
      }
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

// v0.0.923 — buttons of the launch dialogs (FlowCanvas).
function dialogBtn (primary: boolean): React.CSSProperties {
  return {
    padding: '5px 14px', fontSize: 11, cursor: 'pointer',
    borderRadius: 'var(--radius)',
    border: `1px solid ${primary ? 'var(--accent)' : 'var(--border)'}`,
    background: primary ? 'var(--accent)' : 'transparent',
    color: primary ? 'var(--bg)' : 'var(--text)',
  };
}

function MenuDivider() {
  return <div style={{ borderTop: '1px solid var(--border)', margin: '4px 0' }} />;
}

// ── Main canvas ────────────────────────────────────────────────────────────
function FlowCanvas() {
  const { isStandalone } = useContext(DawContext);
  const [nodes, setNodes] = useState<Node<any>[]>([]);
  const [edges, setEdges] = useState<Edge[]>([]);

  // v0.0.930 — wraps (see WrapNode.tsx). wrapsRef mirrors the state so the
  // drag / fold handlers always see the latest values.
  const [wraps, setWrapsState] = useState<RawWrap[]>([]);
  const wrapsRef = useRef<RawWrap[]>([]);
  // v0.0.941 — wraps folded to the header straight from their open frame
  // (session only, like node folding): unfolding goes back to the frame
  const [headerFromOpen, setHeaderFromOpen] = useState<Set<string>>(new Set());
  const setWraps = useCallback((next: RawWrap[] | ((ps: RawWrap[]) => RawWrap[])) => {
    wrapsRef.current = typeof next === 'function' ? next(wrapsRef.current) : next;
    setWrapsState(wrapsRef.current);
  }, []);
  const [selectedWraps, setSelectedWraps] = useState<Set<string>>(new Set());
  // v0.0.937 — "Enter" view: the canvas shows only this wrap's nodes, its
  // outside connections as port rails at the sides. Viewports remembered per
  // wrap (and the main one) for the session.
  const [entered, setEntered] = useState<string | null>(null);
  const enteredRef = useRef<string | null>(null);
  enteredRef.current = entered;
  const viewports = useRef<Map<string, { x: number; y: number; zoom: number }>>(new Map());
  // the rails' measured size, given back on every render (else ReactFlow
  // re-measures them each frame of a drag and their wires flicker)
  const [railMeasured, setRailMeasured] = useState<Record<string, { width: number; height: number }>>({});
  const wrapDragStart = useRef<Map<string, { x: number; y: number }>>(new Map());
  // v0.0.935 — ⌥-drag into / out of open wraps: the frames' outlines frozen
  // at drag start (a frame grows with its members, so a member could never
  // be "outside" its live frame), and the join / leave highlight shown
  const dragFrames = useRef<{ wrapId: string; x: number; y: number; w: number; h: number }[]>([]);
  const [dropHint, setDropHint] = useState<Record<string, 'join' | 'leave'>>({});
  // While ⌥ is held during a drag, the open frames are DRAWN at their frozen
  // outline too (not just tested against it) — otherwise the frame visibly
  // grew with the node being taken out
  const [frozenFrames, setFrozenFrames] = useState<Record<string, { x: number; y: number; w: number; h: number }> | null>(null);
  const dragStartPos = useRef<Map<string, { x: number; y: number }>>(new Map());
  // Measured size of each folded box: ReactFlow needs it back on every render,
  // or it hides the box until it re-measures it (= flashing while dragging)
  const [wrapMeasured, setWrapMeasured] = useState<Record<string, { width: number; height: number }>>({});
  // Open-wrap frame drag: frame start + members' start positions
  const frameDrag = useRef<Map<string, { fx: number; fy: number; members: Map<string, { x: number; y: number }> }>>(new Map());

  // v0.0.919 — "Host paused — Patchy running independently" indicator
  // (bottom-left of the canvas). See Bridge.onHostPaused.
  const [hostPaused, setHostPaused] = useState(false);
  useEffect(() => Bridge.onHostPaused(setHostPaused), []);

  // v0.0.923 — recent files + the two launch dialogs (see Bridge / AppSettings.h).
  const [recentFiles, setRecentFiles] = useState<RecentFile[]>([]);
  const [recentOpen,  setRecentOpen]  = useState(false);
  const [fileMissing, setFileMissing] = useState<FileMissing | null>(null);
  const [askReopen,   setAskReopen]   = useState(false);
  useEffect(() => Bridge.onRecentFiles(setRecentFiles), []);
  useEffect(() => Bridge.onFileMissing(setFileMissing), []);
  useEffect(() => Bridge.onAskReopenLastProject(() => setAskReopen(true)), []);

  // v0.0.920 — plugin window resize grip (bottom-right; plugin builds only).
  // Hosts like Logic give AU windows no resize handle, and JUCE's own grip
  // sits under the WebView. Screen coordinates + pointer capture keep the
  // drag smooth while the window grows under the pointer; one request per
  // animation frame.
  const resizeDrag = useRef<{ x: number; y: number; w: number; h: number } | null>(null);
  const resizeRaf  = useRef(0);
  const resizeNext = useRef<[number, number]>([0, 0]);
  const onGripDown = (e: React.PointerEvent<HTMLDivElement>) => {
    e.preventDefault(); e.stopPropagation();
    e.currentTarget.setPointerCapture(e.pointerId);
    resizeDrag.current = { x: e.screenX, y: e.screenY, w: window.innerWidth, h: window.innerHeight };
  };
  const onGripMove = (e: React.PointerEvent<HTMLDivElement>) => {
    const d = resizeDrag.current;
    if (!d) return;
    resizeNext.current = [d.w + e.screenX - d.x, d.h + e.screenY - d.y];
    if (!resizeRaf.current)
      resizeRaf.current = requestAnimationFrame(() => {
        resizeRaf.current = 0;
        Bridge.resizeEditor(resizeNext.current[0], resizeNext.current[1]);
      });
  };
  const onGripUp = (e: React.PointerEvent<HTMLDivElement>) => {
    resizeDrag.current = null;
    if (e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
  };
  const wrapperRef = useRef<HTMLDivElement>(null);
  const { setHint } = useContext(HintContext);
  const { screenToFlowPosition, setViewport, getViewport, fitView, updateNode, getNodes, getEdges, deleteElements } = useReactFlow();
  const pendingDrop     = useRef<{ dropX: number; dropY: number } | null>(null);
  const knownNodeIds    = useRef<Set<string>>(new Set());
  const burgerBtnRef    = useRef<HTMLButtonElement>(null);
  const prefsBtnRef     = useRef<HTMLButtonElement>(null);
  const updateNodeInternals = useUpdateNodeInternals();
  const [prefs, setPrefs]       = useState<GraphPreferences>(loadPrefs);
  setMaxWrapFaders(prefs.maxWrapFaders ?? 4);   // v0.0.952 — read by the wrap's ⚙ panel
  setKbdFaderPrefs(prefs.maxKbdFaders ?? 4);   // v0.0.956 — read by the MIDI Keyboard
  const [showPrefs, setShowPrefs] = useState(false);
  const [allCollapsed, setAllCollapsed] = useState(false);
  // v0.0.948 — Panic state (latched + Preferences options), pushed by the engine
  const [panic, setPanic] = useState<PanicState>({ latched: false, blackout: true, mute: true });
  useEffect(() => Bridge.onPanicState(setPanic), []);
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
  // Refs (not direct state) for the same reason usePortActivityStyles uses
  // them for its own edgeList/nodesRef: the mouseover/mouseout listeners
  // below are added once (empty-ish deps) and read the CURRENT edges/nodes
  // on each hover, without needing to re-register the global listeners on
  // every edges/nodes change (which happens very frequently — every drag,
  // every connection).
  const edgesForHover = useRef(edges);
  const nodesForHover = useRef(nodes);
  useEffect(() => { edgesForHover.current = edges; }, [edges]);
  useEffect(() => { nodesForHover.current = nodes; }, [nodes]);

  // Current value of each node's own declared generic ("Value" edge)
  // output ports — Phase 5's own live readout-on-hover feature. Keyed by
  // nodeId, one array per node (ordered to match that node's own
  // Value-classified ports in its `ports` array — see Bridge.ts's own
  // PortActivityEntry.genericValuePortValues comment for the full
  // correlation reasoning).
  const genericValueData = useRef<Map<string, number[]>>(new Map());

  // The DOM element of whichever Value edge is currently being hovered, if
  // any — set on mouseover, cleared on mouseout (see handleMouseOut below).
  // Needed because `mouseover` only fires ONCE, when the cursor first
  // enters an element — never again while it just sits there — so without
  // this, the live readout would freeze at whatever it read at that one
  // instant, only updating again on the next leave-and-re-enter. Tracking
  // the hovered element lets the port-activity effect below re-run this
  // same computation every time fresh data arrives, independent of mouse
  // movement.
  const hoveredValueEdge = useRef<HTMLElement | null>(null);

  // Computes and applies the live-value hint for a given Value-edge DOM
  // element — shared by the initial mouseover handler and by the
  // port-activity effect's own continuous refresh below, so both stay
  // exactly in sync with no duplicated logic.
  const applyValueEdgeHint = useCallback((edge: HTMLElement) => {
    const edgeId = edge.getAttribute('data-id');
    const flowEdge = edgeId ? edgesForHover.current.find(fe => fe.id === edgeId) : null;
    const srcNode = flowEdge ? nodesForHover.current.find(n => n.id === flowEdge.source) : null;
    const srcPorts: RawPort[] = (srcNode?.data as any)?.ports ?? [];
    const valuePorts = srcPorts.filter(p => p.type === 'value' && p.direction === 'output');
    const portPos = flowEdge ? valuePorts.findIndex(p => p.id === flowEdge.sourceHandle) : -1;
    const values = flowEdge ? genericValueData.current.get(flowEdge.source) : undefined;
    const liveValue = (portPos >= 0 && values && portPos < values.length) ? values[portPos] : undefined;
    setHint(liveValue !== undefined
      ? { title: 'Value Connection', body: `Current value: ${liveValue.toFixed(3)}` }
      : (EDGE_HINTS['value'] ?? EDGE_HINTS['midi']));
  }, [setHint]);

  useEffect(() => {
    return Bridge.onPortActivity((entries: PortActivityEntry[]) => {
      entries.forEach(entry => {
        if (entry.genericValuePortValues && entry.genericValuePortValues.length > 0)
          genericValueData.current.set(entry.id, entry.genericValuePortValues);
      });
      // Refresh the hint live if a Value edge is currently being hovered —
      // see hoveredValueEdge's own comment above for why this is needed.
      if (hoveredValueEdge.current) applyValueEdgeHint(hoveredValueEdge.current);
    });
  }, [applyValueEdgeHint]);

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
        // ReactFlow edges are SVG <g> elements — .className on an SVG
        // element returns an SVGAnimatedString object, not a plain
        // string, so every .includes() check below was silently failing
        // for every edge type (not just this feature's own Value edges).
        // .getAttribute('class') always returns a plain string regardless
        // of element type — the standard, correct fix for this exact,
        // well-known SVG-vs-HTML platform quirk.
        const cls = edge.getAttribute('class') ?? '';
        if (cls.includes('edge-midi'))   setHint(EDGE_HINTS['midi']);
        else if (cls.includes('edge-audio')) setHint(EDGE_HINTS['audio']);
        else if (cls.includes('edge-mixed')) setHint(EDGE_HINTS['av']);
        else if (cls.includes('edge-osc'))   setHint(EDGE_HINTS['osc']   ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-dmx'))   setHint(EDGE_HINTS['dmx']   ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-mqtt'))  setHint(EDGE_HINTS['mqtt']  ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-udp'))   setHint(EDGE_HINTS['udp']   ?? EDGE_HINTS['midi']);
        else if (cls.includes('edge-value')) {
          // Live numeric readout — Phase 5's own "Value edges" feature.
          // Tracks this edge as currently hovered so the port-activity
          // effect (see hoveredValueEdge's own comment) can keep this
          // hint refreshed live, not just at this one instant.
          hoveredValueEdge.current = edge;
          applyValueEdgeHint(edge);
        }
        return;
      }
    };
    const handleMouseOut = (e: MouseEvent) => {
      const target = e.target as HTMLElement;
      if (target.closest('.react-flow__handle') || target.closest('.react-flow__edge')) {
        hoveredValueEdge.current = null;
        setHint(null);
      }
    };
    document.addEventListener('mouseover', handleMouseOver);
    document.addEventListener('mouseout',  handleMouseOut);
    return () => {
      document.removeEventListener('mouseover', handleMouseOver);
      document.removeEventListener('mouseout',  handleMouseOut);
    };
  }, [setHint]);

  // Phase 6 rename fix, 2026-09-25 — report text-field focus to C++ (its own
  // Cmd+Z shortcut in PatchyEditor::keyPressed() checks it), for every
  // input/textarea/contentEditable in the UI, not just node names. The state
  // is always recomputed from document.activeElement (on focus changes AND
  // on every key press, in the capture phase) rather than trusted from
  // focusin/focusout alone: a focused field removed from the DOM (node
  // deleted, graph loaded) doesn't reliably fire focusout, which would leave
  // C++ blocking graph undo. Only actual changes are sent.
  useEffect(() => {
    let reported = false;
    const isEditable = (el: Element | null) =>
      el instanceof HTMLElement &&
      (el.tagName === 'TEXTAREA' || el.isContentEditable ||
       (el.tagName === 'INPUT' && !['checkbox', 'radio', 'range', 'button', 'submit', 'color', 'file']
          .includes((el as HTMLInputElement).type)));
    const sync = () => {
      const now = isEditable(document.activeElement);
      if (now !== reported) { reported = now; Bridge.setTextEditing(now); }
    };
    const syncSoon = () => setTimeout(sync, 0);   // after focus has actually moved
    // Fix 2026-09-25 — WebKit's own autocomplete drop-down (values typed
    // before in similar fields) popped up in text fields, and the first
    // Escape only closed that list. Switched off on every text field at the
    // moment it gains focus, before any typing, so no component needs to
    // remember to set it itself.
    const noAutocomplete = (e: FocusEvent) => {
      const el = e.target;
      if (el instanceof HTMLInputElement && isEditable(el)) {
        el.setAttribute('autocomplete',   'off');
        el.setAttribute('autocorrect',    'off');
        el.setAttribute('autocapitalize', 'off');
        el.spellcheck = false;
      }
    };
    document.addEventListener('focusin',  noAutocomplete);
    document.addEventListener('focusin',  syncSoon);
    document.addEventListener('focusout', syncSoon);
    window.addEventListener('keydown',    sync, true);
    return () => {
      document.removeEventListener('focusin',  noAutocomplete);
      document.removeEventListener('focusin',  syncSoon);
      document.removeEventListener('focusout', syncSoon);
      window.removeEventListener('keydown',    sync, true);
    };
  }, []);

  // Keyboard shortcuts: Cmd+Z = undo, Cmd+Shift+Z = redo
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      const cmd = e.metaKey;  // Mac only — Cmd+Z, not Ctrl+Z
      if (!cmd) return;
      // Phase 6 rename fix, 2026-09-25 — never undo/redo the graph while
      // typing in a text field (native text undo applies instead).
      const ae = document.activeElement as HTMLElement | null;
      if (ae && (ae.tagName === 'TEXTAREA' || ae.isContentEditable ||
                 (ae.tagName === 'INPUT' && (ae as HTMLInputElement).type === 'text'))) return;
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

  // AudioPlayerNode transport shortcuts — Space toggles play/pause on
  // whichever Audio Player node is currently selected (not a global
  // transport affecting every player at once, deliberately, to avoid
  // surprising behaviour if more than one exists in the same graph);
  // a second Space press within 400ms is treated as a double-press,
  // returning to the start and stopping instead of toggling again.
  const audioPlayerStatusesRef = useRef<AudioPlayerStatus[]>([]);
  const midiPlayerStatusesRef  = useRef<{ nodeId: string; playing: boolean }[]>([]);
  useEffect(() => {
    const unsub = Bridge.onAudioPlayerStatus((statuses: AudioPlayerStatus[]) => {
      audioPlayerStatusesRef.current = statuses;
    });
    // v0.0.954 — the MIDI File Player answers Space too (same statuses list: nodeId + playing)
    const unsub2 = Bridge.onMidiFilePlayerStatus(st => {
      midiPlayerStatusesRef.current = st;
    });
    return () => { try { unsub(); unsub2(); } catch {} };
  }, []);

  const lastSpacePressRef = useRef(0);
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key !== ' ' || e.metaKey || e.ctrlKey ||
          document.activeElement instanceof HTMLInputElement ||
          document.activeElement instanceof HTMLTextAreaElement)
        return;

      const selectedPlayer = nodes.find(n => n.selected && (n.type === 'audioPlayer' || n.type === 'midiFilePlayer'));
      if (!selectedPlayer) return;
      const pre = selectedPlayer.type === 'midiFilePlayer' ? 'midiFilePlayer' : 'audioPlayer';
      const statuses = pre === 'midiFilePlayer' ? midiPlayerStatusesRef.current : audioPlayerStatusesRef.current;

      e.preventDefault();   // avoid scrolling the canvas or re-triggering a focused button

      const now = Date.now();
      const isDoublePress = now - lastSpacePressRef.current < 400;
      lastSpacePressRef.current = now;

      if (isDoublePress) {
        Bridge.setNodeParam(selectedPlayer.id, `${pre}Playing`, '0');
        Bridge.setNodeParam(selectedPlayer.id, `${pre}ReturnToStart`, '1');
      } else {
        const status = statuses.find(s => s.nodeId === selectedPlayer.id);
        const currentlyPlaying = status?.playing ?? false;
        Bridge.setNodeParam(selectedPlayer.id, `${pre}Playing`, currentlyPlaying ? '0' : '1');
      }
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [nodes]);
  usePortActivityStyles(edges, nodes);

  // ── Wraps (v0.0.930) ─────────────────────────────────────────────────────
  // Real nodes / edges stay the truth (and feed usePortActivityStyles above);
  // ReactFlow draws the derived view below.
  // The real nodes (folded members are not in ReactFlow's own list)
  const nodesRef = useRef<Node<any>[]>(nodes);
  nodesRef.current = nodes;
  const realNodesOfWrap = useCallback((wrapId: string) => {
    const p = wrapsRef.current.find(q => q.id === wrapId);
    if (!p) return [] as Node<any>[];
    const ids = new Set(p.nodeIds);
    return nodesRef.current.filter(n => ids.has(n.id));
  }, []);

  // ── Enter view (v0.0.937) ────────────────────────────────────────────────
  // The wrap's folded / open state is left as it is; the main viewport comes
  // back on leaving, each wrap's own one on entering it again.
  const enterWrap = useCallback((wrapId: string) => {
    if (!wrapsRef.current.some(w => w.id === wrapId)) return;
    viewports.current.set(enteredRef.current ?? '', getViewport());
    setSelectedWraps(new Set());
    setEntered(wrapId);
    setTimeout(() => {
      const vp = viewports.current.get(wrapId);
      if (vp) setViewport(vp);
      else fitView({ padding: 0.25, maxZoom: 1.2, duration: 200 });
    }, 60);
  }, [getViewport, setViewport, fitView]);

  const exitWrap = useCallback(() => {
    const id = enteredRef.current;
    if (!id) return;
    viewports.current.set(id, getViewport());
    setEntered(null);
    setTimeout(() => { const vp = viewports.current.get(''); if (vp) setViewport(vp); }, 60);
  }, [getViewport, setViewport]);

  // v0.0.949 — merge a patch into a node's settingsJson (local state + ref),
  // returning the merged object (for setNodeSettings / commitSettingsChange)
  const mergeNodeSettings = (nodeId: string, patch: Record<string, unknown>) => {
    const n = nodesRef.current.find(m => m.id === nodeId);
    let base: Record<string, unknown> = {};
    try { base = n?.data?.settingsJson ? JSON.parse(n.data.settingsJson as string) : {}; } catch { /* keep {} */ }
    const merged = { ...base, ...patch };
    const json = JSON.stringify(merged);
    nodesRef.current = nodesRef.current.map(m => m.id === nodeId ? { ...m, data: { ...m.data, settingsJson: json } } : m);
    setNodes(ns => ns.map(m => m.id === nodeId ? { ...m, data: { ...m.data, settingsJson: json } } : m));
    return merged;
  };
  const wrapActions = useMemo<WrapActions>(() => ({
    enter: (wrapId) => enterWrap(wrapId),
    saveToLibrary: (wrapId) => Bridge.wrapSaveToLibrary(wrapId),
    open: (wrapId) => {
      // Members come back measured from scratch (as after a file load):
      // sizes / handle positions kept from before they were hidden are stale
      const p   = wrapsRef.current.find(q => q.id === wrapId);
      const ids = new Set(p?.nodeIds ?? []);
      setNodes(ns => ns.map(n => ids.has(n.id) ? { ...n, measured: undefined } : n));
      setTimeout(() => ids.forEach(id => updateNodeInternals(id)), 50);
      setWraps(ps => ps.map(p => p.id === wrapId ? { ...p, open: true } : p));
      setSelectedWraps(s => { const n = new Set(s); n.delete(wrapId); return n; });
      setHeaderFromOpen(s => { if (!s.has(wrapId)) return s; const n = new Set(s); n.delete(wrapId); return n; });
      Bridge.wrapSetOpen(wrapId, true);
    },
    close: (wrapId) => {
      const p = wrapsRef.current.find(q => q.id === wrapId);
      if (!p) return;
      const all   = getNodes().filter(n => !isWrapViewId(n.id));
      const ports = wrapFacePorts(p, all, getEdges()).map(q => q.handleId);
      const b     = wrapBounds(p.nodeIds, all);
      const x = b ? b.x0 : p.x, y = b ? b.y0 : p.y;
      // members leave the selection as they disappear
      const ids = new Set(p.nodeIds);
      setNodes(ns => ns.map(n => ids.has(n.id) && n.selected ? { ...n, selected: false } : n));
      setWraps(ps => ps.map(q => q.id === wrapId ? { ...q, open: false, ports, x, y } : q));
      Bridge.wrapSetOpen(wrapId, false, ports);
      Bridge.wrapMove(wrapId, x, y);
    },
    unwrap: (wrapId) => Bridge.wrapUnwrap(wrapId),
    remove: (wrapId) => Bridge.wrapRemove(wrapId),
    // v0.0.949 — built-in nodes' controls: the same messages as the nodes'
    // own controls; settings changes also update the local settingsJson so
    // the panel, the summary and the node itself (when shown) agree
    playerTransport: (nodeId, cmd, playing) => {
      // v0.0.954 — the MIDI File Player shares the Audio Player's transport row
      const pre = nodesRef.current.find(m => m.id === nodeId)?.data?.nodeType === 29 ? 'midiFilePlayer' : 'audioPlayer';
      if (cmd === 'start') Bridge.setNodeParam(nodeId, `${pre}ReturnToStart`, '1');
      else                 Bridge.setNodeParam(nodeId, `${pre}Playing`, playing ? '0' : '1');
    },
    playerLevel: (nodeId, level, commit) => {
      const merged = mergeNodeSettings(nodeId, { level });
      Bridge.setNodeSettings(nodeId, merged);
      Bridge.setNodeParam(nodeId, 'audioPlayerLevel', String(level));
      if (commit) Bridge.commitNodeSettings(nodeId);
    },
    consoleBlackout: (nodeId, kind, on) => {
      const merged = mergeNodeSettings(nodeId, { blackout: on });
      if (kind === 'dmxConsole') Bridge.setDmxBlackout(nodeId, on); else Bridge.setArtNetBlackout(nodeId, on);
      Bridge.commitSettingsChange(nodeId, merged);   // one undo step, like the console's own button
    },
    // v0.0.950 — console faders: the console's own channel path (live) + its
    // settings (base64, like its own faders) — one undo step per drag
    consoleFader: (nodeId, kind, ch, value, commit) => {
      const key = kind === 'dmxConsole' ? 'dmxChannels' : 'artNetChannels';
      const n = nodesRef.current.find(m => m.id === nodeId);
      let base: Record<string, unknown> = {};
      try { base = n?.data?.settingsJson ? JSON.parse(n.data.settingsJson as string) : {}; } catch { /* {} */ }
      const bytes = new Uint8Array(512);
      if (typeof base[key] === 'string') { try { const bin = atob(base[key] as string); for (let i = 0; i < 512 && i < bin.length; i++) bytes[i] = bin.charCodeAt(i); } catch { /* zeros */ } }
      bytes[ch - 1] = Math.max(0, Math.min(255, Math.round(value)));
      let bin = ''; for (let i = 0; i < 512; i++) bin += String.fromCharCode(bytes[i]);
      const merged = mergeNodeSettings(nodeId, { [key]: btoa(bin) });
      Bridge.setNodeSettings(nodeId, merged);
      if (kind === 'dmxConsole') Bridge.setDmxConsoleChannel(nodeId, ch - 1, bytes[ch - 1]);
      else                       Bridge.setArtNetConsoleChannel(nodeId, ch - 1, bytes[ch - 1]);
      if (commit) Bridge.commitNodeSettings(nodeId);
    },
    // v0.0.956 — a MIDI Keyboard fader: its MIDI now, its value saved (one undo step per drag)
    kbdFader: (nodeId, k, value, commit) => {
      const n = nodesRef.current.find(m => m.id === nodeId);
      let base: Record<string, unknown> = {};
      try { base = n?.data?.settingsJson ? JSON.parse(n.data.settingsJson as string) : {}; } catch { /* {} */ }
      const faders = fadersOfSettings(base);
      if (! faders[k]) return;
      faders[k] = { ...faders[k], value: Math.max(0, Math.min(maxValue(faders[k].type), Math.round(value))) };
      const merged = mergeNodeSettings(nodeId, { faders });
      Bridge.setNodeSettings(nodeId, merged);
      sendKbdFader(nodeId, kbdChannel(base), k, faders[k], commit);
      if (commit) Bridge.commitNodeSettings(nodeId);
    },
    consoleFaderChannel: (nodeId, faders, commit) => {
      const merged = mergeNodeSettings(nodeId, { wrapFaders: faders });
      Bridge.setNodeSettings(nodeId, merged);
      if (commit) Bridge.commitNodeSettings(nodeId);
    },
    // v0.0.951 — batch 2
    midiOutChannels: (nodeId, channels) => {   // same message as the node's own filter (saved + undoable there)
      mergeNodeSettings(nodeId, { channels });
      Bridge.setNodeParam(nodeId, 'midiOutChannelFilter', JSON.stringify(channels));
    },
    setDisabled: (nodeId, disabled) => {       // the node's own ⏻ (live, one undo step)
      setNodes(ns => ns.map(m => m.id === nodeId ? { ...m, data: { ...m.data, disabled } } : m));
      Bridge.setNodeParam(nodeId, 'disabled', disabled ? '1' : '0');
    },
    wrapPower: (wrapId) => {                   // any member on → all off; all off → all on
      const p = wrapsRef.current.find(q => q.id === wrapId);
      if (!p) return;
      const ids = new Set(p.nodeIds);
      const members = nodesRef.current.filter(n => ids.has(n.id) && canDisable(n));
      if (members.length === 0) return;
      const off = members.some(n => !n.data.disabled);
      setNodes(ns => ns.map(m => members.some(q => q.id === m.id) ? { ...m, data: { ...m.data, disabled: off } } : m));
      Bridge.wrapSetDisabled(members.map(n => n.id), off);
    },
    foldHeader: (wrapId) => {   // v0.0.941 — see headerFromOpen
      setHeaderFromOpen(s => new Set(s).add(wrapId));
      wrapActionsRef.current?.close(wrapId);
    },
    rename: (wrapId, name) => {
      setWraps(ps => ps.map(p => p.id === wrapId ? { ...p, name } : p));
      Bridge.wrapRename(wrapId, name);
    },
    // v0.0.931 — the wrap panel edits the (not drawn) Pax node exactly like
    // its own controls do; the local settingsJson is updated too, so the
    // panel, the summary and the node itself (when shown again) agree
    setParam: (nodeId, index, value) => {
      const n = nodesRef.current.find(m => m.id === nodeId);
      if (!n) return;
      const vals = paxSettingsWith(n, index, value);
      const json = JSON.stringify(vals);
      nodesRef.current = nodesRef.current.map(m => m.id === nodeId ? { ...m, data: { ...m.data, settingsJson: json } } : m);
      setNodes(ns => ns.map(m => m.id === nodeId ? { ...m, data: { ...m.data, settingsJson: json } } : m));
      Bridge.setNodeSettings(nodeId, vals);
      Bridge.setPaxParameter(nodeId, index, value);
    },
    commitParam: (nodeId) => Bridge.commitNodeSettings(nodeId),
    // v0.0.932 — pin / unpin / rename a control on the box (one undo step)
    setPins: (wrapId, pins) => {
      setWraps(ps => ps.map(p => p.id === wrapId ? { ...p, pins } : p));
      Bridge.wrapSetPins(wrapId, pins);
    },
  }), [getNodes, getEdges, setWraps, updateNodeInternals, enterWrap]);
  const wrapActionsRef = useRef<WrapActions | null>(null);
  wrapActionsRef.current = wrapActions;
  // v0.0.941 — the global ⇕ fold also folds open frames to their header, and
  // unfolding brings them back open (folded boxes follow via _forceCollapsed)
  const firstAllCollapsed = useRef(true);
  useEffect(() => {
    if (firstAllCollapsed.current) { firstAllCollapsed.current = false; return; }
    if (allCollapsed) {
      for (const p of wrapsRef.current) if (p.open) wrapActions.foldHeader(p.id);
    } else {
      for (const id of headerFromOpen) if (wrapsRef.current.some(p => p.id === id && !p.open)) wrapActions.open(id);
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [allCollapsed]);

  // Left automatically if the wrap goes (unwrapped, deleted, undone)
  useEffect(() => {
    if (entered && !wraps.some(w => w.id === entered)) exitWrap();
  }, [wraps, entered, exitWrap]);

  // ⌘↑ enter (the selected folded wrap, or the open wrap of a selected node),
  // ⌘↓ / Esc back to the main graph — not while typing (user's choice)
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      const ae = document.activeElement as HTMLElement | null;
      if (ae && (ae.tagName === 'TEXTAREA' || ae.tagName === 'INPUT' || ae.isContentEditable)) return;
      if (enteredRef.current && (e.key === 'Escape' || (e.metaKey && e.key === 'ArrowDown'))) {
        e.preventDefault(); exitWrap(); return;
      }
      if (!enteredRef.current && e.metaKey && e.key === 'ArrowUp') {
        const sel = new Set(getNodes().filter(n => n.selected && !isWrapViewId(n.id)).map(n => n.id));
        const target = wrapsRef.current.find(w => selectedWraps.has(w.id))
                    ?? wrapsRef.current.find(w => w.open && w.nodeIds.some(id => sel.has(id)));
        if (target) { e.preventDefault(); enterWrap(target.id); }
      }
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [enterWrap, exitWrap, getNodes, selectedWraps]);

  const view = useMemo(() => {
    // v0.0.937 — Enter view: only the wrap's nodes; wires to / from outside
    // end on two port rails (left: from outside, right: to outside) whose
    // handles reuse the inner ports' ids, so wires keep their ids and flash
    const ew = entered ? wraps.find(q => q.id === entered) : undefined;
    if (ew) {
      const members = new Set(ew.nodeIds);
      const inner   = nodes.filter(n => members.has(n.id));
      const ports   = wrapFacePorts(ew, nodes, edges);
      const b = wrapBounds(ew.nodeIds, nodes) ?? { x0: 0, y0: 0, x1: 400, y1: 200 };
      const rail = (side: 'in' | 'out', x: number): Node<any> => ({
        id: RAIL_PREFIX + side, type: 'wrapRail', position: { x, y: b.y0 },
        draggable: false, selectable: false, deletable: false, focusable: false, measured: railMeasured[side],
        data: { side, ports: ports.filter(q => q.dir === side) },
      });
      const vEdges: Edge[] = [];
      for (const e of edges) {
        const s = members.has(e.source), t = members.has(e.target);
        if (s && t) vEdges.push(e);
        else if (t) vEdges.push({ ...e, source: RAIL_PREFIX + 'in',  sourceHandle: e.targetHandle });
        else if (s) vEdges.push({ ...e, target: RAIL_PREFIX + 'out', targetHandle: e.sourceHandle });
      }
      // v0.0.938 — a port with nothing connected outside (e.g. a wrap just
      // dropped from the library) still shows its link to the inner node:
      // a dimmed drawn-only stub, as on the open frame
      for (const q of ports) {
        const used = edges.some(e => (q.dir === 'in'  && e.targetHandle === q.handleId && !members.has(e.source))
                                  || (q.dir === 'out' && e.sourceHandle === q.handleId && !members.has(e.target)));
        if (used) continue;
        const cls = edgeClassForHandle(q.handleId), node = realNodeOfHandle(q.handleId);
        vEdges.push({
          id: `stub~in:enter:${q.handleId}`, className: cls,
          style: { strokeWidth: 2, stroke: EDGE_REST_COLOUR[cls], opacity: 0.55 },
          selectable: false, deletable: false, reconnectable: false, focusable: false,
          ...(q.dir === 'in'
            ? { source: RAIL_PREFIX + 'in', sourceHandle: q.handleId, target: node, targetHandle: q.handleId }
            : { source: node, sourceHandle: q.handleId, target: RAIL_PREFIX + 'out', targetHandle: q.handleId }),
        } as Edge);
      }
      return { nodes: [...inner, rail('in', b.x0 - 300), rail('out', b.x1 + 140)], edges: vEdges };
    }
    if (wraps.length === 0) return { nodes, edges };
    const closedOf = new Map<string, string>();       // member → its folded wrap
    for (const p of wraps) if (!p.open) for (const id of p.nodeIds) closedOf.set(id, p.id);

    const frames: Node<any>[] = [];
    const boxes:  Node<any>[] = [];
    const frameStubs = new Map<string, WrapPort[]>();   // open wrap → its frame ports
    const portPos = new Map<string, { x: number; y: number }>();   // "<wrap>|<handle>" → frame port centre
    for (const p of wraps) {
      if (p.open) {
        const b = wrapBounds(p.nodeIds, nodes);
        if (!b) continue;
        // v0.0.934 — border ports on the frame, as on the folded box, ordered
        // top to bottom by their inner node (fewer crossings)
        const fports = wrapFacePorts(p, nodes, edges);
        const rows = Math.max(fports.filter(q => q.dir === 'in').length, fports.filter(q => q.dir === 'out').length);
        // label gutters left (inputs) / right (outputs)
        const gl = frameGutter(fports.filter(q => q.dir === 'in')), gr = frameGutter(fports.filter(q => q.dir === 'out'));
        frameStubs.set(p.id, fports);
        const fz = frozenFrames?.[p.id];
        const w = fz ? fz.w : b.x1 - b.x0 + gl + gr;
        const h = fz ? fz.h : Math.max(b.y1 - b.y0 + 2 * FRAME_PAD + FRAME_HEAD, FRAME_HEAD + FRAME_PORT_TOP + rows * FRAME_PORT_GAP);
        const fx = fz ? fz.x : b.x0 - gl, fy = fz ? fz.y : b.y0 - FRAME_PAD - FRAME_HEAD;
        fports.filter(q => q.dir === 'in').forEach((q, k) =>
          portPos.set(`${p.id}|${q.handleId}`, { x: fx,     y: fy + FRAME_HEAD + FRAME_PORT_TOP + k * FRAME_PORT_GAP }));
        fports.filter(q => q.dir === 'out').forEach((q, k) =>
          portPos.set(`${p.id}|${q.handleId}`, { x: fx + w, y: fy + FRAME_HEAD + FRAME_PORT_TOP + k * FRAME_PORT_GAP }));
        frames.push({
          id: FRAME_PREFIX + p.id, type: 'wrapFrame',
          position: { x: fx, y: fy },
          width: w, height: h, zIndex: -1, dragHandle: '.wrap-frame-drag',
          selectable: false, deletable: false, focusable: false,
          style: { width: w, height: h, pointerEvents: 'none' },
          data: { wrap: p, actions: wrapActions, ports: fports, drop: dropHint[p.id], power: wrapPowerOf(p, nodes) },
        });
      } else {
        boxes.push({
          id: WRAP_PREFIX + p.id, type: 'wrap', position: { x: p.x, y: p.y },
          deletable: false, selected: selectedWraps.has(p.id), measured: wrapMeasured[p.id],
          data: { wrap: p, ports: wrapFacePorts(p, nodes, edges), pax: wrapPaxOf(p, nodes), builtins: wrapBuiltinsOf(p, nodes), power: wrapPowerOf(p, nodes), actions: wrapActions,
                  _forceCollapsed: allCollapsed,     // v0.0.941 — global ⇕ fold
                  fromOpen: headerFromOpen.has(p.id) },
        });
      }
    }
    // Folded members are left out entirely (not `hidden`): ReactFlow keeps
    // stale internals for hidden nodes, and a member shown again after being
    // drawn once stayed invisible. Left out, it comes back as a fresh node.
    const shown = nodes.filter(n => !closedOf.has(n.id));
    // v0.0.934 — open wraps: a wire crossing the frame stays ONE wire (its
    // real id and class: colour, flash, select, delete all as usual), drawn
    // through the frame port(s) it crosses — edge type "wrapRoute" with the
    // port centres as `vias`. (A first version split it into two wires; the
    // inside halves missed the class colouring / MIDI flash.)
    const openOf = new Map<string, string>();
    for (const p of wraps) if (p.open) for (const id of p.nodeIds) openOf.set(id, p.id);
    const viewEdges: Edge[] = [];
    for (const e of edges) {
      const sp = closedOf.get(e.source), tp = closedOf.get(e.target);
      if (sp && sp === tp) continue;                   // internal to a folded wrap
      let out: Edge = (sp || tp) ? { ...e, source: sp ? WRAP_PREFIX + sp : e.source, target: tp ? WRAP_PREFIX + tp : e.target } : e;
      const so = openOf.get(e.source), to = openOf.get(e.target);
      if (so !== to) {
        const vias: { x: number; y: number }[] = [];
        const vo = so && e.sourceHandle ? portPos.get(`${so}|${e.sourceHandle}`) : undefined;   // leaves a frame (right edge)
        const vi = to && e.targetHandle ? portPos.get(`${to}|${e.targetHandle}`) : undefined;   // enters a frame (left edge)
        if (vo) vias.push(vo);
        if (vi) vias.push(vi);
        if (vias.length) out = { ...out, type: 'wrapRoute', data: { ...(out.data ?? {}), vias } };
      }
      viewEdges.push(out);
    }
    // v0.0.934 — a frame port with no wire crossing right now (it stays on the
    // frame, as on the folded box) keeps its inside part, drawn as a stub
    for (const [wrapId, fports] of frameStubs) {
      const members = new Set(wraps.find(q => q.id === wrapId)?.nodeIds ?? []);
      for (const q of fports) {
        const used = edges.some(e => (q.dir === 'in'  && e.targetHandle === q.handleId && !members.has(e.source))
                                  || (q.dir === 'out' && e.sourceHandle === q.handleId && !members.has(e.target)));
        if (used) continue;
        const cls = edgeClassForHandle(q.handleId), node = realNodeOfHandle(q.handleId);
        viewEdges.push({
          id: `stub~in:${wrapId}:${q.handleId}`, className: cls,
          style: { strokeWidth: 2, stroke: EDGE_REST_COLOUR[cls], opacity: 0.55 },
          selectable: false, deletable: false, reconnectable: false, focusable: false,
          ...(q.dir === 'in'
            ? { source: FRAME_PREFIX + wrapId, sourceHandle: `${q.handleId}~in`, target: node, targetHandle: q.handleId }
            : { source: node, sourceHandle: q.handleId, target: FRAME_PREFIX + wrapId, targetHandle: `${q.handleId}~in` }),
        } as Edge);
      }
    }
    return { nodes: [...frames, ...shown, ...boxes], edges: viewEdges };
  }, [nodes, edges, wraps, selectedWraps, wrapActions, wrapMeasured, dropHint, frozenFrames, entered, railMeasured, allCollapsed, headerFromOpen, prefs.maxWrapFaders]);

  // ── ⌥-drag: nodes join / leave open wraps (v0.0.935) ──────────────────────
  // Plain drags are unchanged. With ⌥ held, a node whose centre is dropped
  // inside another open wrap's frame joins it (leaving its own wrap, one
  // level); a member dropped outside its own frame leaves. Its wires stay as
  // they are. One undo step for the whole drop.
  const edgesRef = useRef<Edge[]>(edges);
  edgesRef.current = edges;

  const onNodeDragStart = useCallback((_ev: unknown, _n: Node, dragged: Node[]) => {
    dragStartPos.current = new Map(dragged.filter(n => !n.id.startsWith(FRAME_PREFIX)).map(n => [n.id, { ...n.position }]));
    // a folded wrap's members (not drawn) — where they are now
    for (const n of dragged) if (n.id.startsWith(WRAP_PREFIX))
      for (const m of realNodesOfWrap(n.id.slice(WRAP_PREFIX.length)))
        dragStartPos.current.set(m.id, { ...m.position });
    dragFrames.current = getNodes()
      .filter(n => n.id.startsWith(FRAME_PREFIX))
      .map(n => ({ wrapId: n.id.slice(FRAME_PREFIX.length), x: n.position.x, y: n.position.y,
                   w: (n.width ?? n.measured?.width ?? 0), h: (n.height ?? n.measured?.height ?? 0) }));
  }, [getNodes, realNodesOfWrap]);

  // The changes an ⌥-drop of these nodes would make
  const dropChanges = useCallback((dragged: Node[]) => {
    const memberOf = new Map<string, string>();
    for (const p of wrapsRef.current) for (const id of p.nodeIds) memberOf.set(id, p.id);
    const add = new Map<string, string[]>(), remove = new Map<string, string[]>();
    const push = (m: Map<string, string[]>, k: string, v: string) => m.set(k, [...(m.get(k) ?? []), v]);
    for (const n of dragged) {
      if (n.id.startsWith(FRAME_PREFIX)) continue;
      if (n.id.startsWith(WRAP_PREFIX)) {
        // a folded wrap dropped into an open one: merged (its nodes join; it goes)
        const wrapId = n.id.slice(WRAP_PREFIX.length);
        const bx = n.position.x + (n.measured?.width  ?? 220) / 2;
        const by = n.position.y + (n.measured?.height ?? 80)  / 2;
        const into = dragFrames.current.find(r => r.wrapId !== wrapId && bx >= r.x && bx <= r.x + r.w && by >= r.y && by <= r.y + r.h);
        if (into) for (const id of wrapsRef.current.find(q => q.id === wrapId)?.nodeIds ?? []) push(add, into.wrapId, id);
        continue;
      }
      const cx = n.position.x + (n.measured?.width  ?? 220) / 2;
      const cy = n.position.y + (n.measured?.height ?? 120) / 2;
      const inside = dragFrames.current.find(r => cx >= r.x && cx <= r.x + r.w && cy >= r.y && cy <= r.y + r.h);
      const own = memberOf.get(n.id);
      if (inside && inside.wrapId !== own) push(add, inside.wrapId, n.id);        // joins (leaves its own, if any)
      else if (own && dragFrames.current.some(r => r.wrapId === own) && inside?.wrapId !== own)
        push(remove, own, n.id);                                               // leaves its open wrap
    }
    return { add, remove };
  }, []);

  const onNodeDrag = useCallback((ev: React.MouseEvent | MouseEvent, _n: Node, dragged: Node[]) => {
    if (!ev.altKey || dragged.every(n => n.id.startsWith(FRAME_PREFIX))) {   // (dragging a frame itself: nothing to do)
      setDropHint(h => Object.keys(h).length ? {} : h);
      setFrozenFrames(f => f ? null : f);
      return;
    }
    setFrozenFrames(f => f ?? Object.fromEntries(dragFrames.current.map(r => [r.wrapId, { x: r.x, y: r.y, w: r.w, h: r.h }])));
    const { add, remove } = dropChanges(dragged);
    const next: Record<string, 'join' | 'leave'> = {};
    remove.forEach((_, k) => { next[k] = 'leave'; });
    add.forEach((_, k) => { next[k] = 'join'; });
    setDropHint(h => JSON.stringify(h) === JSON.stringify(next) ? h : next);
  }, [dropChanges]);

  const onNodeDragStop = useCallback((ev: React.MouseEvent | MouseEvent, _n: Node, dragged: Node[]) => {
    setDropHint({});
    setFrozenFrames(null);
    if (!ev.altKey) return;
    const { add, remove } = dropChanges(dragged);
    if (add.size === 0 && remove.size === 0) return;
    const allEdges = edgesRef.current;
    const changes = [...new Set([...add.keys(), ...remove.keys()])].map(wrapId => {
      const p = wrapsRef.current.find(q => q.id === wrapId)!;
      const a = add.get(wrapId) ?? [], r = remove.get(wrapId) ?? [];
      const members = new Set([...p.nodeIds.filter(id => !r.includes(id)), ...a]);
      // face ports: the old ones still on a member and not now fully inside,
      // plus whatever crosses the new border
      const internalOnly = (h: string) => {
        const es = allEdges.filter(e => e.sourceHandle === h || e.targetHandle === h);
        return es.length > 0 && es.every(e => members.has(e.source) && members.has(e.target));
      };
      const ports = [...new Set([...p.ports.filter(h => members.has(realNodeOfHandle(h)) && !internalOnly(h)),
                                 ...wrapBoundaryHandles(members, allEdges)])];
      return { wrapId, add: a, remove: r, ports };
    });
    const moves: { id: string; fromX: number; fromY: number; toX: number; toY: number; wrap?: boolean }[] = [];
    for (const n of dragged) {
      const s = dragStartPos.current.get(n.id);
      if (!s) continue;
      if (n.id.startsWith(WRAP_PREFIX)) {
        const wrapId = n.id.slice(WRAP_PREFIX.length), dx = n.position.x - s.x, dy = n.position.y - s.y;
        moves.push({ id: wrapId, wrap: true, fromX: s.x, fromY: s.y, toX: n.position.x, toY: n.position.y });
        for (const id of wrapsRef.current.find(q => q.id === wrapId)?.nodeIds ?? []) {
          const m = dragStartPos.current.get(id);
          if (m) moves.push({ id, fromX: m.x, fromY: m.y, toX: m.x + dx, toY: m.y + dy });
        }
      } else moves.push({ id: n.id, fromX: s.x, fromY: s.y, toX: n.position.x, toY: n.position.y });
    }
    Bridge.wrapChange(changes, moves);
  }, [dropChanges]);

  // ⌘G — wrap the selected nodes (2 or more)
  const wrapSelection = useCallback(() => {
    const sel = getNodes().filter(n => n.selected && !n.hidden && !isWrapViewId(n.id));
    if (sel.length < 2) return;
    const ids     = sel.map(n => n.id);
    const members = new Set(ids);
    const ports   = [...new Set(wrapBoundaryHandles(members, getEdges()))];
    const x = Math.min(...sel.map(n => n.position.x));
    const y = Math.min(...sel.map(n => n.position.y));
    setNodes(ns => ns.map(n => members.has(n.id) ? { ...n, selected: false } : n));
    Bridge.wrapNodes(ids, 'WRAP', ports, x, y);
  }, [getNodes, getEdges]);

  // ⌘⇧G — unwrap the selected wraps, or the open wraps holding a selected node
  const unwrapSelection = useCallback(() => {
    const selNodes = new Set(getNodes().filter(n => n.selected && !isWrapViewId(n.id)).map(n => n.id));
    for (const p of wrapsRef.current)
      if ((!p.open && selectedWraps.has(p.id)) || (p.open && p.nodeIds.some(id => selNodes.has(id))))
        Bridge.wrapUnwrap(p.id);
  }, [getNodes, selectedWraps]);

  const canWrap   = () => getNodes().filter(n => n.selected && !n.hidden && !isWrapViewId(n.id)).length >= 2;
  const canUnwrap = () => {
    const selNodes = new Set(getNodes().filter(n => n.selected && !isWrapViewId(n.id)).map(n => n.id));
    return wrapsRef.current.some(p => (!p.open && selectedWraps.has(p.id)) || (p.open && p.nodeIds.some(id => selNodes.has(id))));
  };

  const lastWrapKey = useRef(0);
  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      const ae = document.activeElement as HTMLElement | null;
      if (ae && (ae.tagName === 'TEXTAREA' || ae.tagName === 'INPUT' || ae.isContentEditable)) return;
      const native = e.metaKey && (e.key === 'g' || e.key === 'G');
      const wrap   = e.key === 'Wrap'   || (native && !e.shiftKey);
      const unwrap = e.key === 'Unwrap' || (native &&  e.shiftKey);
      if (!wrap && !unwrap) return;
      e.preventDefault();
      // native + forwarded copies of the same keystroke: act once
      const now = Date.now();
      if (now - lastWrapKey.current < 200) return;
      lastWrapKey.current = now;
      if (enteredRef.current) return;   // not inside the Enter view (one level)
      if (wrap) wrapSelection(); else unwrapSelection();
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [wrapSelection, unwrapSelection]);


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
        setWraps(state.wraps ?? []);
      });

      // Update node internals so edge endpoints snap to port dots after load
      setTimeout(() => {
        state.nodes.forEach(n => updateNodeInternals(n.id));
      }, 50);

      // Restore saved viewport if present (not while inside a wrap: the saved
      // one is the main graph's)
      if (state.viewportZoom && state.viewportZoom > 0 && !enteredRef.current) {
        setViewport({
          x:    state.viewportX    ?? 0,
          y:    state.viewportY    ?? 0,
          zoom: state.viewportZoom ?? 1,
        });
      }

      // Claimed devices are rebuilt in Bridge.ts onGraphUpdate — no echo needed.
    });


    Bridge.onPaxList((paxItems) => {
      _paxInfoMap.clear();   // v0.0.958 — the list can now change while running (Pax folder autoscan)
      paxItems.forEach(a => _paxInfoMap.set(a.name, a));
      _paxListLoaded = true;   // v0.0.941
      if (!_paxScanModeSent) { _paxScanModeSent = true; Bridge.setPaxAutoScan(!(loadPrefs().paxManualRescan ?? false)); }   // v0.0.958 — the backend learns the Preference once the UI is up
    });
    Bridge.ready();
    return () => unsubGraph();
  }, []);

  // ── Node / edge change handlers ──────────────────────────────────────────
  const onNodesChange = useCallback((all: NodeChange[]) => {
    // v0.0.930 — changes to the drawn-only wrap box / frame never reach the
    // real nodes: the box moves its wrap (members follow at drag end) and
    // keeps its own selection; everything else about them is ignored.
    const changes: NodeChange[] = [];
    for (const c of all) {
      const id = (c as { id?: string }).id;
      if (id && id.startsWith(RAIL_PREFIX)) {           // Enter-view rails: drawn only — keep their size
        const d = c.type === 'dimensions' ? c.dimensions : undefined;
        if (d && d.width && d.height) {
          const side = id.slice(RAIL_PREFIX.length);
          setRailMeasured(rm => rm[side]?.width === d.width && rm[side]?.height === d.height ? rm : { ...rm, [side]: { width: d.width, height: d.height } });
        }
        continue;
      }
      if (!id || !isWrapViewId(id)) {
        // a folded member is hidden: any size reported for it is meaningless
        if (id && c.type === 'dimensions' && wrapsRef.current.some(p => !p.open && p.nodeIds.includes(id))) continue;
        changes.push(c); continue;
      }
      if (id.startsWith(FRAME_PREFIX)) {
        // Open wrap: dragging the frame's title bar moves its members
        if (c.type !== 'position') continue;
        const wrapId = id.slice(FRAME_PREFIX.length);
        let st = frameDrag.current.get(wrapId);
        if (!st && c.position) {
          const mem = realNodesOfWrap(wrapId);
          const fr  = getNodes().find(n => n.id === id);   // the frame as drawn (its gutters vary)
          if (!fr) continue;
          st = { fx: fr.position.x, fy: fr.position.y,
                 members: new Map(mem.map(n => [n.id, { x: n.position.x, y: n.position.y }])) };
          frameDrag.current.set(wrapId, st);
        }
        if (!st) continue;
        const s0 = st;
        if (c.position) {
          const dx = c.position.x - s0.fx, dy = c.position.y - s0.fy;
          setNodes(ns => ns.map(n => { const m = s0.members.get(n.id);
            return m ? { ...n, position: { x: m.x + dx, y: m.y + dy } } : n; }));
          (s0 as any).dx = dx; (s0 as any).dy = dy;
        }
        if (c.dragging === false) {
          frameDrag.current.delete(wrapId);
          const dx = (s0 as any).dx ?? 0, dy = (s0 as any).dy ?? 0;
          if (dx || dy) s0.members.forEach((m, nid) => Bridge.moveNode(nid, m.x + dx, m.y + dy));
        }
        continue;
      }
      const wrapId = id.slice(WRAP_PREFIX.length);
      if (c.type === 'dimensions') {
        const d = c.dimensions;
        if (d && d.width && d.height)
          setWrapMeasured(pm => pm[wrapId]?.width === d.width && pm[wrapId]?.height === d.height
            ? pm : { ...pm, [wrapId]: { width: d.width, height: d.height } });
      } else if (c.type === 'select') {
        setSelectedWraps(sp => {
          if (sp.has(wrapId) === c.selected) return sp;
          const n = new Set(sp); if (c.selected) n.add(wrapId); else n.delete(wrapId); return n;
        });
      } else if (c.type === 'position') {
        const p = wrapsRef.current.find(q => q.id === wrapId);
        if (!p) continue;
        if (c.position) {
          if (!wrapDragStart.current.has(wrapId)) wrapDragStart.current.set(wrapId, { x: p.x, y: p.y });
          const { x, y } = c.position;
          setWraps(ps => ps.map(q => q.id === wrapId ? { ...q, x, y } : q));
        }
        if (c.dragging === false) {
          const start = wrapDragStart.current.get(wrapId);
          wrapDragStart.current.delete(wrapId);
          const cur = wrapsRef.current.find(q => q.id === wrapId)!;
          const dx = start ? cur.x - start.x : 0, dy = start ? cur.y - start.y : 0;
          if (dx || dy) {
            const moved = realNodesOfWrap(wrapId).map(n => ({ id: n.id, x: n.position.x + dx, y: n.position.y + dy }));
            const byId  = new Map(moved.map(m => [m.id, m]));
            setNodes(ns => ns.map(n => { const m = byId.get(n.id); return m ? { ...n, position: { x: m.x, y: m.y } } : n; }));
            moved.forEach(m => Bridge.moveNode(m.id, m.x, m.y));
          }
          Bridge.wrapMove(wrapId, cur.x, cur.y);
        }
      }
    }
    if (changes.length === 0) return;
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
  }, [updateNode, realNodesOfWrap, setWraps, getNodes]);

  const onEdgesChange = useCallback((all: EdgeChange[]) => {
    // v0.0.934 — "<id>~in…" = the inside half of a wire crossing an open
    // wrap's frame: drawn only, never selected / removed on its own
    const changes = all.filter(c => !('id' in c && String((c as any).id).includes('~in')));
    if (changes.length === 0) return;
    setEdges(es => applyEdgeChanges(changes, es));
    for (const c of changes) {
      if (c.type === 'remove')
        Bridge.removeConnection(c.id);
    }
  }, []);


  // v0.0.934 — keyboard delete (⌫ / Delete) done like ☰ → Edit → Delete:
  // ReactFlow's own removal is cancelled and the backend does it — a node
  // goes with its wires (it used to come straight back: only its wires were
  // removed, each as its own undo step), a wire on its own as before. The
  // graph push that follows redraws everything. Wrap boxes / frames and the
  // drawn-only stubs are never deleted this way.
  const onBeforeDelete = useCallback(async ({ nodes: dn, edges: de }: { nodes: Node[]; edges: Edge[] }) => {
    const nodeIds = new Set(dn.map(n => n.id).filter(id => !isWrapViewId(id)));
    nodeIds.forEach(id => Bridge.removeNode(id));
    de.filter(e => !String(e.id).includes('~') && !nodeIds.has(e.source) && !nodeIds.has(e.target))
      .forEach(e => Bridge.removeConnection(e.id));
    return false;
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
      if (isWrapViewId(n.id)) return;   // v0.0.930 — drawn-only wrap box / frame
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
    (oldEdge: Edge, viewConnection: Connection) => {
      const newConnection = toRealConnection(viewConnection);   // v0.0.930 — wrap box ports
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


  const onConnect = useCallback((viewConnection: Connection) => {
    if (!isValidConnection(viewConnection)) return;
    const connection = toRealConnection(viewConnection);   // v0.0.930 — wrap box ports
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

    // v0.0.938 — a wrap from the library: placed folded at the drop point
    // (not inside the Enter view: wraps are one level)
    try {
      const d = JSON.parse(raw);
      if (d && typeof d.wrapFile === 'string') {
        if (enteredRef.current) return;
        const p = screenToFlowPosition({ x: e.clientX, y: e.clientY });
        Bridge.wrapLibraryLoad(d.wrapFile, p.x, p.y);
        return;
      }
    } catch {}

    // Data is JSON { nodeType, paxName, paxType? }
    // For Pax: nodeType=0 (sentinel), paxType=1/2/3/4 (PAX MIDI/Audio/AV/Value)
    // For built-ins: nodeType=1-4, paxName=''
    let nodeType: number = 1;
    let paxName = '';
    let paxType: number = 0;
    try {
      const parsed = JSON.parse(raw);
      nodeType   = parsed.nodeType   as number;
      paxName = parsed.paxName ?? '';
      paxType    = parsed.paxType    ?? 0;
    } catch {
      nodeType = parseInt(raw, 10);
    }

    // Send paxType as nodeType to C++ for Pax so ports are correct,
    // but offset by 100 to guarantee no collision with built-in types 1-4.
    // C++ checks paxName first, so the actual value only matters for port layout.
    const cppNodeType = paxName ? (100 + paxType) : nodeType;

    const position = screenToFlowPosition({ x: e.clientX, y: e.clientY });
    pendingDrop.current = { dropX: position.x, dropY: position.y };
    Bridge.addNode(cppNodeType, position.x, position.y, paxName, enteredRef.current ?? '');
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
      pendingFragment.connections,
      // v0.0.938 — whole wraps in the fragment come back as wraps
      (pendingFragment.wraps ?? []).map(w => ({ ...w, x: flowPos.x + w.x, y: flowPos.y + w.y })),
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
        nodes={view.nodes}
        edges={view.edges}
        nodeTypes={nodeTypes}
        edgeTypes={edgeTypes}
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
        deleteKeyCode={['Delete', 'Backspace']}   /* v0.0.934 — a MacBook's ⌫ sends Backspace */
        onBeforeDelete={onBeforeDelete}
        style={{ background: 'var(--bg)' }}
        connectionLineStyle={{ stroke: connectionLineColour, strokeWidth: 2, strokeDasharray: '6 3' }}
        defaultEdgeOptions={{ style: { strokeWidth: 2 } }}
        onDragOver={onDragOver}
        onDrop={onDrop}
        onMoveEnd={(_, vp) => { if (!enteredRef.current) Bridge.setViewport(vp.x, vp.y, vp.zoom); }}
        onSelectionChange={onSelectionChange}
        onNodeDragStart={onNodeDragStart}
        onNodeDrag={onNodeDrag}
        onNodeDragStop={onNodeDragStop}

      >
        <Background variant={BackgroundVariant.Dots} gap={24} size={1.2} color="var(--border)" />
        <Controls style={{ bottom: 16, right: 16, left: 'auto' }} />

        {/* v0.0.923 — launch dialogs: last project missing / first-launch question */}
        {(fileMissing || askReopen) && (
          <div style={{
            position: 'fixed', inset: 0, zIndex: 1000,
            background: 'rgba(0,0,0,0.55)',
            display: 'flex', alignItems: 'center', justifyContent: 'center',
          }}>
            <div style={{
              width: 380, maxWidth: '90%', padding: '18px 20px',
              background: 'var(--surface2)', border: '1px solid var(--border)',
              borderRadius: 'var(--radius)', boxShadow: '0 12px 40px rgba(0,0,0,.6)',
              color: 'var(--text)', fontSize: 12,
            }}>
              {fileMissing ? (<>
                <div style={{ fontWeight: 600, marginBottom: 8 }}>
                  {fileMissing.context === 'launch' ? 'Last project not found' : 'Project not found'}
                </div>
                <div style={{ color: 'var(--text-muted)', marginBottom: 6 }}>
                  {fileMissing.context === 'launch'
                    ? 'Patchy couldn\'t find the project it was asked to reopen:'
                    : 'This recent project couldn\'t be found (it was removed from the list):'}
                </div>
                <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, wordBreak: 'break-all', marginBottom: 16 }}>
                  {fileMissing.path}
                </div>
                <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 8 }}>
                  <button className="nodrag" onClick={() => setFileMissing(null)} style={dialogBtn(false)}>
                    {fileMissing.context === 'launch' ? 'Start empty' : 'Cancel'}
                  </button>
                  <button className="nodrag" onClick={() => { Bridge.locateFile(fileMissing.path); setFileMissing(null); }} style={dialogBtn(true)}>
                    Locate…
                  </button>
                </div>
              </>) : (<>
                <div style={{ fontWeight: 600, marginBottom: 8 }}>Reopen your last project at launch?</div>
                <div style={{ color: 'var(--text-muted)', marginBottom: 16 }}>
                  Patchy can reopen the project you last worked on each time it starts.
                  You can change this later in Preferences.
                </div>
                <div style={{ display: 'flex', justifyContent: 'flex-end', gap: 8 }}>
                  <button className="nodrag" onClick={() => { Bridge.setReopenLastProject(false); setAskReopen(false); }} style={dialogBtn(false)}>No</button>
                  <button className="nodrag" onClick={() => { Bridge.setReopenLastProject(true);  setAskReopen(false); }} style={dialogBtn(true)}>Yes</button>
                </div>
              </>)}
            </div>
          </div>
        )}

        {/* v0.0.920 — plugin window resize grip (see onGripDown). */}
        {!isStandalone && (
          <div
            onPointerDown={onGripDown}
            onPointerMove={onGripMove}
            onPointerUp={onGripUp}
            onPointerCancel={onGripUp}
            title="Drag to resize"
            style={{
              position: 'absolute', right: 0, bottom: 0, width: 16, height: 16, zIndex: 30,
              cursor: 'nwse-resize', touchAction: 'none',
            }}
          >
            <svg width="16" height="16" viewBox="0 0 16 16" style={{ display: 'block' }}>
              <path d="M15 5 L5 15 M15 9 L9 15 M15 13 L13 15" stroke="var(--text-muted)" strokeWidth="1.2" strokeLinecap="round" />
            </svg>
          </div>
        )}

        {/* v0.0.937 — Enter view breadcrumb: "Main ‹ WRAP" (⌘↓ / Esc) */}
        {entered && (() => {
          const w = wraps.find(q => q.id === entered);
          return (
            <div style={{ position: 'absolute', top: 12, left: 16, zIndex: 10, display: 'flex', alignItems: 'center', gap: 8,
                          padding: '5px 10px', borderRadius: 'var(--radius)', background: 'var(--surface2)',
                          border: '1px solid var(--border-hi)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
                          boxShadow: '0 4px 16px rgba(0,0,0,.4)', userSelect: 'none' }}>
              <span onClick={exitWrap} style={{ cursor: 'pointer', color: 'var(--accent)' }}
                onMouseEnter={() => setHint({ title: 'Back to the main graph', body: 'Leaves this wrap (⌘↓ or Esc). The wrap stays folded or open as it was.' })}
                onMouseLeave={() => setHint(null)}>Main</span>
              <span style={{ color: 'var(--text-muted)' }}>‹</span>
              <span style={{ color: 'var(--text)', fontWeight: 700, letterSpacing: '0.08em', textTransform: 'uppercase' }}>{w?.name || 'WRAP'}</span>
              <span style={{ color: 'var(--text-muted)', fontSize: 9 }}>{w?.nodeIds.length ?? 0} nodes · ⌘↓ back</span>
            </div>
          );
        })()}

        {/* v0.0.919 — shown while the host (e.g. Logic) has stopped calling
            Patchy's audio engine and Patchy drives its graph itself. Fades
            in/out; never intercepts clicks. */}
        <div style={{
          position: 'absolute', bottom: 16, left: 16, zIndex: 10,
          display: 'flex', alignItems: 'center', gap: 6,
          padding: '4px 9px', borderRadius: 4,
          background: 'var(--surface)', border: '1px solid var(--border)',
          fontSize: 10, color: 'var(--text-muted)', letterSpacing: '0.02em',
          pointerEvents: 'none', userSelect: 'none',
          opacity: hostPaused ? 0.9 : 0, transition: 'opacity 0.4s ease',
        }}>
          <span style={{ width: 6, height: 6, borderRadius: '50%', background: '#f59e0b', flexShrink: 0 }} />
          Host paused — Patchy running independently
        </div>

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
                    {/* v0.0.923 — Open Recent (opens further left, like the File submenu) */}
                    <div style={{ position: 'relative' }}
                         onMouseEnter={() => setRecentOpen(true)}
                         onMouseLeave={() => setRecentOpen(false)}>
                      <MenuRow
                        label="Open Recent"
                        enabled={recentFiles.length > 0}
                        trailing={<ChevronLeft size={12} style={{ color: 'var(--text-muted)' }} />}
                      />
                      {recentOpen && recentFiles.length > 0 && (
                        <div
                          onMouseDown={e => e.stopPropagation()}
                          style={{
                            position: 'absolute', top: -4, right: '100%',
                            background: 'var(--surface2)', border: '1px solid var(--border)',
                            borderRadius: 'var(--radius)', padding: '4px 0',
                            boxShadow: '0 8px 32px rgba(0,0,0,.6)',
                            minWidth: 220, maxWidth: 360, zIndex: 101,
                          }}>
                          {recentFiles.map(f => (
                            <MenuRow
                              key={f.path}
                              label={f.name}
                              trailing={<span style={{ color: 'var(--text-muted)', fontSize: 10 }}>{f.folder}</span>}
                              onClick={() => { Bridge.openRecentFile(f.path); setShowFileMenu(false); }}
                            />
                          ))}
                          <MenuDivider />
                          <MenuRow label="Clear Recent" onClick={() => { Bridge.clearRecentFiles(); setShowFileMenu(false); }} />
                        </div>
                      )}
                    </div>
                    <MenuRow label={fileState.hasFile ? 'Save' : 'Save…'} shortcut="⌘S" onClick={() => { Bridge.fileSave(); setShowFileMenu(false); }} />
                    <MenuRow label="Save As…" shortcut="⌘⇧S" onClick={() => { Bridge.fileSaveAs(); setShowFileMenu(false); }} />
                    <MenuDivider />
                    {(() => {
                      // v0.0.930 — a selected folded wrap exports its nodes
                      const wrapped        = new Set(wrapsRef.current.filter(p => !p.open && selectedWraps.has(p.id)).flatMap(p => p.nodeIds));
                      const selectedNodes = nodesRef.current.filter(n => (n.selected && !wrapsRef.current.some(p => !p.open && p.nodeIds.includes(n.id))) || wrapped.has(n.id));
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
                    <MenuRow label="Wrap"   shortcut="⌘G"  enabled={canWrap()}   onClick={() => { wrapSelection();   setShowFileMenu(false); }} />
                    <MenuRow label="Unwrap" shortcut="⌘⇧G" enabled={canUnwrap()} onClick={() => { unwrapSelection(); setShowFileMenu(false); }} />
                    <MenuDivider />
                    {(() => {
                      const selectedNodes = getNodes().filter(n => n.selected && !isWrapViewId(n.id));
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
          {/* v0.0.948 — Panic (also ⌘.): MIDI all notes / sound off; with the
              Preferences options on it latches (DMX / ArtNet blackout, audio
              muted) — lit red until pressed again */}
          <button
            onClick={() => Bridge.panic()}
            onMouseEnter={() => setHint({ title: panic.latched ? 'Panic — engaged' : 'Panic (⌘.)',
              body: panic.latched
                ? `Click to release: ${[panic.blackout && 'DMX / ArtNet lights back', panic.mute && 'audio back'].filter(Boolean).join(', ') || 'back to normal'}.`
                : `Sends All Notes Off, All Sound Off and Sustain off on all 16 channels to every MIDI Out${panic.blackout || panic.mute ? `, and holds ${[panic.blackout && 'a DMX / ArtNet blackout', panic.mute && 'all audio muted'].filter(Boolean).join(' and ')} until clicked again` : ''}. Options: Preferences → Panic.` })}
            onMouseLeave={() => setHint(null)}
            style={{
              background:   panic.latched ? '#ef4444' : 'transparent',
              border:       `1px solid ${panic.latched ? '#ef4444' : 'var(--border)'}`,
              borderRadius: 'var(--radius)',
              color:        panic.latched ? '#fff' : '#f59e0b',
              cursor:       'pointer',
              width:        26, height: 26,
              display:      'flex', alignItems: 'center', justifyContent: 'center',
              animation:    panic.latched ? 'patchyLearnPulse 1s ease-in-out infinite' : 'none',
              transition:   'background 0.15s, color 0.15s',
            }}
          >
            <span style={{ fontSize: 15, lineHeight: 1 }}>⚠</span>
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
        <div style={{ display: 'flex', width: '100%', height: '100%', position: 'relative' }}>   {/* relative: v0.0.921 floating sidebar */}
          <ReactFlowProvider>
      <Sidebar />
            <FlowCanvas />
          </ReactFlowProvider>
          <Toasts />   {/* v0.0.958 */}
        </div>
      </HintProvider>
    </DawContext.Provider>
  );
}
