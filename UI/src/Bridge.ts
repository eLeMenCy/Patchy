/**
 * Bridge.ts
 * Typed façade over the JUCE WebView native integration.
 *
 * JUCE 8 injects `window.__JUCE__.backend` with:
 *   __JUCE__.backend.emitEvent(eventId, payload)  – send JS→C++
 *
 * We expose `window.__bridge.onGraphUpdate(json)` for C++→JS.
 */

export interface GraphState {
  nodes:         RawNode[];
  connections:   RawConnection[];
  viewportX?:    number;
  viewportY?:    number;
  viewportZoom?: number;
}

export interface RawNode {
  id: string;
  label: string;
  nodeType: number;  // 1-4 built-in, higher = Xtension
  paxName?: string;
  settingsJson?: string;   // serialised UI settings blob
  x: number;
  y: number;
  ports: RawPort[];
  selectedDeviceId?: string;
}

export interface RawPort {
  id: string;
  label: string;
  type: 'midi' | 'audio';
  direction: 'input' | 'output';
}

export interface RawConnection {
  id: string;
  sourceNodeId: string;
  sourcePortId: string;
  targetNodeId: string;
  targetPortId: string;
}

// ── Port activity types ───────────────────────────────────────────────────────
export interface PortActivityEntry {
  id:      string;    // nodeId
  midi:    number;    // event count (0 = inactive)
  l:       number;    // audio RMS left  ×1000
  r:       number;    // audio RMS right ×1000
  portRms: number[];  // per-output-port RMS ×1000 for multi-port nodes
  notes:   string;    // "status,note status,note ..." for keyboard nodes
}
type PortActivityCallback = (entries: PortActivityEntry[]) => void;
const _portActivitySubscribers: PortActivityCallback[] = [];

// ── Audio monitor types ───────────────────────────────────────────────────────
export interface AudioSnapshot {
  nodeId: string;
  sr:     number;   // sample rate
  n:      number;   // number of display samples
  l:      string;   // comma-separated integers (value × 1000)
  r:      string;
}
type AudioSnapshotCallback = (snapshots: AudioSnapshot[]) => void;
const _audioSnapshotSubscribers: AudioSnapshotCallback[] = [];

// ── Monitor types ─────────────────────────────────────────────────────────────
export interface RawMidiMonitorEvent {
  ts: number;   // timestamp ms
  sn: string;   // source node id
  sd: string;   // source device name
  st: number;   // status byte
  d1: number;   // data byte 1
  d2: number;   // data byte 2
}
export interface MidiMonitorBatch {
  nodeId: string;
  events: RawMidiMonitorEvent[];
}
type MidiMonitorCallback = (batches: MidiMonitorBatch[]) => void;

type GraphUpdateCallback  = (state: GraphState)   => void;
type MidiDevicesCallback  = (devices: MidiDeviceList) => void;

export interface MidiDeviceInfo  { id: string; name: string; }
export interface MidiDeviceList  {
  midiOutDevices: MidiDeviceInfo[];
  midiInDevices:  MidiDeviceInfo[];
}
export interface AudioDeviceInfo { id: string; name: string; dawHost?: boolean; channelCount?: number; }
export interface AudioDeviceList {
  audioOutDevices: AudioDeviceInfo[];
  audioInDevices:  AudioDeviceInfo[];
}
type AddonListCallback   = (addons: PaxInfo[]) => void;

export interface PaxParamInfo {
  index:        number;
  name:         string;
  min:          number;
  max:          number;
  defaultValue: number;
  step:         number;   // 0=continuous, 1=integer, etc.
}

// ── DAW context (shared via React context, not Bridge) ───────────────────────
// isStandalone and dawLoopbackEnabled are managed in App.tsx via React context

export interface SpectrumBand  { lo: number; hi: number; }
export interface SpectrumSnapshot {
  id:    string;
  sr:    number;
  mags:  number[];   // 64 magnitude bins ×1000
  bands: SpectrumBand[];
}
type SpectrumCallback = (snaps: SpectrumSnapshot[]) => void;
const _spectrumSubscribers: SpectrumCallback[] = [];

export interface AudioSettings {
  sampleRate:           number;
  bufferSize:           number;
  muteFeedback:         boolean;
  availableSampleRates: number[];
  availableBufferSizes: number[];
}

export interface FileState {
  fileName: string;
  hasFile:  boolean;
}

export interface UndoState {
  canUndo: boolean;
  canRedo: boolean;
}

