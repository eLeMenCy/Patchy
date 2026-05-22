![Patchy](Assets/PatchyLogo.jpg)
## About
**Patchy** is a JUCE 8 VST3 / AU / Standalone node-graph audio/MIDI plugin with a React/ReactFlow UI served via `WebBrowserComponent`. It lets you build and connect audio and MIDI processing chains visually — in real time, inside your DAW or as a standalone application — and extend it with custom node types compiled as dynamic libraries (`.dylib` / `.so` / `.dll`) without recompiling the host.

> Version 0.1.507 — first milestone release.

---

## Table of Contents

1. [Feature Overview](#feature-overview)
2. [Project Structure](#project-structure)
3. [Architecture](#architecture)
4. [Built-in Nodes](#built-in-nodes)
5. [Signal Flow Visualisation](#signal-flow-visualisation)
6. [Addon System](#addon-system)
7. [Patch Files](#patch-files)
8. [Building](#building)
9. [Writing an Addon](#writing-an-addon)
10. [API Reference](#api-reference)
11. [Thread Safety](#thread-safety)

---

## Feature Overview

- **Visual node graph** — drag, connect and rearrange processing nodes on a zoomable/pannable canvas
- **Real-time signal flow** — ports and edges animate with live MIDI flash and audio VU colour (green → yellow → red)
- **Patch files** — save/load/new graph state as human-readable `.patchy` JSON files via the ☰ file menu
- **Auto-save** — full graph state (nodes, connections, viewport) also persisted automatically via DAW project
- **Built-in nodes** — MIDI In/Out, Audio In/Out, MIDI Monitor, Audio Monitor (oscilloscope), MIDI Keyboard
- **Addon system** — drop a `.dylib/.so/.dll` into the addons folder; new node type appears in the sidebar immediately on next launch, no recompile needed
- **Variable port counts** — addons can declare any number of audio/MIDI input and output ports via `NGA_Descriptor`
- **Hint panel** — hover any node, button, port or edge to see a description in the sidebar hint panel
- **Fold/Unfold all** — collapse all nodes to headers for a bird's eye view (`F` key or ⊟ button)
- **WebView UI** — React + ReactFlow running inside JUCE's `WebBrowserComponent`; all UI logic is TypeScript, all audio logic is C++
- **Lucide icons** — clean SVG icons throughout the UI

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
│   └── MidiKeyboardNode.h           MIDI Keyboard (type 7)
│
├── Addons/                          Addon ecosystem
│   ├── AddonAPI.h                   The ONLY header an addon author needs
│   ├── AddonRegistry.h/.cpp         Loads addons, owns DynamicLibrary handles
│   ├── AddonScanner.h/.cpp          Discovers addons in platform folders
│   ├── LevelAddon/                  Audio level control (-60dB to +6dB)
│   ├── AmpAddon/                    Audio amplifier (0dB to +24dB)
│   ├── TransposeAddon/              MIDI transpose (-24 to +24 semitones)
│   ├── EnvelopeAddon/               Audio envelope → MIDI CC converter
│   └── StereoSplitterAddon/         Stereo → Left + Right split (1 in / 2 out)
│
├── UI/                              React / TypeScript frontend
│   └── src/
│       ├── App.tsx                  ReactFlow canvas, graph state, port activity, file menu
│       ├── Bridge.ts                JS↔C++ typed façade + subscriber system
│       ├── GenericNode.tsx          Device nodes + addon nodes (types 1–4, 100+)
│       ├── MidiMonitorNode.tsx      MIDI Monitor node (type 5)
│       ├── AudioMonitorNode.tsx     Audio Monitor node (type 6)
│       ├── MidiKeyboardNode.tsx     MIDI Keyboard node (type 7)
│       ├── NodeUtils.tsx            Shared hooks + NodeHeader + NodeHeaderButton
│       ├── HintPanel.tsx            Hint context, panel, and hint dictionaries
│       ├── Sidebar.tsx              Node palette + hint panel
│       ├── NodeSelect.tsx           Shared custom combobox
│       └── PreferencesPanel.tsx     Graph preferences
│
├── CMakeLists.txt                   Main build — host + UI bundle
└── CMakePresets.json                Build presets
```

---

## Architecture

### Three-layer design

```
┌─────────────────────────────────────────────────────────┐
│  C++ Audio Engine  (audio thread + message thread)      │
│                                                         │
│  PatchyProcessor ──owns──► GraphModel                   │
│       │                    ProcessingGraph              │
│       │                    AddonRegistry                │
│       │                    Monitor buffers (maps)       │
│       └──creates──► PatchyEditor ──owns──► WebBridge    │
└───────────────────────────────┬─────────────────────────┘
           thread + process boundary
┌───────────────────────────────▼─────────────────────────┐
│  WebBridge  (juce::Component + juce::Timer)             │
│                                                         │
│  JS ← C++ push:  evaluateJavascript()                   │
│  C++ ← JS pull:  emitEvent() handler                    │
│  30fps timer:    MIDI events, audio snapshots,          │
│                  port activity, graph trash cleanup     │
│  File I/O:       juce::FileChooser save/open dialogs    │
└───────────────────────────────┬─────────────────────────┘
        WebView boundary (JSON over evaluateJavascript)
┌───────────────────────────────▼─────────────────────────┐
│  React UI  (Vite · ReactFlow · TypeScript · Lucide)     │
│                                                         │
│  Bridge.ts ──dispatches──► App.tsx (ReactFlow canvas)   │
│                            Node components              │
│                            HintPanel system             │
└─────────────────────────────────────────────────────────┘
```

**Key design decisions:**

- `GraphModel.onChange` calls both `rebuildProcessingGraph()` AND `pushGraphToUI()` — single source of truth
- `setStateInformation` uses a RAII guard to guarantee `resumeNotifications()` always fires even on early return
- Audio device callbacks are transferred between graph rebuilds (`transferCallbackTo()`) — eliminates audio gaps on node changes
- Old graphs are moved to a `graphTrash` bin and destroyed on the message thread — prevents audio device destructors running on the audio thread
- Separate `outputManager` and `inputManager` for audio device nodes — prevents IN/OUT nodes clobbering each other

---

## Built-in Nodes

| Type | Name | Ports | Description |
|------|------|-------|-------------|
| 1 | MIDI In Device | MIDI Out | Receives from a physical MIDI input device |
| 2 | MIDI Out Device | MIDI In | Sends to a physical MIDI output device |
| 3 | Audio In Device | Audio Out | Receives from a physical audio input device |
| 4 | Audio Out Device | Audio In | Sends to a physical audio output device |
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

Multi-output nodes (e.g. Stereo Splitter) colour each output dot independently.

---

## Addon System

Addons are shared libraries implementing the `NGA_Descriptor` C API in `Addons/AddonAPI.h`. Discovered at startup by `AddonScanner`, loaded by `AddonRegistry`.

### Addon folder locations

| Platform | Path |
|----------|------|
| macOS | `~/Library/Patchy/Addons/` |
| Windows | `%APPDATA%\Patchy\Addons\` |
| Linux | `~/.patchy/addons/` |

### Port counts

By default addons get 1 in / 1 out matching their `nodeType`. Override with the optional port count fields:

```c
static NGA_Descriptor d {
    "Splitter", "My Studio", "1.0.0",
    2,                // nodeType: Audio
    NGA_API_VERSION,
    1,                // audioInputs:  1 stereo in
    2,                // audioOutputs: 2 mono out
    0,                // midiInputs
    0                 // midiOutputs
};
```

For N audio output ports, `audioOut` pointer layout is: `[port0_ch0, port0_ch1, port1_ch0, port1_ch1, ...]`

### Bundled addons

| Addon | Type | Ports | Parameters |
|-------|------|-------|------------|
| Level | Audio | 1in/1out | Level: -60dB to +6dB |
| Amp | Audio | 1in/1out | Amp: 0dB to +24dB |
| Transpose | MIDI | 1in/1out | Semitones: -24 to +24 |
| Envelope | AV | 1m+1a in/out | Mode, CC, Attack, Release, Band filters |
| Splitter | Audio | 1in/2out | Balance: -1.0 to +1.0 |

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

### Host

In CLion: open the project, select a build configuration and build. Or from the terminal:

```bash
cmake -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j$(nproc)
```

### UI dev server (hot reload)

```bash
cd UI && npm install && npm run dev
```

Then build the host in Debug mode with `PATCHY_DEV_MODE=ON` to connect to the Vite dev server.

### Addons

```bash
cd Addons
clang++ -std=c++20 -shared -fPIC LevelAddon/LevelAddon.cpp -o LevelAddon.dylib
# Copy .dylib to ~/Library/Patchy/Addons/
```

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
        "My Addon",       // name shown in sidebar
        "My Studio",      // vendor
        "1.0.0",          // version
        2,                // nodeType: 1=MIDI, 2=Audio, 3=AV
        NGA_API_VERSION,
        0, 0, 0, 0        // port counts: 0 = use nodeType defaults
    };
    return &d;
}

NGA_Instance* NGA_create()           { return new MyAddon(); }
void NGA_destroy(NGA_Instance* i)    { delete (MyAddon*)i; }
void NGA_prepare(NGA_Instance*, double, int) {}

void NGA_process(NGA_Instance*,
                 float** audioIn, float** audioOut,
                 int numChannels, int numSamples,
                 const NGA_MidiEvent* midiIn,  int midiInCount,
                       NGA_MidiEvent* midiOut, int* midiOutCount, int midiOutMax)
{
    // Audio passthrough
    for (int ch = 0; ch < numChannels; ++ch)
        if (audioIn && audioOut && audioIn[ch] && audioOut[ch])
            std::memcpy(audioOut[ch], audioIn[ch], (size_t)numSamples * sizeof(float));

    // MIDI passthrough
    int w = 0;
    for (int e = 0; e < midiInCount && w < midiOutMax; ++e)
        midiOut[w++] = midiIn[e];
    *midiOutCount = w;
}

int   NGA_getParameterCount(NGA_Instance*)                           { return 0; }
void  NGA_getParameterInfo (NGA_Instance*, int, NGA_ParameterInfo*)  {}
float NGA_getParameter     (NGA_Instance*, int)                      { return 0.f; }
void  NGA_setParameter     (NGA_Instance*, int, float)               {}

} // extern "C"
```

Compile and drop the binary into the addon folder. Restart Patchy — the new node appears in the sidebar.

---

## API Reference

### NGA_Descriptor

```c
typedef struct {
    const char* name;          // Display name
    const char* vendor;        // Author/studio
    const char* version;       // Semver string e.g. "1.0.0"
    int         nodeType;      // 1=MIDI, 2=Audio, 3=AV
    int         apiVersion;    // Must equal NGA_API_VERSION

    // Optional port counts (0 = use nodeType defaults)
    int         audioInputs;
    int         audioOutputs;
    int         midiInputs;
    int         midiOutputs;
} NGA_Descriptor;
```

### NGA_MidiEvent

```c
typedef struct {
    int     sampleOffset;  // Sample position within the block
    uint8_t data[3];       // Raw MIDI bytes
    uint8_t size;          // 1, 2 or 3
} NGA_MidiEvent;
```

### NGA_ParameterInfo

```c
typedef struct {
    const char* name;
    float       minValue;
    float       maxValue;
    float       defaultValue;
    float       step;          // 0 = continuous, ≥1 = integer steps
} NGA_ParameterInfo;
```

### Required exports

| Symbol | Description |
|--------|-------------|
| `NGA_getDescriptor` | Return static descriptor |
| `NGA_create` | Allocate instance |
| `NGA_destroy` | Free instance |
| `NGA_prepare` | Called before audio starts |
| `NGA_process` | Called every audio block |
| `NGA_getParameterCount` | Number of parameters |
| `NGA_getParameterInfo` | Parameter metadata |
| `NGA_getParameter` | Get parameter value |
| `NGA_setParameter` | Set parameter value |

---

## Thread Safety

| Operation | Thread | Mechanism |
|-----------|--------|-----------|
| `NodeProcessor::process()` | Audio | Lock-free atomic counters |
| `MidiMonitorBuffer::push()` | Audio | Atomic read/write indices |
| `AudioMonitorBuffer::push()` | Audio | Atomic write position |
| `MidiKeyboardNode::pushUIEvent()` | Message | Lock-free queue |
| Graph rebuild | Message | `pendingGraph` atomic swap |
| Old graph destruction | Message | `graphTrash` deferred from audio thread |
| Audio device transfer | Message | `transferCallbackTo()` atomic remove+add |

**Rule:** no `std::mutex` on the audio thread. All audio↔message communication uses `std::atomic` or lock-free ring buffers.

---

## Licensing

Patchy uses a **source-open, binary-paid** model — inspired by projects like [Kushview Element](https://kushview.net/element/):

| What | Cost | Terms |
|------|------|-------|
| Source code | Free | GPL v3 — compile it yourself |
| Official pre-built binary | Paid | Convenience fee — supports development |
| Addon API (`AddonAPI.h`) | Free | MIT — no strings attached |
| Bundled example addons | Free | MIT — use as reference |

**Source code** is licensed under [GPL v3](LICENSE) — you are free to download, study, modify and compile Patchy yourself at no cost.

**Official pre-built binaries** are available for a small fee. This supports ongoing development.

**Addon developers** are explicitly free to license their addons under any terms they choose — proprietary, MIT, GPL, or anything else. Addons are considered separate works communicating with Patchy at arm's length via the Addon API (see [Addons/LICENSE](Addons/LICENSE)).

---

*Patchy v0.1.507 — JUCE 8 · React 19 · ReactFlow · Vite · TypeScript · Lucide*
