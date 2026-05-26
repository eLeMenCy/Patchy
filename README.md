![Patchy](Assets/PatchyLogo.jpg)

## About

**Patchy** is a JUCE 8 VST3 / AU / Standalone node-graph audio/MIDI plugin with a React/ReactFlow UI served via `WebBrowserComponent`. It lets you build and connect audio and MIDI processing chains visually — in real time, inside your DAW or as a standalone application — and extend it with custom node types compiled as dynamic libraries (`.dylib` / `.so` / `.dll`) without recompiling the host.

> Version 0.0.654

---

## Table of Contents

1. [Feature Overview](#feature-overview)
2. [Project Structure](#project-structure)
3. [Built-in Nodes](#built-in-nodes)
4. [Signal Flow Visualisation](#signal-flow-visualisation)
5. [DAW Mode](#daw-mode)
6. [Addon System](#addon-system)
7. [Patch Files](#patch-files)
8. [Building](#building)
9. [Writing an Addon](#writing-an-addon)
10. [API Reference](#api-reference)
11. [Thread Safety](#thread-safety)
12. [Licensing](#licensing)

---

## Feature Overview

- **Visual node graph** — drag, connect and rearrange processing nodes on a zoomable/pannable canvas
- **Real-time signal flow** — ports and edges animate with live MIDI flash and audio VU colour (green → yellow → red)
- **Per-port VU** — multi-output nodes (Splitter, Spectrumyser) colour each output dot independently
- **DAW mode** — full bidirectional audio routing between Patchy and your DAW track via a virtual "DAW" device
- **DAW device protection** — DAW loopback and DAW host devices locked by default; unlockable via Preferences
- **Patch files** — save/load/new graph state as human-readable `.patchy` JSON files via the ☰ file menu
- **Auto-save** — full graph state also persisted automatically via DAW project state
- **Built-in nodes** — MIDI In/Out, Audio In/Out, MIDI Monitor, Audio Monitor (oscilloscope), MIDI Keyboard
- **Addon system** — drop a `.dylib/.so/.dll` into the addons folder; new node type appears in the sidebar on next launch
- **Dynamic port counts** — addons can change their output port count at runtime (e.g. Spectrumyser band count) without audio interruption
- **Hint panel** — hover any node, button, port or edge to see a description in the sidebar hint panel
- **Fold/Unfold** — double-click header to collapse nodes; edges merge gracefully to centre
- **WebView UI** — React + ReactFlow running inside JUCE's `WebBrowserComponent`; all UI logic is TypeScript, all audio logic is C++

---

## Project Structure

```
Patchy/
├── Source/                          Core C++ engine
│   ├── PatchyProcessor.h/.cpp       AudioProcessor — owns all state
│   ├── PatchyEditor.h/.cpp          AudioProcessorEditor — owns WebBridge
│   ├── WebBridge.h/.cpp             JS↔C++ bridge + 30fps timer + file I/O
│   ├── GraphModel.h/.cpp            UI data model (message thread)
│   ├── ProcessingGraph.h/.cpp       Topological sort + audio/MIDI routing
│   ├── NodeProcessor.h/.cpp         Abstract base — single + multi-port buffers
│   ├── MidiDeviceNodes.h/.cpp       MIDI In (type 1) + MIDI Out (type 2)
│   ├── AudioDeviceNodes.h/.cpp      Audio In (type 3) + Audio Out (type 4)
│   ├── MidiMonitorNode.h/.cpp       MIDI Monitor (type 5)
│   ├── AudioMonitorNode.h/.cpp      Audio Monitor (type 6)
│   ├── MidiKeyboardNode.h           MIDI Keyboard (type 7)
│   └── StandaloneApp.h/.cpp         Standalone wrapper (AudioDeviceManager)
│
├── Addons/                          Addon ecosystem
│   ├── AddonAPI.h                   The ONLY header an addon author needs
│   ├── AddonRegistry.h/.cpp         Loads addons, owns DynamicLibrary handles
│   ├── AddonScanner.h/.cpp          Discovers addons in platform folders
│   ├── LevelAddon/                  Audio level control (-60dB to +6dB)
│   ├── AmpAddon/                    Audio amplifier (0dB to +24dB)
│   ├── TransposeAddon/              MIDI transpose (-24 to +24 semitones)
│   ├── EnvelopeAddon/               Audio envelope → MIDI CC converter
│   ├── StereoSplitterAddon/         Stereo → Left + Right split (1 in / 2 out)
│   └── SpectrumyserAddon/           FFT spectrum analyser with band outputs
│
├── UI/                              React / TypeScript frontend
│   └── src/
│       ├── App.tsx                  ReactFlow canvas, graph state, port activity
│       ├── Bridge.ts                JS↔C++ typed façade + subscriber system
│       ├── GenericNode.tsx          Device nodes + addon nodes (types 1–4, 100+)
│       ├── MidiMonitorNode.tsx      MIDI Monitor node (type 5)
│       ├── AudioMonitorNode.tsx     Audio Monitor node (type 6)
│       ├── MidiKeyboardNode.tsx     MIDI Keyboard node (type 7)
│       ├── SpectrumyserNode.tsx     Spectrumyser custom node with FFT canvas
│       ├── NodeUtils.tsx            Shared hooks + NodeHandle + NodeHeaderButton
│       ├── HintPanel.tsx            Hint context, panel, and hint dictionaries
│       ├── Sidebar.tsx              Node palette + hint panel
│       ├── NodeSelect.tsx           Shared custom combobox with hint + warning support
│       ├── DawContext.ts            DAW mode context (loopback + host device toggles)
│       └── PreferencesPanel.tsx     Graph preferences (DAW routing, etc.)
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
| 3 | Audio In Device | Audio Out | Receives from physical device or DAW track |
| 4 | Audio Out Device | Audio In | Sends to physical device or DAW track |
| 5 | MIDI Monitor | MIDI In + Out | Inspects MIDI events; pass-through; event table with filters |
| 6 | Audio Monitor | Audio In + Out | Stereo oscilloscope; trigger modes; VU zoom; pass-through |
| 7 | MIDI Keyboard | MIDI In + Out | Virtual keyboard; pitch/mod wheels; upstream note display |
| 100+ | Addon nodes | Per descriptor | Dynamically loaded from `.dylib/.so/.dll` |

---

## Signal Flow Visualisation

All ports and edges animate live at 30fps:

**MIDI activity** — flashes bright cyan-white (150ms) on OUT port, edge and downstream IN port

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

## Addon System

Addons are shared libraries implementing the `NGA_Descriptor` C API in `Addons/AddonAPI.h`. Discovered at startup by `AddonScanner`, loaded by `AddonRegistry`.

### Addon folder locations

| Platform | Path |
|----------|------|
| macOS | `~/Library/Audio/Plug-Ins/Patchy Addons/` |
| Windows | `%APPDATA%\Patchy\Addons\` |
| Linux | `~/.patchy/addons/` |

### Bundled addons

| Addon | Type | Ports | Parameters |
|-------|------|-------|------------|
| Level | Audio | 1in/1out | Level: -60dB to +6dB |
| Amp | Audio | 1in/1out | Amp: 0dB to +24dB |
| Transpose | MIDI | 1in/1out | Semitones: -24 to +24 |
| Envelope | AV | 1m+1a in/1m+1a out | Mode, CC, Attack, Release, Band filters |
| Splitter | Audio | 1in/2out | — (L→out1, R→out2) |
| Spectrumyser | Audio | 1in/1-5out | Band count (1-5), per-band frequency range |

### Dynamic port counts

Addons can change their output port count at runtime by exporting `NGA_getAudioOutputCount`. Patchy will update the node's ports and routing live without a full graph rebuild or audio interruption.

### Building an addon (macOS example)

```bash
cd Addons/SpectrumyserAddon
clang++ -std=c++20 -shared -fPIC SpectrumyserAddon.cpp \
        -o SpectrumyserAddon.dylib
cp SpectrumyserAddon.dylib ~/Library/Audio/Plug-Ins/Patchy\ Addons/
```

---

## Patch Files

Patches are saved as `.patchy` files — plain JSON containing nodes, connections, viewport and settings. Fully human-readable and tweakable in any text editor.

**File menu (☰ top-right):**
- **New** — clear the graph
- **Open…** — load a `.patchy` file
- **Save** — save to current file (or prompt if unsaved)
- **Save As…** — always prompt for location

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

## Writing an Addon

Include only `Addons/AddonAPI.h`. No JUCE dependency required.

```cpp
#include "AddonAPI.h"
#include <cstring>

struct MyAddon {};

extern "C" {

const NGA_Descriptor* NGA_getDescriptor() {
    static NGA_Descriptor d {
        "My Addon", "My Studio", "1.0.0",
        2,               // nodeType: 1=MIDI, 2=Audio, 3=AV
        NGA_API_VERSION,
        1, 1, 0, 0       // audioIn, audioOut, midiIn, midiOut
    };
    return &d;
}

NGA_Instance* NGA_create()           { return new MyAddon(); }
void NGA_destroy(NGA_Instance* i)    { delete (MyAddon*)i; }
void NGA_prepare(NGA_Instance*, double, int) {}

void NGA_process(NGA_Instance*,
                 float** audioIn, float** audioOut,
                 int numChannels, int numSamples,
                 const NGA_MidiEvent*, int,
                       NGA_MidiEvent*, int* outCount, int)
{
    *outCount = 0;
    for (int ch = 0; ch < numChannels; ++ch)
        if (audioIn && audioOut && audioIn[ch] && audioOut[ch])
            std::memcpy(audioOut[ch], audioIn[ch], (size_t)numSamples * sizeof(float));
}

int   NGA_getParameterCount(NGA_Instance*)                          { return 0; }
void  NGA_getParameterInfo (NGA_Instance*, int, NGA_ParameterInfo*) {}
float NGA_getParameter     (NGA_Instance*, int)                     { return 0.f; }
void  NGA_setParameter     (NGA_Instance*, int, float)              {}

} // extern "C"
```

### Optional exports

| Symbol | Description |
|--------|-------------|
| `NGA_getAudioOutputCount` | Return current output port count (dynamic ports) |
| `NGA_getFFTSize` | Return FFT magnitude bin count (for spectrum display) |
| `NGA_getFFTMagnitudes` | Return pointer to FFT magnitude array |

---

## API Reference

### NGA_Descriptor

```c
typedef struct {
    const char* name;        // Display name in sidebar
    const char* vendor;      // Author/studio
    const char* version;     // Semver string e.g. "1.0.0"
    int         nodeType;    // 1=MIDI, 2=Audio, 3=AV
    int         apiVersion;  // Must equal NGA_API_VERSION
    int         audioInputs;
    int         audioOutputs;
    int         midiInputs;
    int         midiOutputs;
} NGA_Descriptor;
```

### NGA_MidiEvent

```c
typedef struct {
    int     sampleOffset;
    uint8_t data[3];
    uint8_t size;
} NGA_MidiEvent;
```

### NGA_ParameterInfo

```c
typedef struct {
    const char* name;
    float       minValue;
    float       maxValue;
    float       defaultValue;
    float       step;   // 0 = continuous, ≥1 = integer steps
} NGA_ParameterInfo;
```

---

## Thread Safety

| Operation | Thread | Mechanism |
|-----------|--------|-----------|
| `NodeProcessor::process()` | Audio | Lock-free per-node buffers |
| `MidiMonitorBuffer::push()` | Audio | Atomic read/write indices |
| `AudioMonitorBuffer::push()` | Audio | Atomic write position |
| Graph rebuild | Message | `pendingGraph` atomic swap in `processBlock` |
| Old graph destruction | Message | `graphTrash` deferred bin |
| Dynamic port resize | Message | `suspendProcessing` only when reducing ports |
| `updateNodeAudioOutputCount` | Message | `suspendNotificationsQuiet` to avoid rebuild |
| DAW host audio injection | Audio | Step 2 of `ProcessingGraph::process()` |

**Rule:** no `std::mutex` on the audio thread. All audio↔message communication uses `std::atomic` or lock-free ring buffers.

---

## Licensing

Patchy uses a **source-open, binary-paid** model:

| What | Cost | Terms |
|------|------|-------|
| Source code | Free | GPL v3 — compile it yourself |
| Official pre-built binary | Paid | Convenience fee — supports development |
| Addon API (`AddonAPI.h`) | Free | MIT — no strings attached |
| Bundled example addons | Free | MIT — use as reference |

Addon developers are free to license their addons under any terms — proprietary, MIT, GPL, or anything else.

---

*Patchy v0.0.654 — JUCE 8 · React 19 · ReactFlow · Vite · TypeScript · Lucide*
