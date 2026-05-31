import { useCallback, useEffect, useRef, useState, useContext, DragEvent } from 'react';
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

import { Bridge, FileState, AudioSettings, GraphState, RawNode, RawConnection, PortActivityEntry, AddonParamInfo } from './Bridge';
import GenericNode, { NodeData } from './GenericNode';
import MidiMonitorNode,      { MidiMonitorNodeData }      from './MidiMonitorNode';
import AudioMonitorNode,   { AudioMonitorNodeData }   from './AudioMonitorNode';
import MidiKeyboardNode,  { MidiKeyboardNodeData }  from './MidiKeyboardNode';
import PreferencesPanel, { GraphPreferences, loadPrefs, savePrefs } from './PreferencesPanel';
import { HintProvider, HintContext, BUTTON_HINTS, PORT_HINTS, EDGE_HINTS } from './HintPanel';
import { Menu, ChevronsDownUp, ChevronsUpDown, Settings } from 'lucide-react';
import Sidebar from './Sidebar';
import SpectrumyserNode from './SpectrumyserNode';
import EnvelopeNode     from './EnvelopeNode';

// ── Node type registry ────────────────────────────────────────────────────────
const nodeTypes = { custom: GenericNode, monitor: MidiMonitorNode, audioMonitor: AudioMonitorNode, midiKeyboard: MidiKeyboardNode, spectrumyser: SpectrumyserNode, envelope: EnvelopeNode };

// ── Conversion helpers ────────────────────────────────────────────────────────
// Module-level addon params map — populated when addon list arrives
const _addonParamsMap = new Map<string, AddonParamInfo[]>();

function rawToFlowNode(raw: RawNode, addonParamsMap?: Map<string, AddonParamInfo[]>): Node<NodeData | MidiMonitorNodeData> {
  const isMonitor      = raw.nodeType === 5;
  const isAudioMonitor = raw.nodeType === 6;
  const isMidiKeyboard = raw.nodeType === 7;
  return {
    id:       raw.id,
    type:     isMonitor ? 'monitor' : isAudioMonitor ? 'audioMonitor' : isMidiKeyboard ? 'midiKeyboard'
            : raw.addonName === 'Spectrumyser' ? 'spectrumyser'
            : raw.addonName === 'Envelope'     ? 'envelope' : 'custom',
    position: { x: raw.x, y: raw.y },
    data: isMonitor
      ? { label: raw.label, nodeType: 5, ports: raw.ports, settingsJson: raw.settingsJson } as MidiMonitorNodeData
      : isAudioMonitor
      ? { label: raw.label, nodeType: 6, ports: raw.ports, settingsJson: raw.settingsJson } as AudioMonitorNodeData
      : isMidiKeyboard
      ? { label: raw.label, nodeType: 7, ports: raw.ports, settingsJson: raw.settingsJson } as MidiKeyboardNodeData
      : { label: raw.label, nodeType: raw.nodeType,
          ports: raw.ports, selectedDeviceId: raw.selectedDeviceId,
          addonName: raw.addonName,
          addonParams: raw.addonName ? (addonParamsMap?.get(raw.addonName) ?? []) : [],
          settingsJson: raw.settingsJson } as NodeData,
  };
}