/** A self-contained graph fragment ready to be placed on the canvas.
 *  Node positions are relative to the fragment's own bounding box origin.
 *  All IDs have already been remapped to fresh UUIDs by C++. */
export interface FragmentData {
  nodes:       RawNode[];
  connections: RawConnection[];
  /** Bounding box size of the fragment (flow units) — used for ghost sizing */
  width:  number;
  height: number;
}

export interface PaxInfo {
  name:         string;
  vendor:       string;
  version:      string;
  nodeType:     1 | 2 | 3;
  audioInputs:  number;
  audioOutputs: number;
  midiInputs:   number;
  midiOutputs:  number;
  params:       PaxParamInfo[];
}

// ── Singleton bridge ─────────────────────────────────────────────────────────

const _graphUpdateSubscribers: GraphUpdateCallback[] = [];

const _midiMonitorSubscribers: MidiMonitorCallback[] = [];
const _addonListSubscribers: AddonListCallback[] = [];
type FileStateCallback      = (s: FileState) => void;
type AudioSettingsCallback  = (s: AudioSettings) => void;
type StandaloneModeCallback = (v: boolean) => void;
type FragmentReadyCallback  = (fragment: FragmentData) => void;
type UndoStateCallback      = (s: UndoState) => void;
type NodeSettingsCallback   = (nodeId: string, settingsJson: string) => void;
const _fileStateSubscribers: FileStateCallback[] = [];
const _audioSettingsSubscribers: AudioSettingsCallback[] = [];
const _standaloneModeSubscribers: StandaloneModeCallback[] = [];
const _fragmentReadySubscribers: FragmentReadyCallback[] = [];
const _undoStateSubscribers: UndoStateCallback[] = [];
const _nodeSettingsSubscribers: NodeSettingsCallback[] = [];
type AudioDeviceChangedCallback = (nodeId: string, settingsJson: string) => void;
const _audioDeviceChangedSubscribers: AudioDeviceChangedCallback[] = [];

// MIDI devices use a subscriber array so multiple DeviceSelector components
// can all receive updates, and a cache so late-mounting components get the
// last known list immediately on subscribe.
const _midiDeviceSubscribers: MidiDevicesCallback[] = [];
let   _midiDeviceCache: MidiDeviceList | null = null;

// Track which device IDs are already claimed: nodeId → deviceId
const _claimedDevices = new Map<string, { deviceId: string; nodeType: number }>();

// Subscribers for claimed-device changes (same pattern as devices)
type ClaimedCallback = (claimed: Map<string, { deviceId: string; nodeType: number }>) => void;
const _claimedSubscribers: ClaimedCallback[] = [];

function _dispatchMidiDevices(list: MidiDeviceList) {
  _midiDeviceCache = list;
  _midiDeviceSubscribers.forEach(cb => cb(list));
}

// Audio devices — same subscriber+cache pattern as MIDI
type AudioDevicesCallback = (devices: AudioDeviceList) => void;
const _audioDeviceSubscribers: AudioDevicesCallback[] = [];
let   _audioDeviceCache: AudioDeviceList | null = null;

function _dispatchAudioDevices(list: AudioDeviceList) {
  _audioDeviceCache = list;
  _audioDeviceSubscribers.forEach(cb => cb(list));
}

function _dispatchClaimed() {
  const snapshot = new Map(_claimedDevices);
  _claimedSubscribers.forEach(cb => cb(snapshot));
}



