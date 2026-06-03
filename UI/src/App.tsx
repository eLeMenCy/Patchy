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

import { Bridge, FileState, AudioSettings, GraphState, RawNode, RawConnection, PortActivityEntry, AddonParamInfo, FragmentData, UndoState } from './Bridge';
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
const nodeTypes = { custom: GenericNode, midiMonitor: MidiMonitorNode, audioMonitor: AudioMonitorNode, midiKeyboard: MidiKeyboardNode, spectrumyser: SpectrumyserNode, envelope: EnvelopeNode };

// ── Conversion helpers ────────────────────────────────────────────────────────
// Module-level addon params map — populated when addon list arrives
const _addonParamsMap = new Map<string, AddonParamInfo[]>();

function rawToFlowNode(raw: RawNode, addonParamsMap?: Map<string, AddonParamInfo[]>): Node<NodeData | MidiMonitorNodeData> {
  const isMidiMonitor  = raw.nodeType === 5;
  const isAudioMonitor = raw.nodeType === 6;
  const isMidiKeyboard = raw.nodeType === 7;
  return {
    id:       raw.id,
    type:     isMidiMonitor ? 'midiMonitor' : isAudioMonitor ? 'audioMonitor' : isMidiKeyboard ? 'midiKeyboard'
            : raw.addonName === 'Spectrumyser' ? 'spectrumyser'
            : raw.addonName === 'Envelope'     ? 'envelope' : 'custom',
    position: { x: raw.x, y: raw.y },
    data: isMidiMonitor
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
  const styleRef      = useRef<HTMLStyleElement | null>(null);
  const midiTimers    = useRef<Map<string, number>>(new Map());
  const audioLevels   = useRef<Map<string, number>>(new Map());
  const portRmsLevels = useRef<Map<string, number[]>>(new Map());
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
        if (entry.midi > 0) midiTimers.current.set(entry.id, now + 80);
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
      const audioSources = new Set<string>(audioEdges.map(e => e.source));
      audioLevels.current.forEach((rms, id) => { if (rms > 0.01) audioSources.add(id); });
      const midiSources  = new Set<string>(midiEdges.map(e => e.source));
      const allSources   = new Set([...audioSources, ...midiSources]);

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

        // MIDI flash — written last so it wins over audio VU
        if (isMidiFlash && nodeMidiEdges.length > 0) {
          const fc = '#B2EBF2';
          css += `[data-handleid="${id}_MIDI Out_out"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
          nodeMidiEdges.forEach(e => {
            css += `g.react-flow__edge[data-id="${e.id}"] path.react-flow__edge-path{stroke:${fc}!important;filter:drop-shadow(0 0 4px ${fc});transition:none}`;
            if (e.targetHandle) css += `[data-handleid="${e.targetHandle}"]{background:${fc}!important;box-shadow:0 0 10px ${fc}!important;transition:none}`;
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
  const { screenToFlowPosition, setViewport, updateNode, getNodes } = useReactFlow();
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
  const [pendingFragment, setPendingFragment] = useState<FragmentData | null>(null);
  const ghostPos = useRef<{ x: number; y: number }>({ x: 0, y: 0 });
  const ghostRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    return Bridge.onFileState(setFileState);
  }, []);

  useEffect(() => {
    return Bridge.onUndoState(setUndoState);
  }, []);

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
    if (!showFileMenu) return;
    const close = (e: MouseEvent) => {
      if (burgerBtnRef.current?.contains(e.target as Element)) return;
      setShowFileMenu(false);
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
          _addonParamsMap
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
              ref={burgerBtnRef}
              onMouseDown={e => { e.stopPropagation(); setShowPrefs(false); setShowFileMenu(v => !v); }}
              onClick={e => e.stopPropagation()}
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
                {/* ── Undo / Redo section ── */}
                <div style={{ borderTop: '1px solid var(--border)', margin: '4px 0' }} />
                {[
                  { label: 'Undo', shortcut: '⌘Z',   enabled: undoState.canUndo, action: () => { Bridge.undo(); setShowFileMenu(false); } },
                  { label: 'Redo', shortcut: '⌘⇧Z', enabled: undoState.canRedo, action: () => { Bridge.redo(); setShowFileMenu(false); } },
                ].map(item => (
                  <div key={item.label} onClick={item.enabled ? item.action : undefined}
                    style={{ padding: '6px 14px', fontSize: 11,
                             color: item.enabled ? 'var(--text)' : 'var(--text-muted)',
                             cursor: item.enabled ? 'pointer' : 'default',
                             fontFamily: "'JetBrains Mono', monospace",
                             display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                             gap: 24, opacity: item.enabled ? 1 : 0.45 }}
                    onMouseEnter={e => { if (item.enabled) e.currentTarget.style.background = 'var(--surface)'; }}
                    onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
                  >
                    <span>{item.label}</span>
                    <span style={{ color: 'var(--text-muted)', fontSize: 10 }}>{item.shortcut}</span>
                  </div>
                ))}
                {/* ── Fragment section ── */}
                <div style={{ borderTop: '1px solid var(--border)', margin: '4px 0' }} />
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
                      const d = n.data as { nodeType?: number; addonName?: string; label?: string };
                      return d.addonName || NODE_LABELS[d.nodeType ?? 0] || d.label || 'Node';
                    });
                    const unique    = [...new Set(names)];
                    const suggested = unique.slice(0, 3).join('_') + (unique.length > 3 ? '_etc' : '');
                    Bridge.exportSelection(ids, suggested);
                    setShowFileMenu(false);
                  };
                  return (
                    <>
                      <div onClick={handleExport}
                        style={{ padding: '6px 14px', fontSize: 11,
                                 color: hasSelection ? 'var(--text)' : 'var(--text-muted)',
                                 cursor: hasSelection ? 'pointer' : 'default',
                                 fontFamily: "'JetBrains Mono', monospace",
                                 display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                                 gap: 24, opacity: hasSelection ? 1 : 0.45 }}
                        onMouseEnter={e => { if (hasSelection) e.currentTarget.style.background = 'var(--surface)'; }}
                        onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
                      >
                        <span>Export…</span>
                        <span style={{ color: 'var(--text-muted)', fontSize: 10 }}>
                          {hasSelection ? `${selectedNodes.length} node${selectedNodes.length !== 1 ? 's' : ''}` : 'select nodes'}
                        </span>
                      </div>
                      <div onClick={() => { Bridge.importFragment(); setShowFileMenu(false); }}
                        style={{ padding: '6px 14px', fontSize: 11, color: 'var(--text)',
                                 cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
                                 display: 'flex', justifyContent: 'space-between', alignItems: 'center',
                                 gap: 24 }}
                        onMouseEnter={e => (e.currentTarget.style.background = 'var(--surface)')}
                        onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
                      >
                        <span>Import…</span>
                      </div>
                    </>
                  );
                })()}
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