function rawToFlowEdge(raw: RawConnection): Edge {
  const isMidi  = raw.sourcePortId.toLowerCase().includes('midi');
  const isAudio = raw.sourcePortId.toLowerCase().includes('audio');
  const cls     = isMidi && isAudio ? 'edge-mixed'
                : isMidi            ? 'edge-midi'
                :                     'edge-audio';
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
function usePortActivityStyles (edges: any[], nodes: any[]) {
  const styleRef = useRef<HTMLStyleElement | null>(null);
  const midiTimers = useRef<Map<string, number>>(new Map());
  const audioLevels    = useRef<Map<string, number>>(new Map());
  const portRmsLevels  = useRef<Map<string, number[]>>(new Map());
  const edgeList = useRef<typeof edges>(edges);
  const nodesRef = useRef<any[]>(nodes);
  useEffect(() => { edgeList.current = edges; }, [edges]);
  useEffect(() => { nodesRef.current = nodes;  }, [nodes]);

  useEffect(() => {
    // Create a single <style> tag we'll update at 30fps
    const el = document.createElement('style');
    el.id = 'port-activity-styles';
    document.head.appendChild(el);
    styleRef.current = el;
    return () => el.remove();
  }, []);

  useEffect(() => {
    const unsub = Bridge.onPortActivity((entries: PortActivityEntry[]) => {
      const now = Date.now();
      entries.forEach(entry => {
        if (entry.midi > 0)
          midiTimers.current.set(entry.id, now + 80);
        const rawRms = Math.max(entry.l, entry.r) / 1000;
        const scaledRms = Math.min(rawRms * 4, 1.0);
        const prev = audioLevels.current.get(entry.id) ?? 0;
        audioLevels.current.set(entry.id, Math.max(scaledRms, prev * 0.88));
        // Store per-port RMS for multi-port nodes
        if (entry.portRms && entry.portRms.length > 1) {
          const prevPorts = portRmsLevels.current.get(entry.id) ?? [];
          const newPorts = entry.portRms.map((v, i) => {
            const scaled = Math.min((v / 1000) * 4, 1.0);
            return Math.max(scaled, (prevPorts[i] ?? 0) * 0.88);
          });
          portRmsLevels.current.set(entry.id, newPorts);
        } else {
          portRmsLevels.current.delete(entry.id);
        }
      });
    });
    return unsub;
  }, []);

  // Separate RAF loop — always renders ALL edges every frame
  useEffect(() => {
    let rafId: number;
    const render = () => {
      const style = styleRef.current;
      if (!style) { rafId = requestAnimationFrame(render); return; }
      const now = Date.now();
      // Decay audio levels each frame
      audioLevels.current.forEach((v: number, k: string) => {
        audioLevels.current.set(k, v * 0.97);
      });
      portRmsLevels.current.forEach((ports: number[], k: string) => {
        portRmsLevels.current.set(k, ports.map((v: number) => v * 0.97));
      });
      const allAudioEdges: any[] = edgeList.current.filter((e: any) =>
        !!(e.sourceHandle ?? '').toLowerCase().includes('audio'));
      const allMidiEdges: any[]  = edgeList.current.filter((e: any) =>
        !!(e.sourceHandle ?? '').toLowerCase().includes('midi'));
      // Collect unique source node IDs — include ALL nodes with audio activity,
      // not just ones with edges (so unconnected AudioIN shows VU)
      const audioSources: string[] = [...new Set(allAudioEdges.map((e: any) => e.source as string))];
      const midiSources: string[]  = [...new Set(allMidiEdges.map((e: any)  => e.source as string))];
      // Add any node with audio activity even if it has no outgoing edges
      audioLevels.current.forEach((rms: number, id: string) => {
        if ((rms as number) > 0.01 && !audioSources.includes(id as string))
          audioSources.push(id as string);
      });
      const entries: { id: string }[] = [
        ...audioSources.map((id: string) => ({ id })),
        ...midiSources.filter((id: string) => !audioSources.includes(id))
          .map((id: string) => ({ id })),
      ];
      let css = '';

      entries.forEach(entry => {
        const isMidiFlash = (midiTimers.current.get(entry.id) ?? 0) > now;
        const audioRms    = audioLevels.current.get(entry.id) ?? 0;
        // Separate outgoing edges by port type
        const midiEdges  = edgeList.current.filter(e => e.source === entry.id &&
                            !!(e.sourceHandle ?? '').toLowerCase().includes('midi'));
        const audioEdges = edgeList.current.filter(e => e.source === entry.id &&
                            !!(e.sourceHandle ?? '').toLowerCase().includes('audio'));
        const hasAudioOut = audioEdges.length > 0;
        const hasMidiOut  = midiEdges.length > 0;

        // ── Audio VU — always applied for all audio nodes ──────────────────
        {
          const col  = rmsToColour(audioRms);
          const glow = rmsToGlow(audioRms, col);

          // Colour ALL audio output port dots for this node
          const nodeData = nodesRef.current.find((n: any) => n.id === entry.id);
          const audioPorts: string[] = nodeData
            ? (nodeData.data?.ports as any[] ?? [])
                .filter((p: any) => p.direction === 'output' && p.type === 'audio')
                .map((p: any) => p.label as string)
            : ['Audio Out'];

          const portRmsList = portRmsLevels.current.get(entry.id);

          audioPorts.forEach((portLabel: string, portIdx: number) => {
            const portRms = portRmsList ? (portRmsList[portIdx] ?? 0) : audioRms;
            if (portRms > 0.01 || audioEdges.some(e => (e.sourceHandle ?? '').includes(portLabel))) {
              const pCol  = rmsToColour(portRms);
              const pGlow = rmsToGlow(portRms, pCol);
              const hid = `${entry.id}_${portLabel}_out`;
              css += `[data-handleid="${hid}"] {
  background: ${pCol} !important;
  box-shadow: ${pGlow} !important;
}`;
            }
          });

          // Also colour connected audio OUT handles (catches any missed above)
          const audioOutHandles = [...new Set(audioEdges.map(e => e.sourceHandle ?? ''))];
          audioOutHandles.forEach(hid => {
            if (hid) css += `[data-handleid="${hid}"] {
  background: ${col} !important;
  box-shadow: ${glow} !important;
}`;
          });

          // Audio edges — colour edge path AND target IN handle
          audioEdges.forEach(e => {
            const tgtId = e.targetHandle ?? '';
            css += `
g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path {
  stroke: ${col} !important;
  filter: drop-shadow(0 0 3px ${col});
}
[data-handleid="${tgtId}"] {
  background: ${col} !important;
  box-shadow: ${glow} !important;
}`;
          });
        }

        // ── MIDI flash — written LAST so it wins over audio VU ─────────
        // Only flash if this node actually has a MIDI OUT handle or MIDI edges
        if (isMidiFlash && hasMidiOut) {
          const flashCol = '#B2EBF2';
          const midiOutHandleId = `${entry.id}_MIDI Out_out`;
          // Flash MIDI OUT dot
          css += `[data-handleid="${midiOutHandleId}"] {
  background: ${flashCol} !important;
  box-shadow: 0 0 10px ${flashCol} !important;
  transition: none;
}`;
          // Flash MIDI outgoing edges AND target IN handles
          midiEdges.forEach(e => {
            const tgtId = e.targetHandle ?? '';
            css += `
g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path {
  stroke: ${flashCol} !important;
  filter: drop-shadow(0 0 4px ${flashCol});
  transition: none;
}
[data-handleid="${tgtId}"] {
  background: ${flashCol} !important;
  box-shadow: 0 0 10px ${flashCol} !important;
  transition: none;
}`;
          });
        }
      });

      style.textContent = css;
      rafId = requestAnimationFrame(render);
    };
    rafId = requestAnimationFrame(render);
    return () => cancelAnimationFrame(rafId);
  }, []);
}

// ── Inner component (needs useReactFlow hook) ─────────────────────────────────
function FlowCanvas() {
  const { isStandalone } = useContext(DawContext);
  const [nodes, setNodes] = useState<Node<any>[]>([]);
  const [edges, setEdges] = useState<Edge[]>([]);
  const wrapperRef = useRef<HTMLDivElement>(null);
  const { setHint } = useContext(HintContext);
  const { screenToFlowPosition, setViewport, updateNode, getNodes, getViewport } = useReactFlow();
  const pendingDrop   = useRef<{ dropX: number; dropY: number } | null>(null);
  const knownNodeIds  = useRef<Set<string>>(new Set());
  const updateNodeInternals = useUpdateNodeInternals();
  const [prefs, setPrefs]       = useState<GraphPreferences>(loadPrefs);
  const [showPrefs, setShowPrefs] = useState(false);
  const [allCollapsed, setAllCollapsed] = useState(false);
  const [fileState, setFileState] = useState<FileState>({ fileName: 'Untitled', hasFile: false });
  const [audioSettings, setAudioSettings] = useState<AudioSettings | null>(null);
  const [showFileMenu, setShowFileMenu] = useState(false);

  useEffect(() => {
    return Bridge.onFileState(setFileState);
  }, []);

  useEffect(() => {
    return Bridge.onAudioSettings(setAudioSettings);
  }, []);

  useEffect(() => {
    if (!showFileMenu) return;
    const close = () => setShowFileMenu(false);
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
        if (cls.includes('edge-midi'))  setHint(EDGE_HINTS['midi']);
        else if (cls.includes('edge-audio')) setHint(EDGE_HINTS['audio']);
        else if (cls.includes('edge-mixed')) setHint(EDGE_HINTS['av']);
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
    Bridge.onGraphUpdate((state: GraphState) => {
      flushSync(() => {
        setNodes(prev => {
          const styleMap = new Map(prev.map(n => [n.id, n.style]));
          return state.nodes.map(raw => {
            const node = rawToFlowNode(raw, _addonParamsMap);
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

      // Repopulate claimed devices from restored graph state
      state.nodes.forEach(n => {
        if ((n.nodeType === 1 || n.nodeType === 2) && n.selectedDeviceId)
          Bridge.setNodeParam(n.id, 'midiDeviceId', n.selectedDeviceId, n.nodeType);
        if ((n.nodeType === 3 || n.nodeType === 4) && n.selectedDeviceId)
          Bridge.setNodeParam(n.id, 'audioDeviceId', n.selectedDeviceId, n.nodeType);
      });
    });


    Bridge.onAddonList((addons) => {
      addons.forEach(a => _addonParamsMap.set(a.name, a.params ?? []));
    });
    Bridge.ready();
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

  // ── Connection validation ─────────────────────────────────────────────────
  const isValidConnection = useCallback((connection: Connection | Edge): boolean => {
    const { sourceHandle, targetHandle } = connection;
    if (!sourceHandle || !targetHandle) return false;
    const srcType  = sourceHandle.includes('Audio') ? 'audio' : 'midi';
    const dstType  = targetHandle.includes('Audio') ? 'audio' : 'midi';
    const srcIsOut = sourceHandle.endsWith('_out');
    const dstIsIn  = targetHandle.endsWith('_in');
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
    setEdges(es => addEdge({
      ...connection,
      className: connection.sourceHandle?.includes('Audio') ? 'edge-audio' : 'edge-midi',
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

    // Data is JSON { nodeType, addonName, ngaType? }
    // For addons: nodeType=0 (sentinel), ngaType=1/2/3 (NGA MIDI/Audio/AV)
    // For built-ins: nodeType=1-4, addonName=''
    let nodeType: number = 1;
    let addonName = '';
    let ngaType: number = 0;
    try {
      const parsed = JSON.parse(raw);
      nodeType   = parsed.nodeType   as number;
      addonName = parsed.addonName ?? '';
      ngaType    = parsed.ngaType    ?? 0;
    } catch {
      nodeType = parseInt(raw, 10);
    }

    // Send ngaType as nodeType to C++ for addons so ports are correct,
    // but offset by 100 to guarantee no collision with built-in types 1-4.
    // C++ checks addonName first, so the actual value only matters for port layout.
    const cppNodeType = addonName ? (100 + ngaType) : nodeType;

    const position = screenToFlowPosition({ x: e.clientX, y: e.clientY });
    pendingDrop.current = { dropX: position.x, dropY: position.y };
    Bridge.addNode(cppNodeType, position.x, position.y, addonName);
  }, [screenToFlowPosition]);

  // ── Render ────────────────────────────────────────────────────────────────
  return (
    <div
      ref={wrapperRef}
      style={{ flex: 1, height: '100%', position: 'relative' }}
      onDragOver={onDragOver}
      onDrop={onDrop}
    >
      <ReactFlow
        nodes={nodes}
        edges={edges}
        nodeTypes={nodeTypes}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        onReconnect={onReconnect}
        onReconnectEnd={onReconnectEnd}
        isValidConnection={isValidConnection}
        defaultViewport={{ x: 0, y: 0, zoom: 1 }}
        minZoom={0.05}
        maxZoom={2}
        deleteKeyCode="Delete"
        style={{ background: 'var(--bg)' }}
        connectionLineStyle={{ stroke: 'var(--accent)', strokeWidth: 2, strokeDasharray: '6 3' }}
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
              onClick={e => { e.stopPropagation(); setShowFileMenu(v => !v); }}
              onMouseEnter={() => setHint({ title: 'File Menu', body: 'New, Open, Save or Save As a patch file (.patchy).' })}
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
                  minWidth: 160, zIndex: 100,
                }}>
                {([
                  { label: 'New',      shortcut: '⌘N',  action: () => { Bridge.fileNew();    setShowFileMenu(false); } },
                  { label: 'Open…',    shortcut: '⌘O',  action: () => { Bridge.fileOpen();   setShowFileMenu(false); } },
                  { label: fileState.hasFile ? 'Save' : 'Save…', shortcut: '⌘S',  action: () => { Bridge.fileSave(); setShowFileMenu(false); } },
                  { label: 'Save As…', shortcut: '⌘⇧S', action: () => { Bridge.fileSaveAs(); setShowFileMenu(false); } },
                ] as {label:string; shortcut:string; action:()=>void}[]).map(item => (
                  <div key={item.label} onClick={item.action}
                    style={{ padding: '6px 14px', fontSize: 11, color: 'var(--text)',
                             cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
                             display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                             gap: 24 }}
                    onMouseEnter={e => (e.currentTarget.style.background = 'var(--surface)')}
                    onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
                  >
                    <span>{item.label}</span>
                    <span style={{ color: 'var(--text-muted)', fontSize: 10 }}>{item.shortcut}</span>
                  </div>
                ))}
                {fileState.hasFile && (
                  <div style={{ padding: '4px 14px 2px', fontSize: 9, color: 'var(--text-muted)',
                                fontFamily: "'JetBrains Mono', monospace",
                                borderTop: '1px solid var(--border)', marginTop: 2 }}>
                    {fileState.fileName}.patchy
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
              onClick={() => setShowPrefs(v => !v)}
             
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