// Expose callback target for C++ to call
(window as any).__bridge = {
  onGraphUpdate: (json: string) => {
    try {
      const state: GraphState = JSON.parse(json);
      // Rebuild claimed devices from graph state — keeps claims in sync with C++
      _claimedDevices.clear();
      for (const node of state.nodes) {
        if (node.selectedDeviceId && (node.nodeType === 3 || node.nodeType === 4)) {
          _claimedDevices.set(node.id, { deviceId: node.selectedDeviceId, nodeType: node.nodeType });
        }
      }
      _graphUpdateSubscribers.forEach(cb => cb(state));
    } catch (e) {
      console.error('Bridge parse error', e);
    }
  },
  onAudioDevices: (json: string) => {
    try {
      const data: AudioDeviceList = JSON.parse(json);
      _dispatchAudioDevices(data);
    } catch (e) {
      console.error('Bridge audioDevices parse error', e);
    }
  },
  onPortActivity: (json: string) => {
    try {
      const entries: PortActivityEntry[] = JSON.parse(json);
      _portActivitySubscribers.forEach(cb => {
        try { cb(entries); } catch (e) { console.error('portActivity subscriber error', e); }
      });
    } catch (e) { console.error('Bridge portActivity parse error', e); }
  },
  onAudioSnapshot: (json: string) => {
    try {
      const snaps: AudioSnapshot[] = JSON.parse(json);
      // Call each subscriber independently so one failure can't break others
      _audioSnapshotSubscribers.forEach(cb => {
        try { cb(snaps); } catch (e) { console.error('audioSnapshot subscriber error', e); }
      });
    } catch (e) {
      console.error('Bridge audioSnapshot parse error', e);
    }
  },
  onMidiMonitorEvents: (json: string) => {
    try {
      const batches: MidiMonitorBatch[] = JSON.parse(json);
      _midiMonitorSubscribers.forEach(cb => cb(batches));
    } catch (e) {
      console.error('Bridge monitorEvents parse error', e);
    }
  },
  onMidiDevices: (json: string) => {
    try {
      const data: MidiDeviceList = JSON.parse(json);
      _dispatchMidiDevices(data);
    } catch (e) {
      console.error('Bridge midiDevices parse error', e);
    }
  },
  onSpectrumSnapshots: (json: string) => {
    try {
      const snaps = JSON.parse(json) as SpectrumSnapshot[];
      _spectrumSubscribers.forEach(cb => cb(snaps));
    } catch {}
  },

  onStandaloneMode: (val: string) => {
    const v = val === 'true';
    _standaloneModeSubscribers.forEach(cb => cb(v));
  },

  onAudioSettings: (json: string) => {
    try {
      const s = JSON.parse(json) as AudioSettings;
      _audioSettingsSubscribers.forEach(cb => cb(s));
    } catch {}
  },

  onFileState: (json: string) => {
    try {
      const s = JSON.parse(json) as FileState;
      _fileStateSubscribers.forEach(cb => cb(s));
    } catch {}
  },

  onNodeSettings: (json: string) => {
    try {
      const { nodeId, settingsJson } = JSON.parse(json);
      _nodeSettingsSubscribers.forEach(cb => cb(nodeId, settingsJson));
    } catch {}
  },

  onFragmentReady: (json: string) => {
    try {
      const fragment = JSON.parse(json) as FragmentData;
      _fragmentReadySubscribers.forEach(cb => cb(fragment));
    } catch (e) {
      console.error('Bridge fragment parse error', e);
    }
  },

  onUndoState: (json: string) => {
    try {
      const s = JSON.parse(json) as UndoState;
      _undoStateSubscribers.forEach(cb => cb(s));
    } catch {}
  },

  /** Forwarded key events from JUCE (e.g. Escape when WebView doesn't have focus) */
  onKeyEvent: (json: string) => {
    try {
      const key = JSON.parse(json) as string;
      window.dispatchEvent(new KeyboardEvent('keydown', { key, bubbles: true }));
    } catch {}
  },

  onPaxList: (json: string) => {
    try {
      const data = JSON.parse(json);
      const addons = data.paxItems ?? [];
      _addonListSubscribers.forEach(cb => cb(addons));
    } catch (e) {
      console.error('Bridge Xtension list parse error', e);
    }
  },

  onAudioDeviceChanged: (json: string) => {
    try {
      const { nodeId, settingsJson } = JSON.parse(json) as { nodeId: string; settingsJson: string };
      _audioDeviceChangedSubscribers.forEach(cb => cb(nodeId, settingsJson));
    } catch {}
  },

};

function sendToJuce(msg: object) {
  // JUCE 8 injects window.__JUCE__.backend — NOT window.Juce
  // See: https://docs.juce.com/master/classWebBrowserComponent_1_1Options.html
  const backend = (window as any).__JUCE__?.backend;
  if (backend?.emitEvent) {
    backend.emitEvent('graphMessage', msg);
  } else {
  }
}

