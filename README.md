![Patchy](Assets/PatchyLogo.jpg)

## About

**Patchy** is a JUCE 8 VST3 / AU / Standalone node-graph audio/MIDI plugin with a React/ReactFlow UI served via `WebBrowserComponent`. It lets you build and connect audio and MIDI processing chains visually — in real time, inside your DAW or as a standalone application — and extend it with custom node types compiled as dynamic libraries (`.dylib` / `.so` / `.dll`) without recompiling the host.

> Version 0.0.904

---

## Table of Contents

<!-- TOC -->
  * [About](#about)
  * [Table of Contents](#table-of-contents)
  * [Feature Overview](#feature-overview)
  * [Project Structure](#project-structure)
  * [Built-in Nodes](#built-in-nodes)
  * [Signal Flow Visualisation](#signal-flow-visualisation)
  * [DAW Mode](#daw-mode)
    * [Safety locks](#safety-locks)
  * [Standalone Mode](#standalone-mode)
  * [Pax System](#pax-system)
    * [Pax folder locations](#pax-folder-locations)
    * [Bundled Pax](#bundled-pax)
    * [Parameter persistence](#parameter-persistence)
    * [Dynamic port counts](#dynamic-port-counts)
    * [Building a Pax](#building-a-pax)
  * [Patch Files](#patch-files)
    * [Burger menu `☰` (top-right)](#burger-menu--top-right)
  * [Fragment Export / Import](#fragment-export--import)
    * [Export](#export)
    * [Import — Ghost Overlay UX](#import--ghost-overlay-ux)
  * [Channel Selection](#channel-selection)
  * [Undo / Redo](#undo--redo)
  * [Keyboard Shortcuts](#keyboard-shortcuts)
  * [Building](#building)
    * [Prerequisites](#prerequisites)
    * [Host (VST3 / AU / Standalone)](#host-vst3--au--standalone)
    * [UI dev server (hot reload)](#ui-dev-server-hot-reload)
  * [Writing a Pax](#writing-a-pax)
    * [Optional exports](#optional-exports)
  * [API Reference](#api-reference)
    * [PAX_Descriptor](#pax_descriptor)
    * [PAX_MidiEvent](#pax_midievent)
    * [PAX_ParameterInfo](#pax_parameterinfo)
    * [PAX_Value  *(new in API v2, gained `portIndex` in API v3)*](#pax_value-new-in-api-v2-gained-portindex-in-api-v3)
    * [PAX_ProcessContext  *(new in API v2, gained a DMX universe path in API v4)*](#pax_processcontext-new-in-api-v2-gained-a-dmx-universe-path-in-api-v4)
  * [Thread Safety](#thread-safety)
  * [Performance](#performance)
  * [Licensing](#licensing)
<!-- TOC -->

---

## Feature Overview

- **Visual node graph** — drag, connect and rearrange processing nodes on a zoomable/pannable canvas
- **Strict port typing** — connections are only valid between matching protocol types (MIDI, Audio, OSC, DMX, ArtNet, MQTT, UDP); a UDP node can never be wired directly into an MQTT node or any other mismatched protocol, by design — cross-protocol bridging is meant to go through dedicated converter Pax, not a raw connection
- **Centred node drop** — nodes appear centred on the drop point, sized correctly for every node type
- **Real-time signal flow** — ports and edges animate with live MIDI flash and audio VU colour (green → yellow → red); DMX gets its own treatment, gradual amber intensity tracking the actual channel value rather than a discrete flash, matching its nature as a continuously-held signal rather than a discrete event
- **512-channel DMX universe** — a dedicated wide-payload path (`PAX_ProcessContext`'s `dmxFrameIn`/`dmxFrameOut`, API v4) separate from the general `PAX_Value` mechanism, whose 56-byte inline buffer previously truncated any DMX-carrying node — built-in and Pax alike — to its first 56 of 512 channels
- **Multi-source DMX merging** — several sources (Pax or built-in) feeding the same DMX Out combine correctly via HTP (Highest Takes Precedence), the same convention real DMX consoles/mergers use, rather than the last one silently overwriting the others
- **Colour-coded connection preview** — the dashed line shown while dragging a new connection matches the source port's own protocol colour, not a fixed generic accent
- **Per-port VU** — multi-output nodes (Splitter, Spectrumyser) colour each output dot independently
- **Per-port typed Pax flash** — a Pax with multiple differently-typed Value ports (e.g. MQTT + DMX on the same node) flashes each output in its own correct protocol colour, not one blanket colour for the whole node
- **Automatic Hybrid/Converter node colouring** — a Pax's overall colour is auto-detected from its own declared ports: a type present on only one side (input or output) marks it a Converter (fuchsia); every type mirrored on both sides gives it a single native colour if there's only one, or Hybrid (orange) if there's more than one — e.g. Envelope (Audio+MIDI, mirrored) is Hybrid, `MqttToValuePax` (MQTT in, generic out — not mirrored) is a Converter
- **Live read-only Pax parameters, folding settings for multi-param Pax** — a Pax can mark a parameter as a live display rather than an editable control (`PAX_isParameterReadOnly`), e.g. `OscToValuePax`/`ValueToDMXPax`'s own "Current Value". A Pax with exactly one such display shows it compactly in the node header, no settings toggle needed; a Pax with more than one parameter gets a settings cog instead, folding its full controls behind a click and showing a labelled summary of the rest when closed (e.g. `ValueToDMXPax` folded: "min: 0.00 - max: 1.00 - value: 0.786") — a Pax with exactly one *editable* parameter (`LevelPax`, `AmpPax`) is unaffected either way, always shown directly
- **Per-node channel selection** — Audio IN/OUT nodes expose a settings panel to select any combination of physical channels; supports devices up to 256 channels (e.g. Blackhole 16ch)
- **DAW mode** — full bidirectional audio routing between Patchy and your DAW track via a virtual "DAW" device
- **Standalone mode** — full standalone app with its own audio device selection, window bounds persistence and last-folder memory
- **DAW device protection** — DAW loopback and DAW host devices locked by default; unlockable via Preferences
- **Patch files** — save/load/new graph state as human-readable `.patchy` JSON files
- **Auto-save** — full graph state persisted automatically via DAW project state
- **Fragment export/import** — select any nodes, export as a reusable `.patchy` fragment, reimport with ghost-placement UX
- **50-step undo/redo** — full graph snapshot history via `⌘Z` / `⌘⇧Z`
- **Parameter persistence** — Pax parameters (sliders, steps) survive graph rebuilds, file loads and app restarts
- **Built-in nodes** — MIDI In/Out, Audio In/Out, MIDI Monitor, Audio Monitor (oscilloscope), Audio Player (file/sine/noise source), MIDI Keyboard, UDP In/Out, OSC In/Out, ArtNet In/Out, DMX In/Out, DMX Monitor, DMX Console, ArtNet Monitor, ArtNet Console, OSC Monitor, UDP Monitor, MQTT Subscribe, MQTT Publish, MQTT Monitor, MQTT Console
- **Protocol device nodes** — Phase 3 built-in nodes for network and hardware protocols; UDP, OSC 1.0, Art-Net (ArtDmx), DMX USB (Enttec Pro/Mk2); live byte-rate labels; change-driven activity flash
- **DMX Monitor + Console** — vertical fader bank and bargraph display for all 512 DMX channels; configurable visible count (8/16/24/32); page navigation; dec/pct/hex format; custom name; Blackout button; full undo/redo; Console is output-only
- **ArtNet Monitor + Console** — same 512-channel fader/bargraph as DMX; universe selector (0–32767); universe filter on Monitor (show all or filter by universe, "--" on mismatch); Blackout button; full undo/redo; Console is output-only
- **OSC Monitor** — scrolling message log showing the complete OSC message (address + every typed argument, not just the first); address substring filter; pause/clear; independent raw-capture path so multi-arg messages are never collapsed
- **UDP Monitor** — scrolling log of raw UDP packets with sender IP:port, byte count, and a hex/ASCII toggle display; pause/clear; same independent raw-capture rationale as OSC Monitor
- **MQTT Subscribe** — connects to a broker (via `libmosquitto`) and subscribes to a topic; host/port/topic/QoS/username/password settings; auto-generated unique client ID per instance
- **MQTT Publish** — publishes to a broker topic; topic can be overridden per-message from the incoming value (mirrors OSC Out's address override); numeric payload as plain decimal text; retain flag
- **MQTT Monitor** — pass-through display of MQTT topic+payload traffic; TIME/TOPIC/PAYLOAD scrolling log, pause/clear
- **MQTT Console** — manual topic+payload composer; topic field with a remembered-topics dropdown, numeric payload, explicit Send (button or Enter); pure value source, no broker connection of its own
- **Audio Player** — three source modes: audio file playback (WAV/AIFF always available; FLAC/OGG/MP3 also supported), a sine generator, or white/pink noise; static waveform display with click-to-seek and a moving playhead; log-scale frequency slider and dB-scale level slider (both default to a conservative, safety-conscious level); source node, output only
- **Pax system** — drop a `.dylib/.so/.dll` into the Pax folder; new node type appears in the sidebar on next launch
- **Dynamic port counts** — Pax can change their output port count at runtime (e.g. Spectrumyser band count) without audio interruption
- **Restructured burger menu** — `☰` top-right opens File and Edit flyout submenus with keyboard shortcuts
- **Hint panel** — hover any node, button, port or edge to see a description in the sidebar hint panel; hovering a Value edge (a Pax adapter/converter's generic value port) shows its current, live numeric value, updating continuously while hovered
- **Fold/Unfold** — double-click header to collapse nodes; edges merge gracefully to centre
- **WebView UI** — React + ReactFlow running inside JUCE's `WebBrowserComponent`; all UI logic is TypeScript, all audio logic is C++

---

## Project Structure

```
Patchy/
├── Source/                          Core C++ engine
│   ├── PatchyProcessor.h/.cpp       AudioProcessor — owns all state
│   ├── PatchyEditor.h/.cpp          AudioProcessorEditor + keyboard shortcuts
│   ├── WebBridge.h                  JS↔C++ bridge — class declaration (all 5 .cpp below implement it)
│   ├── WebBridge.cpp                Browser/WebView plumbing + constructor
│   ├── WebBridge_Dispatch.cpp       Message dispatcher — handleMessage() + all its handlers
│   ├── WebBridge_Push.cpp           Push-to-UI functions + 30fps timer
│   ├── WebBridge_FileIO.cpp         File save/open/new + Undo/Redo
│   ├── WebBridge_Fragments.cpp      Fragment export/import dialogs
│   ├── GraphModel.h/.cpp            UI data model (message thread)
│   ├── ProcessingGraph.h/.cpp       Topological sort + audio/MIDI routing
│   ├── NodeProcessor.h/.cpp         Abstract base — single + multi-port buffers
│   ├── MidiDeviceNodes.h/.cpp       MIDI In (type 1) + MIDI Out (type 2)
│   ├── AudioDeviceNodes.h/.cpp      Audio In (type 3) + Audio Out (type 4)
│   │                                Includes AudioDeviceManager + multi-channel FIFO
│   ├── UdpDeviceNodes.h/.cpp        UDP In (type 8) + UDP Out (type 9) + UdpDeviceManager
│   │                                Background socket thread, lock-free FIFO, byte-rate counter
│   ├── UdpMonitorNode.h/.cpp        UDP Monitor (type 21) + UdpMonitorBuffer
│   │                                Full-detail raw capture (sender IP/port), independent of routed PAX_Value
│   ├── MqttDeviceNodes.h/.cpp       MQTT Subscribe (22) + Publish (23) + Monitor (24) + Console (25) + MqttDeviceManager
│   │                                libmosquitto — first external C library dependency in the project
│   ├── OscDeviceNodes.h/.cpp        OSC In (type 10) + OSC Out (type 11) + OscDeviceManager
│   │                                Manual OscCodec (no juce_osc), OSC 1.0, byte-rate counter
│   ├── OscMonitorNode.h/.cpp        OSC Monitor (type 20) + OscMonitorBuffer
│   │                                Full-detail raw capture, independent of routed PAX_Value
│   ├── ArtNetDeviceNodes.h/.cpp     ArtNet In (type 12) + ArtNet Out (type 13) + ArtNetDeviceManager
│   │                                Manual ArtNetCodec, ArtDmx, port 6454, change-driven flash
│   ├── SerialPort.h                 Cross-platform serial port abstraction (POSIX + Win32, no deps)
│   ├── DmxDeviceNodes.h/.cpp        DMX In (type 14) + DMX Out (type 15) + DmxDeviceManager
│   │                                EnttecProCodec, Mk2 auto-detection, universe 0/1
│   ├── DmxMonitorNode.h/.cpp        DMX Monitor (type 16) + DmxMonitorBuffer
│   ├── DmxConsoleNode.h/.cpp        DMX Console (type 17); #includes DmxMonitorNode.h
│   ├── ArtNetMonitorNode.h/.cpp     ArtNet Monitor (type 18) + ArtNetMonitorBuffer; #includes DmxMonitorNode.h
│   ├── ArtNetConsoleNode.h/.cpp     ArtNet Console (type 19); #includes ArtNetMonitorNode.h
│   │                                DmxMonitorBuffer, vertical faders, 30Hz telemetry
│   ├── MidiMonitorNode.h/.cpp       MIDI Monitor (type 5)
│   ├── AudioMonitorNode.h/.cpp      Audio Monitor (type 6)
│   ├── MidiKeyboardNode.h           MIDI Keyboard (type 7)
│   ├── AudioPlayerNode.h/.cpp       Audio Player (type 26); file playback (WAV/AIFF/FLAC/OGG/MP3), sine, or noise
│   │                                AudioPlayerState survives graph rebuilds; log-scale frequency, dB level
│   └── StandaloneApp.h/.cpp         Standalone wrapper (window bounds, file location)
│
├── Pax/                          Pax ecosystem
│   ├── PaxAPI.h                   The ONLY header a Pax author needs
│   ├── PaxRegistry.h/.cpp         Loads Pax, owns DynamicLibrary handles
│   ├── PaxScanner.h/.cpp          Discovers Pax in platform folders
│   ├── LevelPax/                  Audio level control (-60dB to +6dB)
│   ├── AmpPax/                    Audio amplifier (0dB to +24dB)
│   ├── TransposePax/              MIDI transpose (-24 to +24 semitones)
│   ├── EnvelopePax/               Audio envelope → MIDI CC converter
│   ├── StereoSplitterPax/         Stereo → Left + Right split (1 in / 2 out)
│   ├── MqttToValuePax/            MQTT → generic Value adapter (first Phase 4 converter)
│   ├── OscToValuePax/             OSC → generic Value adapter
│   ├── ValueToDMXPax/             Generic Value → DMX channel adapter
│   ├── SpectrumyserPax/           FFT spectrum analyser with band outputs
│   ├── AudioToDmxPax/             Audio (RMS or isolated frequency band) → DMX channel, first Pax hosted inside a DAW
│   ├── MidiToDmxPax/              MIDI CC → DMX channel adapter
│   ├── UdpValueToMidiCCPax/       Generic Value → MIDI CC adapter
│   └── AudioPeakToOscPax/         Audio RMS/Peak → OSC float adapter
│
├── UI/                              React / TypeScript frontend
│   └── src/
│       ├── App.tsx                  ReactFlow canvas, graph sync, port activity, menus
│       ├── Bridge.ts                JS↔C++ typed façade + subscriber system
│       ├── NodeUtils.tsx            Shared hooks, components + style helpers (incl. isLikelyCompleteHost, portColour)
│       ├── GenericNode.tsx          Main shell for device nodes + Pax nodes (types 1–4, 8–15, 22–23, 100+)
│       ├── AudioDeviceUI.tsx        Audio In/Out settings panel + summary
│       ├── UdpDeviceUI.tsx          UDP In/Out settings panel + summary
│       ├── OscDeviceUI.tsx          OSC In/Out settings panel + summary
│       ├── MqttDeviceUI.tsx         MQTT Subscribe/Publish settings panels + summaries
│       ├── ArtNetDeviceUI.tsx       ArtNet In/Out settings panel + summary
│       ├── DmxDeviceUI.tsx          DMX In/Out settings panel + summary
│       ├── MidiMonitorNode.tsx      MIDI Monitor node (type 5)
│       ├── AudioMonitorNode.tsx     Audio Monitor node (type 6)
│       ├── MidiKeyboardNode.tsx     MIDI Keyboard node (type 7)
│       ├── DmxShared.tsx            Shared DMX types, constants, DmxFader, DmxSettingsPanel, NameInput
│       ├── DmxMonitorNode.tsx       DMX Monitor node (type 16)
│       ├── DmxConsoleNode.tsx       DMX Console node (type 17)
│       ├── ArtNetMonitorNode.tsx    ArtNet Monitor node (type 18)
│       ├── ArtNetConsoleNode.tsx    ArtNet Console node (type 19)
│       ├── OscMonitorNode.tsx       OSC Monitor node (type 20) — scrolling log, full multi-arg display
│       ├── UdpMonitorNode.tsx       UDP Monitor node (type 21) — scrolling log, hex/ASCII toggle
│       ├── MqttMonitorNode.tsx      MQTT Monitor node (type 24) — scrolling log, TIME/TOPIC/PAYLOAD
│       ├── MqttConsoleNode.tsx      MQTT Console node (type 25) — manual topic+payload composer
│       ├── AudioPlayerNode.tsx      Audio Player node (type 26) — waveform + click-to-seek, log-scale frequency, dB level
│       ├── SpectrumyserNode.tsx     Spectrumyser custom node with FFT canvas
│       ├── EnvelopeNode.tsx         Envelope custom node with live canvas
│       ├── HintPanel.tsx            Hint context, panel, and hint dictionaries
│       ├── Sidebar.tsx              Node palette + hint panel
│       ├── NodeSelect.tsx           Shared custom combobox with hint + warning support
│       ├── DawContext.ts            DAW mode context (loopback + host device toggles)
│       └── PreferencesPanel.tsx     Graph preferences (DAW routing, audio settings)
│
├── FYI/                             Developer notes (gitignored)
│   ├── Architecture.md             Detailed technical architecture + design decisions
│   ├── SessionLog.md               Full chronological dev diary — bug hunts, refactors, commit messages
│   └── Utils/
│       └── migrate_patch_ids.py    Migrate .patchy files: legacy node IDs to current format
│
├── Tools/                           Developer utilities
│   └── migrate_patchy_v1_to_v2.py  Migrate .patchy files: addonName→paxName
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
| 8 | UDP In Device | UDP Out | Listens on a UDP port; Unicast · Multicast · Broadcast; live byte-rate |
| 9 | UDP Out Device | UDP In | Sends datagrams to a configured host:port; Unicast · Multicast · Broadcast |
| 10 | OSC In Device | OSC Out | Listens on a UDP port; parses OSC 1.0 messages; live byte-rate |
| 11 | OSC Out Device | OSC In | Sends PAX_Value events as OSC messages to a configured host:port; configurable OSC address |
| 12 | ArtNet In Device | ArtDMX Out | Listens on UDP port 6454; parses ArtDmx; universe filtering; change-driven flash; live byte-rate |
| 13 | ArtNet Out Device | ArtDMX In | Sends PAX_Value blobs as ArtDmx packets to a configured host; configurable universe |
| 14 | DMX In Device | DMX Out | Receives DMX512 from an Enttec DMX USB Pro; serial port selector; Mk2 auto-detection; live byte-rate |
| 15 | DMX Out Device | DMX In | Sends DMX512 to an Enttec DMX USB Pro; universe 0 (Pro) or 1 (Mk2 port 2) |
| 16 | DMX Monitor | DMX In + DMX Out | Displays all 512 DMX channels as vertical bargraphs; pass-through; configurable visible count |
| 17 | DMX Console | DMX Out | 512-channel vertical fader bank; blackout; configurable visible count (8/16/24/32); page navigation; dec/pct/hex format; custom name; output-only (no input port) |
| 18 | ArtNet Monitor | ArtDMX In + ArtDMX Out | Displays all 512 ArtNet channels as vertical bargraphs; pass-through; universe filter; "--" on mismatch |
| 19 | ArtNet Console | ArtDMX Out | 512-channel vertical fader bank; universe selector (0–32767); blackout; configurable visible count; output-only |
| 20 | OSC Monitor | OSC In + OSC Out | Scrolling log of complete OSC messages (address + every typed arg); pass-through; address substring filter; pause/clear |
| 21 | UDP Monitor | UDP In + UDP Out | Scrolling log of raw UDP packets (sender IP:port, byte count, hex/ASCII toggle); pass-through; pause/clear |
| 22 | MQTT Subscribe | MQTT Out | Connects to a broker and subscribes to a topic (`libmosquitto`); host/port/topic/QoS/username/password; source node, output only |
| 23 | MQTT Publish | MQTT In | Publishes to a broker topic (`libmosquitto`); topic overridable per-message from incoming value; numeric payload as plain decimal text; retain flag; sink node, input only |
| 24 | MQTT Monitor | MQTT In + MQTT Out | Pass-through display of MQTT topic+payload traffic; TIME/TOPIC/PAYLOAD scrolling log |
| 25 | MQTT Console | MQTT Out | Manual topic+payload composer; topic history dropdown, explicit Send; source node, output only, no broker connection of its own |
| 26 | Audio Player | Audio Out | File playback (WAV/AIFF/FLAC/OGG/MP3), sine generator, or white/pink noise; static waveform with click-to-seek, log-scale frequency slider, dB level slider; source node, output only |
| 100+ | Pax nodes | Per descriptor | Dynamically loaded from `.dylib/.so/.dll` |

---

## Signal Flow Visualisation

All ports and edges animate live at 30fps:

**MIDI activity** — flashes bright cyan-white (80ms) on OUT port, edge and downstream IN port

**UDP activity** — flashes steel blue (80ms) on UDP Out port, edge and downstream IN port; live byte-rate label (B/s or kB/s) displayed inline on UDP In nodes while packets are flowing

**OSC activity** — flashes cyan/teal (80ms) on OSC Out port, edge and downstream IN port; live byte-rate label displayed inline on OSC In nodes while messages are arriving

**ArtNet activity** — flashes pale amber (80ms) on ArtDMX Out port, edge and downstream IN port; change-driven (no flash on 44Hz heartbeat refresh, only on DMX value changes); live byte-rate label on ArtNet In nodes

**MQTT activity** — flashes coral/salmon (80ms) on MQTT Out port, edge and downstream IN port; applies to Subscribe, Publish, Monitor and Console alike

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

## Pax System

Pax are shared libraries implementing the `PAX_Descriptor` C API in `Pax/PaxAPI.h`. Discovered at startup by `PaxScanner`, loaded by `PaxRegistry`.

### Pax folder locations

| Platform | Path |
|----------|------|
| macOS | `~/Library/Patchy/Pax/` |
| Windows | `%APPDATA%\Patchy\Pax\` |
| Linux | `~/.patchy/pax/` |

### Bundled Pax

| Pax | Type | Ports | Parameters |
|-------|------|-------|------------|
| Level | Audio | 1in/1out | Level: -60dB to +6dB |
| Amp | Audio | 1in/1out | Amp: 0dB to +24dB |
| Transpose | MIDI | 1in/1out | Semitones: -24 to +24 |
| Envelope | AV Hybrid | 1m+1a in / 1m+1a out | Mode, CC, Attack, Release, Band filters |
| Splitter | Audio | 1in/2out | — (L→out1, R→out2) |
| Spectrumyser | Audio | 1in/1-5out | Band count (1-5), per-band frequency range |
| MQTT to Value | Converter | 1 MQTT in / 1 Value out | — (stateless passthrough; first Phase 4 adapter, bridges MQTT payloads into the generic Value graph) |
| OSC to Value | Converter | 1 OSC in / 1 Value out | Current Value (read-only live display) |
| Value to DMX | Converter | 1 Value in / 1 DMX out | DMX Channel (1-512), Input Min/Max (default 0.0/1.0), Current Value (read-only live display) |
| Audio to DMX | Converter | 1 Audio in / 1 DMX out | Mode (RMS/Freq Range), Sensitivity (dB), Damping (0-500ms), DMX Channel (1-512); first Pax hosted inside a DAW, not just standalone |
| MIDI to DMX | Converter | 1 MIDI in / 1 DMX out | MIDI CC (0-127), DMX Channel (1-512), Current Value (read-only live display) |
| UDP Value to MIDI CC | Converter | 1 Value in / 1 MIDI out | MIDI CC (0-127), MIDI Channel (1-16), Input Min/Max (default 0.0/1.0), MIDI CC Value (read-only live display) |
| Audio Peak to OSC | Converter | 1 Audio in / 1 OSC out | Mode (Whole/Freq Range), Measurement (RMS/Peak), Sensitivity (dB), Band Low/High (Hz), Damping (0-500ms), Send Mode (Change/Rate), Max Rate (1-100Hz); Measurement is fully orthogonal to Mode, giving all four combinations; Send Mode controls per-node whether messages only go out on genuine change or continuously at a throttled rate |

### Parameter persistence

Pax parameters are automatically saved in `settingsJson` on every change and restored when:
- A patch file is loaded
- The graph is rebuilt (adding/connecting nodes)
- The app is restarted (via DAW project state or patch file)

### Dynamic port counts

Pax can change their output port count at runtime by exporting `PAX_getAudioOutputCount`. Patchy updates the node's ports and routing live — without a full graph rebuild or audio interruption — only when the count actually changes.

### Building a Pax

Recommended — `Pax/build_pax.sh` (macOS/Linux) or `Pax/build_pax.bat` (Windows) builds every bundled Pax in one go:

```bash
cd Pax
./build_pax.sh              # build only, binaries land in Pax/build/pax/
./build_pax.sh --install    # build + copy straight to the Pax folder for your platform
./build_pax.sh --clean      # wipe the build directory first
```

Manual single-Pax build (what the script does under the hood, macOS example):

```bash
cd Pax/SpectrumyserPax
clang++ -std=c++20 -shared -fPIC SpectrumyserPax.cpp \
        -o SpectrumyserPax.dylib
cp SpectrumyserPax.dylib ~/Library/Patchy/Pax/
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
| `Space` | Play / Pause the selected Audio Player node |
| `Space Space` (within 400ms) | Return the selected Audio Player node to the start |
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
- `libmosquitto` (MQTT support — macOS: `brew install mosquitto`; the build fails with a clear error if not found)

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

## Writing a Pax

Include only `Pax/PaxAPI.h`. No JUCE dependency required.

```cpp
#include "PaxAPI.h"
#include <cstring>

struct MyPax {};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor() {
    static PAX_Descriptor d {
        "My Pax", "My Studio", "1.0.0",
        2,               // nodeType: 1=MIDI, 2=Audio, 3=AV, 4=Value only
        PAX_API_VERSION,
        1, 1, 0, 0       // audioIn, audioOut, midiIn, midiOut
    };
    return &d;
}

PAX_Instance* PAX_create()           { return new MyPax(); }
void PAX_destroy (PAX_Instance* i)   { delete (MyPax*)i; }
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

    // Value ports — live, routed by the host the same way audio/MIDI are:
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
| `PAX_getValueInputCount` / `PAX_getValueOutputCount` | Return Value port counts (nodeType 4, cross-protocol adapters) — static, no instance needed, fixed at scan time |
| `PAX_getValueInputType` / `PAX_getValueOutputType` | Return the specific type (`PAX_VALUETYPE_*` — MQTT/OSC/DMX/UDP/ArtNet/MIDI/generic) of the Value port at a given index, so a Pax can mix multiple differently-typed ports on one node rather than only generic Value — indexed, static, defaults to generic if not exported |
| `PAX_isParameterReadOnly` | Render a parameter as a live-updating display rather than a draggable control — the host still calls `PAX_getParameter()` to read it, but never calls `PAX_setParameter()` on it from user interaction. For values a Pax wants to surface for visibility (e.g. the last value it received on a Value input) without inviting the user to edit what's actually just a live mirror of incoming data |
| `PAX_isParameterLiveSynced` | Mark an *editable* parameter as also live-synced to the frontend — stays a normal, draggable control, but when the Pax changes its value internally (via `PAX_setParameter` from its own `PAX_process()`, not user interaction), the host also pushes that value to the frontend live so the control's on-screen position visually follows it. Deliberately opt-in per parameter, not a blanket watch-everything mechanism, to avoid racing a user's own in-progress drag on an unrelated control |
| `PAX_getColourCategory` | Override the node's auto-detected colour category (Hybrid/Converter/native-type) — only consulted for a pure-source or pure-sink Pax, the one case the automatic rule can't resolve on its own |
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
    int         nodeType;    // 1=MIDI, 2=Audio, 3=AV, 4=Value only (see PAX_getValueInputCount/OutputCount below)
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

### PAX_Value  *(new in API v2, gained `portIndex` in API v3)*

```c
typedef struct {
    uint32_t key;        // Integer key resolved from name at setup time
    uint8_t  type;       // Domain: PAX_TYPE_GENERIC/DMX/OSC/MQTT/UDP/ARTNET
    uint8_t  dataType;   // PAX_DATA_FLOAT / PAX_DATA_STRING / PAX_DATA_BLOB
    uint16_t dataSize;   // Byte length of data[] when dataType != PAX_DATA_FLOAT
    float    value;      // Primary payload (default)
    uint8_t  data[56];   // Inline buffer for strings/blobs
    uint8_t  portIndex;  // Which declared Value output port this belongs to
                          // (API v3) — 0 is always a safe default for a Pax
                          // with only one Value output; only matters once
                          // PAX_getValueOutputCount() > 1
} PAX_Value;
```

### PAX_ProcessContext  *(new in API v2, gained a DMX universe path in API v4)*

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
    const PAX_Value*     valuesIn;
    int                  valueInCount;
    PAX_Value*           valuesOut;
    int*                 valueOutCount;
    int                  valueMaxCount;

    // DMX universe (API v4) — a separate wide-payload path from Values
    // above; PAX_Value.data[] is only 56 bytes, nowhere near enough for a
    // full 512-channel universe. Always 512 bytes when non-NULL. Host
    // zero-fills dmxFrameOut and clears *dmxFrameOutValid before each
    // block, so a Pax only needs to touch the channel(s) it actually
    // writes, not all 512. See `AudioToDmxPax` for a worked example.
    const uint8_t* dmxFrameIn;
    bool           dmxFrameInValid;
    uint8_t*       dmxFrameOut;
    bool*          dmxFrameOutValid;
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
| `AudioPlayerNode` seek | Message → Audio | Request/consume: UI writes a target sample index to a separate atomic slot; only the audio thread ever writes the actual playback position, consuming (and clearing) the request at the top of its own block — avoids a read-modify-write race a plain cross-thread store into the position itself would hit |
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
| Pax API (`PaxAPI.h`) | Free | MIT — no strings attached |
| Bundled example Pax | Free | MIT — use as reference |

Pax developers are free to license their Pax under any terms — proprietary, MIT, GPL, or anything else.

---

*Patchy v0.0.904 — JUCE 8 · React 19 · ReactFlow · Vite · TypeScript · Lucide*
