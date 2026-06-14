![Patchy](Assets/PatchyLogo.jpg)

## About

**Patchy** is a JUCE 8 VST3 / AU / Standalone node-graph audio/MIDI plugin with a React/ReactFlow UI served via `WebBrowserComponent`. It lets you build and connect audio and MIDI processing chains visually — in real time, inside your DAW or as a standalone application — and extend it with custom node types compiled as dynamic libraries (`.dylib` / `.so` / `.dll`) without recompiling the host.

> Version 0.0.881

---

## Table of Contents

1. [Feature Overview](#feature-overview)
2. [Project Structure](#project-structure)
3. [Built-in Nodes](#built-in-nodes)
4. [Signal Flow Visualisation](#signal-flow-visualisation)
5. [DAW Mode](#daw-mode)
6. [Standalone Mode](#standalone-mode)
7. [Xtension System](#xtension-system)
8. [Patch Files](#patch-files)
9. [Fragment Export / Import](#fragment-export--import)
10. [Channel Selection](#channel-selection)
11. [Undo / Redo](#undo--redo)
12. [Keyboard Shortcuts](#keyboard-shortcuts)
13. [Building](#building)
14. [Writing an Xtension](#writing-an-xtension)
15. [API Reference](#api-reference)
16. [Thread Safety](#thread-safety)
17. [Performance](#performance)
18. [Licensing](#licensing)

---

## Feature Overview

- **Visual node graph** — drag, connect and rearrange processing nodes on a zoomable/pannable canvas
- **Centred node drop** — nodes appear centred on the drop point, sized correctly for every node type
- **Real-time signal flow** — ports and edges animate with live MIDI flash and audio VU colour (green → yellow → red)
- **Per-port VU** — multi-output nodes (Splitter, Spectrumyser) colour each output dot independently
- **Per-node channel selection** — Audio IN/OUT nodes expose a settings panel to select any combination of physical channels; supports devices up to 256 channels (e.g. Blackhole 16ch)
- **DAW mode** — full bidirectional audio routing between Patchy and your DAW track via a virtual "DAW" device
- **Standalone mode** — full standalone app with its own audio device selection, window bounds persistence and last-folder memory
- **DAW device protection** — DAW loopback and DAW host devices locked by default; unlockable via Preferences
- **Patch files** — save/load/new graph state as human-readable `.patchy` JSON files
- **Auto-save** — full graph state persisted automatically via DAW project state
- **Fragment export/import** — select any nodes, export as a reusable `.patchy` fragment, reimport with ghost-placement UX
- **50-step undo/redo** — full graph snapshot history via `⌘Z` / `⌘⇧Z`
- **Parameter persistence** — Xtension parameters (sliders, steps) survive graph rebuilds, file loads and app restarts
- **Built-in nodes** — MIDI In/Out, Audio In/Out, MIDI Monitor, Audio Monitor (oscilloscope), MIDI Keyboard
- **Xtension system** — drop a `.dylib/.so/.dll` into the addons folder; new node type appears in the sidebar on next launch
- **Dynamic port counts** — Xtensions can change their output port count at runtime (e.g. Spectrumyser band count) without audio interruption
- **Restructured burger menu** — `☰` top-right opens File and Edit flyout submenus with keyboard shortcuts
- **Hint panel** — hover any node, button, port or edge to see a description in the sidebar hint panel
- **Fold/Unfold** — double-click header to collapse nodes; edges merge gracefully to centre
- **WebView UI** — React + ReactFlow running inside JUCE's `WebBrowserComponent`; all UI logic is TypeScript, all audio logic is C++

---

## Project Structure

```
Patchy/
├── Source/                          Core C++ engine
│   ├── PatchyProcessor.h/.cpp       AudioProcessor — owns all state
│   ├── PatchyEditor.h/.cpp          AudioProcessorEditor + keyboard shortcuts
│   ├── WebBridge.h/.cpp             JS↔C++ bridge + 30fps timer + file I/O
│   ├── GraphModel.h/.cpp            UI data model (message thread)
│   ├── ProcessingGraph.h/.cpp       Topological sort + audio/MIDI routing
│   ├── NodeProcessor.h/.cpp         Abstract base — single + multi-port buffers
│   ├── MidiDeviceNodes.h/.cpp       MIDI In (type 1) + MIDI Out (type 2)
│   ├── AudioDeviceNodes.h/.cpp      Audio In (type 3) + Audio Out (type 4)
│   │                                Includes AudioDeviceManager + multi-channel FIFO
│   ├── MidiMonitorNode.h/.cpp       MIDI Monitor (type 5)
│   ├── AudioMonitorNode.h/.cpp      Audio Monitor (type 6)
│   ├── MidiKeyboardNode.h           MIDI Keyboard (type 7)
│   └── StandaloneApp.h/.cpp         Standalone wrapper (window bounds, file location)
│
├── Pax/                          Xtension ecosystem
│   ├── PaxAPI.h                   The ONLY header an addon author needs
│   ├── PaxRegistry.h/.cpp         Loads addons, owns DynamicLibrary handles
│   ├── PaxScanner.h/.cpp          Discovers addons in platform folders
│   ├── LevelPax/                  Audio level control (-60dB to +6dB)
│   ├── AmpPax/                    Audio amplifier (0dB to +24dB)
│   ├── TransposePax/              MIDI transpose (-24 to +24 semitones)
│   ├── EnvelopePax/               Audio envelope → MIDI CC converter
│   ├── StereoSplitterPax/         Stereo → Left + Right split (1 in / 2 out)
│   └── SpectrumyserPax/           FFT spectrum analyser with band outputs
│
├── UI/                              React / TypeScript frontend
│   └── src/
│       ├── App.tsx                  ReactFlow canvas, graph sync, port activity, menus
│       ├── Bridge.ts                JS↔C++ typed façade + subscriber system
│       ├── NodeUtils.tsx            Shared hooks, components + style helpers
│       ├── GenericNode.tsx          Device nodes + Xtension nodes (types 1–4, 100+)
│       │                            Includes channel selection settings panel
│       ├── MidiMonitorNode.tsx      MIDI Monitor node (type 5)
│       ├── AudioMonitorNode.tsx     Audio Monitor node (type 6)
│       ├── MidiKeyboardNode.tsx     MIDI Keyboard node (type 7)
│       ├── SpectrumyserNode.tsx     Spectrumyser custom node with FFT canvas
│       ├── EnvelopeNode.tsx         Envelope custom node with live canvas
│       ├── HintPanel.tsx            Hint context, panel, and hint dictionaries
│       ├── Sidebar.tsx              Node palette + hint panel
│       ├── NodeSelect.tsx           Shared custom combobox with hint + warning support
│       ├── DawContext.ts            DAW mode context (loopback + host device toggles)
│       └── PreferencesPanel.tsx     Graph preferences (DAW routing, audio settings)
│
├── FYI/                             Developer notes (gitignored)
│   └── Architecture.md             Detailed technical architecture + design decisions
│
├── CMakeLists.txt                   Main build — host + UI bundle
├── CMakePresets.json                Build presets
└── README.md                        This file
```

---

## Built-in Nodes

| Type | Name | Ports | Description |
|------|------|-------|-------------|
| 1 | MIDI In Device | MIDI Out | Receives from a physical or virtual MIDI input |
| 2 | MIDI Out Device | MIDI In | Sends to a physical or virtual MIDI output |
| 3 | Audio In Device | Audio Out | Captures from physical device or DAW track; channel-selectable |
| 4 | Audio Out Device | Audio In | Sends to physical device or DAW track; channel-selectable |
| 5 | MIDI Monitor | MIDI In + Out | Inspects MIDI events; pass-through; event table with filters |
| 6 | Audio Monitor | Audio In | Stereo oscilloscope; trigger modes; VU zoom |
| 7 | MIDI Keyboard | MIDI In + Out | Virtual keyboard; pitch/mod wheels; upstream note display |
| 100+ | Xtension nodes | Per descriptor | Dynamically loaded from `.dylib/.so/.dll` |

---

## Signal Flow Visualisation

All ports and edges animate live at 30fps:

**MIDI activity** — flashes bright cyan-white (80ms) on OUT port, edge and downstream IN port

**Audio level** — continuously reflects RMS level via colour:
- Silence → dim base colour
- Low → green · Mid → yellow · High → red
- Glow intensity scales with level

**Multi-output nodes** (Splitter, Spectrumyser) colour each output dot independently using per-port RMS.

**Important:** only explicitly connected audio paths produce sound. Observer nodes (AudioMonitor, Spectrumyser) do not route audio to the output unless connected to an AudioOut device node.

---

## DAW Mode

When loaded as a VST3/AU plugin, Patchy operates in DAW mode:

- **"DAW" virtual device** appears at the top of Audio In/Out device combos
- Selecting "DAW" on AudioIN routes the DAW track's audio into the graph
- Selecting "DAW" on AudioOUT routes processed audio back to the DAW track
- **Empty graph** → audio passes through transparently (DAW track unaffected)

### Safety locks

| Setting | Default | Risk if enabled |
|---------|---------|-----------------| 
| DAW loopback (AudioOUT → DAW) | 🔒 Locked | Feedback loop |
| DAW host devices (Bitwig, Ableton, etc.) | 🔒 Locked | Signal doubling |

Both can be unlocked via **Preferences → Graph → DAW Routing**.

---

## Standalone Mode

When launched as a standalone application, Patchy:

- Opens with its own `AudioDeviceManager` — select input/output devices per node
- **Remembers window position and size** across sessions
- **Remembers last file location** — file dialogs reopen in the last used folder
- Audio settings (sample rate, buffer size, feedback mute) configurable via Preferences
- Supports the same patch file workflow as DAW mode

---

## Xtension System

Addons are shared libraries implementing the `NGA_Descriptor` C API in `Pax/PaxAPI.h`. Discovered at startup by `PaxScanner`, loaded by `PaxRegistry`.

### Xtension folder locations

| Platform | Path |
|----------|------|
| macOS | `~/Library/Patchy/Pax/` |
| Windows | `%APPDATA%\Patchy\Pax\` |
| Linux | `~/.patchy/pax/` |

### Bundled Xtensions

| Xtension | Type | Ports | Parameters |
|-------|------|-------|------------|
| Level | Audio | 1in/1out | Level: -60dB to +6dB |
| Amp | Audio | 1in/1out | Amp: 0dB to +24dB |
| Transpose | MIDI | 1in/1out | Semitones: -24 to +24 |
| Envelope | AV Hybrid | 1m+1a in / 1m+1a out | Mode, CC, Attack, Release, Band filters |
| Splitter | Audio | 1in/2out | — (L→out1, R→out2) |
| Spectrumyser | Audio | 1in/1-5out | Band count (1-5), per-band frequency range |

### Parameter persistence

Xtension parameters are automatically saved in `settingsJson` on every change and restored when:
- A patch file is loaded
- The graph is rebuilt (adding/connecting nodes)
- The app is restarted (via DAW project state or patch file)

### Dynamic port counts

Xtensions can change their output port count at runtime by exporting `PAX_getAudioOutputCount`. Patchy updates the node's ports and routing live — without a full graph rebuild or audio interruption — only when the count actually changes.

### Building an Xtension (macOS example)

```bash
cd Addons/SpectrumyserAddon
clang++ -std=c++20 -shared -fPIC SpectrumyserAddon.cpp \
        -o SpectrumyserAddon.dylib
cp SpectrumyserAddon.dylib ~/Library/Patchy/Pax/
```

---

## Patch Files

Patches are saved as `.patchy` files — plain JSON containing nodes, connections, viewport and settings. Fully human-readable and tweakable in any text editor.

### Burger menu `☰` (top-right)

The menu is organised into two flyout submenus, opening to the left on hover:

**File ▸**
- **New** `⌘N` — clear the graph
- **Open…** `⌘O` — load a `.patchy` file
- **Save** `⌘S` — save to current file, or prompt if unsaved
- **Save As…** `⌘⇧S` — always prompt for location
- **Export…** — export selected nodes as a fragment (enabled when nodes are selected)
- **Import…** — import a `.patchy` fragment with ghost-placement UX

**Edit ▸**
- **Undo** `⌘Z` — step back through 50-step history
- **Redo** `⌘⇧Z` — step forward
- **Cut / Copy / Paste** — reserved, coming soon
- **Delete** `⌫` — remove selected nodes (enabled when nodes are selected)

---

## Fragment Export / Import

Sub-graphs can be saved and reused as `.patchy` fragment files.

### Export
1. Select nodes on the canvas (`⌘`-click on node headers)
2. **☰ → File → Export…** — enabled when nodes are selected, showing count
3. Choose a filename — suggested name is derived from selected node types
4. Only connections between selected nodes are included; external connections are silently dropped

### Import — Ghost Overlay UX
1. **☰ → File → Import…** → file picker opens
2. A dashed bounding box follows the cursor showing node count and "click to place · esc to cancel"
3. The canvas remains fully pannable/zoomable while holding the ghost
4. **Click** → places nodes at cursor position
5. **Escape** or **right-click** → cancels

---

## Channel Selection

Audio IN and OUT device nodes support per-node channel selection for multi-channel devices (e.g. Blackhole 16ch, up to 256 channels).

- Click the **⚙ gear icon** on any Audio IN or OUT node header to open the channel settings panel
- Check any combination of channels — the selection is shown as a summary above the device combobox (e.g. `Ch 1, Ch 3, Ch 5`)
- The settings panel adapts its layout (2 / 3 / 4 columns) for 16 / 32 / 128+ channel devices
- **Warn + reset** — if the device is changed and the current channel selection is no longer valid, the selection resets to `Ch 1, Ch 2` and a warning badge appears on the gear icon
- Channel selections are persisted in the patch file and are undo/redo aware

**Routing semantics:**
- **Audio IN** — selected physical input channels are captured and packed into contiguous graph channels (0, 1, 2…) for downstream nodes
- **Audio OUT** — graph channels (0, 1, 2…) from upstream nodes are routed to the selected physical output channels

---

## Undo / Redo

Patchy maintains a **50-step snapshot history** of the full graph state.

| Action | Snapshot taken |
|--------|---------------|
| Drop a node | ✅ |
| Delete a node | ✅ |
| Draw a connection | ✅ |
| Delete a connection | ✅ |
| Change device selection | ✅ |
| Change node settings | ✅ |
| Import a fragment | ✅ |
| Move a node | ❌ (intentional — keeps history clean) |

Undo/Redo is accessible via `⌘Z` / `⌘⇧Z`, or via **☰ → Edit → Undo / Redo**.

---

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| `⌘N` | New graph |
| `⌘O` | Open patch file |
| `⌘S` | Save |
| `⌘⇧S` | Save As |
| `⌘Z` | Undo |
| `⌘⇧Z` | Redo |
| `F` | Fold / unfold all nodes |
| `Delete` / `⌫` | Delete selected node or edge |
| Double-click header | Collapse / expand node |
| `Escape` | Cancel fragment import ghost |

---

## Building

### Prerequisites
- CMake 3.22+
- JUCE 8 (fetched automatically via CMake FetchContent)
- Node.js 18+ and npm (for UI build)
- C++20 compiler

### Host (VST3 / AU / Standalone)

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j$(nproc)
```

### UI dev server (hot reload)

```bash
cd UI && npm install && npm run dev
```

Build the host in Debug mode with `PATCHY_DEV_MODE=ON` to connect to the Vite dev server.

---

## Writing an Xtension

Include only `Pax/PaxAPI.h`. No JUCE dependency required.

```cpp
#include "AddonAPI.h"
#include <cstring>

struct MyAddon {};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor() {
    static PAX_Descriptor d {
        "My Xtension", "My Studio", "1.0.0",
        2,               // nodeType: 1=MIDI, 2=Audio, 3=AV
        PAX_API_VERSION,
        1, 1, 0, 0       // audioIn, audioOut, midiIn, midiOut
    };
    return &d;
}

PAX_Instance* PAX_create()           { return new MyAddon(); }
void PAX_destroy (PAX_Instance* i)   { delete (MyAddon*)i; }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance*, const PAX_ProcessContext* ctx)
{
    // Audio pass-through example
    if (ctx->audioIn && ctx->audioOut)
        for (int ch = 0; ch < ctx->numChannels; ++ch)
            if (ctx->audioIn[ch] && ctx->audioOut[ch])
                std::memcpy (ctx->audioOut[ch], ctx->audioIn[ch],
                             (size_t) ctx->numSamples * sizeof (float));

    // MIDI pass-through
    *ctx->midiOutCount = 0;
    for (int e = 0; e < ctx->midiInCount && e < ctx->midiMaxCount; ++e)
        ctx->midiOut[(*ctx->midiOutCount)++] = ctx->midiIn[e];

    // Value ports — NULL until implemented by host, always guard:
    // if (ctx->valuesOut && ctx->valueMaxCount > 0) { ... }
}

int   PAX_getParameterCount (PAX_Instance*)                          { return 0; }
void  PAX_getParameterInfo  (PAX_Instance*, int, PAX_ParameterInfo*) {}
float PAX_getParameter      (PAX_Instance*, int)                     { return 0.f; }
void  PAX_setParameter      (PAX_Instance*, int, float)              {}

} // extern "C"
```

### Optional exports

| Symbol | Description |
|--------|-------------|
| `PAX_getAudioOutputCount` | Return current output port count (dynamic ports) |
| `PAX_getFFTSize` | Return FFT magnitude bin count (for spectrum display) |
| `PAX_getFFTMagnitudes` | Return pointer to FFT magnitude array |

---

## API Reference

### PAX_Descriptor

```c
typedef struct {
    const char* name;        // Display name in sidebar
    const char* vendor;      // Author/studio
    const char* version;     // Semver string e.g. "1.0.0"
    int         nodeType;    // 1=MIDI, 2=Audio, 3=AV
    int         apiVersion;  // Must equal PAX_API_VERSION
    int         audioInputs;
    int         audioOutputs;
    int         midiInputs;
    int         midiOutputs;
} PAX_Descriptor;
```

### PAX_MidiEvent

```c
typedef struct {
    int     sampleOffset;
    uint8_t byteCount;
    uint8_t bytes[3];
} PAX_MidiEvent;
```

### PAX_ParameterInfo

```c
typedef struct {
    const char* name;
    float       minValue;
    float       maxValue;
    float       defaultValue;
    float       step;   // 0 = continuous, ≥1 = integer steps
} PAX_ParameterInfo;
```

### PAX_Value  *(new in API v2)*

```c
typedef struct {
    uint32_t key;        // Integer key resolved from name at setup time
    uint8_t  type;       // Domain: PAX_TYPE_GENERIC/DMX/OSC/MQTT/UDP/ARTNET
    uint8_t  dataType;   // PAX_DATA_FLOAT / PAX_DATA_STRING / PAX_DATA_BLOB
    uint16_t dataSize;   // Byte length of data[] when dataType != PAX_DATA_FLOAT
    float    value;      // Primary payload (default)
    uint8_t  data[56];   // Inline buffer for strings/blobs
} PAX_Value;
```

### PAX_ProcessContext  *(new in API v2)*

```c
typedef struct {
    float**              audioIn;        // [numChannels] input channel pointers
    float**              audioOut;       // [numChannels] output channel pointers
    int                  numChannels;    // Always 2 (stereo)
    int                  numSamples;     // Block size
    const PAX_MidiEvent* midiIn;
    int                  midiInCount;
    PAX_MidiEvent*       midiOut;
    int*                 midiOutCount;
    int                  midiMaxCount;
    const PAX_Value*     valuesIn;       // NULL until value ports implemented
    int                  valueInCount;
    PAX_Value*           valuesOut;      // NULL until value ports implemented
    int*                 valueOutCount;
    int                  valueMaxCount;
} PAX_ProcessContext;
```

---

## Thread Safety

| Operation | Thread | Mechanism |
|-----------|--------|-----------|
| `NodeProcessor::process()` | Audio | Lock-free per-node buffers |
| `MidiMonitorBuffer::push()` | Audio | Atomic read/write indices |
| `AudioMonitorBuffer::push()` | Audio | Atomic write position |
| `AudioFifo::write/read()` | Audio + Device callback | Pre-allocated ring, `SpinLock` on channel selection |
| Graph rebuild | Message | `pendingGraph` atomic swap in `processBlock` |
| Old graph destruction | Message | `graphTrash` deferred bin |
| Dynamic port resize | Message | `suspendProcessing` only when reducing ports |
| `updateNodeAudioOutputCount` | Message | `suspendNotificationsQuiet` to avoid rebuild |
| DAW host audio injection | Audio | Step 2 of `ProcessingGraph::process()` |

**Rule:** no `std::mutex` on the audio thread. All audio↔message communication uses `std::atomic` or lock-free ring buffers / FIFOs.

---

## Performance

Tested on MacBook Air (Apple Silicon), 44100 Hz / 512 samples:

| Scenario | Result |
|----------|--------|
| 200 nodes added in batches | ~52ms total |
| Audio chain: AudioIN → 400 chained Level nodes → AudioOUT | Smooth, no crackle |
| Audio chain: AudioIN → 500 chained Level nodes → AudioOUT | Begins to break down |
| Blackhole 16ch IN → OUT (all 16 channels active) | Smooth, no crackle |

Practical patches rarely exceed 20–50 nodes. The bottleneck at scale is the 30fps port activity CSS injection across all nodes — not the audio processing itself.

---

## Licensing

Patchy uses a **source-open, binary-paid** model:

| What | Cost | Terms |
|------|------|-------|
| Source code | Free | GPL v3 — compile it yourself |
| Official pre-built binary | Paid | Convenience fee — supports development |
| Addon API (`AddonAPI.h`) | Free | MIT — no strings attached |
| Bundled example Xtensions | Free | MIT — use as reference |

Xtension developers are free to license their Xtensions under any terms — proprietary, MIT, GPL, or anything else.

---

*Patchy v0.0.881 — JUCE 8 · React 19 · ReactFlow · Vite · TypeScript · Lucide*