export const Bridge = {
  onSpectrumSnapshots(cb: SpectrumCallback) {
    _spectrumSubscribers.push(cb);
    return () => {
      const idx = _spectrumSubscribers.indexOf(cb);
      if (idx >= 0) _spectrumSubscribers.splice(idx, 1);
    };
  },

  onStandaloneMode(cb: StandaloneModeCallback) {
    _standaloneModeSubscribers.push(cb);
    return () => {
      const idx = _standaloneModeSubscribers.indexOf(cb);
      if (idx >= 0) _standaloneModeSubscribers.splice(idx, 1);
    };
  },

  onAudioSettings(cb: AudioSettingsCallback) {
    _audioSettingsSubscribers.push(cb);
    return () => {
      const idx = _audioSettingsSubscribers.indexOf(cb);
      if (idx >= 0) _audioSettingsSubscribers.splice(idx, 1);
    };
  },

  setAudioEngineSettings(sampleRate: number, bufferSize: number, muteFeedback: boolean) {
    sendToJuce({ type: 'setAudioEngineSettings', sampleRate, bufferSize, muteFeedback });
  },

  onFileState(cb: FileStateCallback) {
    _fileStateSubscribers.push(cb);
    return () => {
      const idx = _fileStateSubscribers.indexOf(cb);
      if (idx >= 0) _fileStateSubscribers.splice(idx, 1);
    };
  },

  onFragmentReady(cb: FragmentReadyCallback) {
    _fragmentReadySubscribers.push(cb);
    return () => {
      const idx = _fragmentReadySubscribers.indexOf(cb);
      if (idx >= 0) _fragmentReadySubscribers.splice(idx, 1);
    };
  },

  onUndoState(cb: UndoStateCallback) {
    _undoStateSubscribers.push(cb);
    return () => {
      const idx = _undoStateSubscribers.indexOf(cb);
      if (idx >= 0) _undoStateSubscribers.splice(idx, 1);
    };
  },

  onNodeSettings(cb: NodeSettingsCallback) {
    _nodeSettingsSubscribers.push(cb);
    return () => {
      const idx = _nodeSettingsSubscribers.indexOf(cb);
      if (idx >= 0) _nodeSettingsSubscribers.splice(idx, 1);
    };
  },

  undo() { sendToJuce({ type: 'undo' }); },
  redo() { sendToJuce({ type: 'redo' }); },

  fileSave()    { sendToJuce({ type: 'fileSave' }); },
  fileSaveAs()  { sendToJuce({ type: 'fileSaveAs' }); },
  fileOpen()    { sendToJuce({ type: 'fileOpen' }); },
  fileNew()     { sendToJuce({ type: 'fileNew' }); },

  /** Export selected nodes to a fragment file.
   *  selectedNodeIds: ReactFlow node IDs currently selected.
   *  suggestedName:   filename hint derived from node types (no extension). */
  exportSelection(selectedNodeIds: string[], suggestedName: string) {
    sendToJuce({ type: 'exportSelection', selectedNodeIds, suggestedName });
  },

  /** Ask C++ to open a file picker and load a fragment.
   *  C++ will push onFragmentReady with remapped JSON when done. */
  importFragment() {
    sendToJuce({ type: 'importFragment' });
  },

  /** Called after the user drops a fragment on the canvas.
   *  Sends the remapped nodes + connections (with final positions) to C++
   *  so the audio graph is rebuilt to match the React state. */
  importFragmentNodes(nodes: RawNode[], connections: RawConnection[]) {
    sendToJuce({ type: 'importFragmentNodes', nodes, connections });
  },

  onPaxList(cb: AddonListCallback) {
    _addonListSubscribers.push(cb);
    return () => {
      const idx = _addonListSubscribers.indexOf(cb);
      if (idx >= 0) _addonListSubscribers.splice(idx, 1);
    };
  },
  onPortActivity(cb: PortActivityCallback) {
    _portActivitySubscribers.push(cb);
    return () => {
      const idx = _portActivitySubscribers.indexOf(cb);
      if (idx !== -1) _portActivitySubscribers.splice(idx, 1);
    };
  },
  onAudioSnapshot(cb: AudioSnapshotCallback) {
    _audioSnapshotSubscribers.push(cb);
    return () => {
      const idx = _audioSnapshotSubscribers.indexOf(cb);
      if (idx !== -1) _audioSnapshotSubscribers.splice(idx, 1);
    };
  },
  onMidiMonitorEvents(cb: MidiMonitorCallback) {
    _midiMonitorSubscribers.push(cb);
    // Return unsubscribe function — MUST be called on unmount to prevent duplicates
    return () => {
      const idx = _midiMonitorSubscribers.indexOf(cb);
      if (idx !== -1) _midiMonitorSubscribers.splice(idx, 1);
    };
  },
  onMidiDevices(cb: MidiDevicesCallback) {
    _midiDeviceSubscribers.push(cb);
    if (_midiDeviceCache) cb(_midiDeviceCache);
    return () => {
      const idx = _midiDeviceSubscribers.indexOf(cb);
      if (idx !== -1) _midiDeviceSubscribers.splice(idx, 1);
    };
  },
  onAudioDevices(cb: AudioDevicesCallback) {
    _audioDeviceSubscribers.push(cb);
    if (_audioDeviceCache) cb(_audioDeviceCache);
    return () => {
      const idx = _audioDeviceSubscribers.indexOf(cb);
      if (idx !== -1) _audioDeviceSubscribers.splice(idx, 1);
    };
  },
  onAudioDeviceChanged(cb: AudioDeviceChangedCallback) {
    _audioDeviceChangedSubscribers.push(cb);
    return () => {
      const idx = _audioDeviceChangedSubscribers.indexOf(cb);
      if (idx !== -1) _audioDeviceChangedSubscribers.splice(idx, 1);
    };
  },
  setPaxParameter(nodeId: string, index: number, value: number) {
    sendToJuce({ type: 'setPaxParameter', nodeId, index, value });
  },
  setNodeSettings(nodeId: string, settings: object) {
    sendToJuce({ type: 'setNodeSettings', nodeId, settings: JSON.stringify(settings) });
  },

  /** Atomic set+commit for discrete controls — one undo step per change */
  commitSettingsChange(nodeId: string, settings: object) {
    sendToJuce({ type: 'commitSettingsChange', nodeId, settings: JSON.stringify(settings) });
  },

  /** Call when the user finishes adjusting a slider/stepper (mouse up, key up).
   *  Pushes one clean undo snapshot for the completed interaction. */
  commitNodeSettings(nodeId: string) {
    sendToJuce({ type: 'commitNodeSettings', nodeId });
  },
  setNodeLabel(nodeId: string, label: string) {
    sendToJuce({ type: 'setNodeLabel', nodeId, label });
  },
  sendMidiKeyEvent(nodeId: string, status: number, data1: number, data2: number) {
    sendToJuce({ type: 'midiKeyEvent', nodeId, status, data1, data2 });
  },
  setNodeParam(nodeId: string, key: string, value: string, nodeType = 0) {
    sendToJuce({ type: 'setNodeParam', nodeId, key, value });
    // Track device claim with nodeType so direction-aware filtering works:
    // IN nodes (1, 3) only compete with other IN nodes of the same type.
    // OUT nodes (2, 4) only compete with other OUT nodes of the same type.
    if (key === 'midiDeviceId' || key === 'audioDeviceId') {
      if (value) _claimedDevices.set(nodeId, { deviceId: value, nodeType });
      else        _claimedDevices.delete(nodeId);
      _dispatchClaimed();
    }
  },

  /** Subscribe to claimed-device changes.
   *  Callback receives a Map<nodeId, deviceId> of all current claims.
   *  Returns an unsubscribe function. */
  onClaimedDevices(cb: ClaimedCallback) {
    _claimedSubscribers.push(cb);
    cb(new Map(_claimedDevices));  // immediate replay
    return () => {
      const idx = _claimedSubscribers.indexOf(cb);
      if (idx !== -1) _claimedSubscribers.splice(idx, 1);
    };
  },

  onGraphUpdate(cb: GraphUpdateCallback) {
    _graphUpdateSubscribers.push(cb);
    return () => {
      const idx = _graphUpdateSubscribers.indexOf(cb);
      if (idx !== -1) _graphUpdateSubscribers.splice(idx, 1);
    };
  },

  ready() {
    sendToJuce({ type: 'ready' });
  },

  addNode(nodeType: number, x: number, y: number, paxName = '') {
    sendToJuce({ type: 'addNode', nodeType, x, y, paxName });
  },

  removeNode(nodeId: string) {
    sendToJuce({ type: 'removeNode', nodeId });
    // Release any device claim held by this node
    if (_claimedDevices.has(nodeId)) {
      _claimedDevices.delete(nodeId);
      _dispatchClaimed();
    }
  },

  addConnection(
    sourceNodeId: string, sourcePortId: string,
    targetNodeId: string, targetPortId: string
  ) {
    sendToJuce({ type: 'addConnection', sourceNodeId, sourcePortId, targetNodeId, targetPortId });
  },

  removeConnection(connectionId: string) {
    sendToJuce({ type: 'removeConnection', connectionId });
  },

  moveNode(nodeId: string, x: number, y: number) {
    sendToJuce({ type: 'moveNode', nodeId, x, y });
  },
  setViewport(x: number, y: number, zoom: number) {
    sendToJuce({ type: 'setViewport', x, y, zoom });
  },
};
