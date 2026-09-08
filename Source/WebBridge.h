#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include <vector>
#include <atomic>
#include "GraphModel.h"
#include "../Pax/PaxRegistry.h"

#include "MidiMonitorNode.h"
#include "AudioMonitorNode.h"
#include "ArtNetConsoleNode.h"
#include "OscMonitorNode.h"
#include "UdpMonitorNode.h"
#include "MqttDeviceNodes.h"

/**
 * WebBridge
 *
 * Follows the exact pattern from JUCE's own WebViewPluginDemo.h and
 * Jan Wilczek's tutorial:
 *
 *  Release  – assets are bundled as a zip in BinaryData.
 *              A resource provider opens the zip via juce::ZipFile and
 *              serves each file by path.
 *              goToURL(WebBrowserComponent::getResourceProviderRoot()) loads the UI.
 *
 *  Dev mode – goToURL("http://localhost:5173") directly.
 *              pageAboutToLoad only allows the dev server URL and the
 *              resource root to prevent accidental navigation.
 */
// Per-node port activity — sent at 30fps alongside audio snapshots
// Small, dedicated status struct for AudioPlayerNode's own periodic push —
// deliberately kept separate from PortActivity below (which already covers
// every other node type) rather than extending it, to avoid any risk to
// that larger, already-established mechanism.
struct AudioPlayerStatus
{
    juce::String nodeId;
    double       fraction = 0.0;   // playhead position, 0.0-1.0
    bool         playing  = false;
};

// Reports a fresh file load that happened via the restore-on-rebuild path
// (project reload, or a graph rebuild after a settings commit) — that path
// loads the file correctly on the backend directly, with no way to push an
// event to the UI itself, unlike the manual browse path's own dedicated
// callback. Collected and consumed (flag cleared) once per load by
// PatchyProcessor::collectPendingAudioPlayerFileLoads(), then reported to
// the frontend the same way the manual browse path's own result already is.
struct AudioPlayerFileLoadedInfo
{
    juce::String nodeId;
    juce::String fileName;
    juce::String filePath;
    std::vector<std::pair<float, float>> peaks;
    int    numSamples = 0;
    double sourceSampleRate = 0.0;
};

struct PortActivity
{
    juce::String      nodeId;
    int               midiOutEvents = 0;   // MIDI events since last push
    float             audioRmsL     = 0.f;
    float             audioRmsR     = 0.f;
    std::vector<float> portRms;           // per-output-port RMS for multi-port nodes
    // For keyboard nodes: active notes in inputMidi this frame
    std::vector<std::pair<uint8_t,uint8_t>> incomingNotes; // {status, note}
    int               udpBytes      = 0;   // bytes received since last push (UDP In nodes only)
    bool              dmxIsMk2      = false; // true if Enttec Pro Mk2 detected
    // Current DMX channel level (0.0-1.0), read straight from the node's own
    // lightweight PAX_Value mirror (outputValues[0].value when its type is
    // PAX_TYPE_DMX) — works uniformly for built-in DMX nodes and any DMX-
    // emitting Pax alike, no per-class special-casing needed. Persists
    // between change-detected updates (built-in nodes only touch
    // outputValues[0] on an actual change, leaving the slot's last real
    // value in place otherwise), so this reflects "current channel level",
    // not "did something just happen" — the two are different questions,
    // see Architecture.md for why that distinction matters here.
    float             dmxValue      = 0.f;
    // {parameterIndex, currentValue} for each of this node's read-only
    // parameters (see PaxAPI.h's PAX_isParameterReadOnly) — empty for
    // every node except a Pax that has at least one. Deliberately
    // separate from the editable-parameter sync path (settingsJson),
    // which the user may be actively dragging — this only ever carries
    // values nothing in the UI lets the user set, so there's no risk of
    // this live poll racing against a user's own in-progress edit.
    std::vector<std::pair<int, float>> paxReadOnlyValues;
    // Set (to the node's freshly-updated settingsJson) when a parameter
    // marked live-synced (see PaxAPI.h's PAX_isParameterLiveSynced) has
    // just been changed by the Pax's own backend logic, not by the user —
    // e.g. UdpValueToMidiCCPax's "MIDI CC" following an incoming 5-byte
    // packet's own CC-number override. Empty otherwise (the overwhelming
    // majority of pushes). Deliberately narrow, opt-in per parameter —
    // unlike paxReadOnlyValues above, this DOES go through the same
    // settingsJson path a user's own edits use, since the point here is
    // moving a still-editable slider's on-screen position, not a
    // display-only mirror — so only a Pax that explicitly opts a specific
    // parameter in can ever trigger this, keeping every other parameter
    // free of any risk of racing a user's own in-progress drag.
    juce::String      liveSyncedSettingsJson;
};

struct SpectrumSnapshot
{
    juce::String       nodeId;
    double             sampleRate  = 44100.0;
    int                bandCount   = 3;
    std::vector<float> magnitudes; // FFT_SIZE/2 magnitude bins
    std::vector<float> bandLow;    // Hz
    std::vector<float> bandHigh;   // Hz
};

// Free struct — used by both WebBridge and PatchyProcessor
struct AudioSnapshot
{
    juce::String        nodeId;
    double              sampleRate = 48000.0;
    std::vector<float>  left;
    std::vector<float>  right;
};

class WebBridge : public juce::Component,
                  private juce::Timer
{
public:
    explicit WebBridge (GraphModel& model,
                       PaxRegistry* registry = nullptr,
                       std::function<void(const juce::String&, const juce::String&)> onSetMidiDevice  = nullptr,
                       std::function<void(const juce::String&, const juce::String&)> onSetAudioDevice = nullptr,
                       std::function<std::vector<MidiMonitorBatch>()> drainMonitor = nullptr,
                       std::function<std::vector<AudioSnapshot>()> getAudioSnapshots = nullptr,
                       std::function<void()>                      clearGraphTrash   = nullptr,
                       std::function<std::vector<PortActivity>()>  getPortActivity   = nullptr,
                       std::function<void(const juce::String&, uint8_t, uint8_t, uint8_t)> onMidiKeyEvent = nullptr,
                       std::function<void(const juce::String&, const juce::String&)>          onSetNodeLabel = nullptr,
                       std::function<void(const juce::String&, int, float)>                      onSetPaxParameter = nullptr,
                       std::function<void()>                                                          onNewGraph          = nullptr,
                       std::function<void(const juce::String&)>                                       onLoadGraph         = nullptr);
    ~WebBridge() override { stopTimer(); }

    void resized() override;
    void loadUI();
    void pushGraphToUI();
    void pushUndoState();
    void pushSettingsToUI (const juce::String& nodeId, const juce::String& settingsJson);

    /** Snapshot captured at the start of a slider drag — pushed on commitNodeSettings. */
    juce::var  pendingSettingsSnapshot;
    juce::String pendingSettingsNodeId;

    // Called directly from C++ (e.g. keyboard shortcuts in PatchyEditor)
    void handleFileNew();
    void handleFileOpen();
    void handleFileSave();
    void handleFileSaveAs();
    void handleUndo();
    void handleRedo();
    void setLastOpenDir (const juce::File& dir) { lastOpenDir = dir; }
    juce::File getLastOpenDir() const { return lastOpenDir; }
    void pushToUI (const juce::String& bridgeFn, juce::String json);
    bool isStandalone = false;  // true only in standalone app
    std::function<std::vector<SpectrumSnapshot>()> getSpectrumSnapshots;
    std::function<std::vector<AudioPlayerStatus>()> getAudioPlayerStatuses;
    std::function<std::vector<AudioPlayerFileLoadedInfo>()> getPendingAudioPlayerFileLoads;
    std::function<int(const juce::String&)>        onGetPaxAudioOutCount;
    std::function<void(const juce::String&)>       onPrunePaxEdges;
    std::function<void(double, int, bool)> onSetAudioEngineSettings;
    std::function<void()>                  onUIReady;
    std::function<void(const juce::String&, const std::vector<int>&)> onSetAudioDeviceChannels;
    std::function<void(const juce::String&, int, int, const juce::String&, const juce::String&)> onSetUdpSettings;
    std::function<void(const juce::String&, int, const juce::String&, const juce::String&)>      onSetOscSettings;
    std::function<void(const juce::String&, const juce::String&, int, const juce::String&, int,
                       const juce::String&, const juce::String&)>                                onSetMqttSubscribeSettings;
    std::function<void(const juce::String&, const juce::String&, int, const juce::String&, int, bool,
                       const juce::String&, const juce::String&)>                                onSetMqttPublishSettings;
    /** MQTT Console's Send action — a discrete trigger, not a persisted
     *  setting like the two above (nodeId, topic, payload). */
    std::function<void(const juce::String&, const juce::String&, float)>                         onMqttConsoleSend;
    std::function<void(const juce::String&, int, const juce::String&)>                           onSetArtNetSettings;
    std::function<void(const juce::String&, const juce::String&, int)>                          onSetDmxSettings;
    std::function<std::vector<DmxSnapshot>()>                                                    drainDmxSnapshots;
    std::function<void(const juce::String&, int, uint8_t)>                                       onSetDmxConsoleChannel;
    // AudioPlayerNode's own controls — playback (play/pause/stop/seek/
    // return-to-start) is real-time/discrete, handled separately from the
    // small, discrete settings (mode/frequency/noise type/level/loop),
    // which flow through the existing generic setNodeSettings mechanism
    // (see React's own AudioPlayerNode.tsx) exactly like AudioMonitorNode's
    // own settings already do — no new mechanism needed for those.
    std::function<void(const juce::String&, const juce::String&)>                                onAudioPlayerLoadFile;   // (nodeId, chosen file path) — called AFTER the async chooser resolves
    std::function<void(const juce::String&)>                                                      onAudioPlayerRequestFileInfo;   // direct pull, no timer/rebuild involved
    std::function<void(const juce::String&, bool)>                                                onAudioPlayerSetPlaying;
    std::function<void(const juce::String&, double)>                                              onAudioPlayerSeek;       // 0.0-1.0 fraction of the loaded file
    std::function<void(const juce::String&)>                                                      onAudioPlayerReturnToStart;
    std::function<void(const juce::String&, const juce::String&, const juce::String&)>             onAudioPlayerSetLiveParam;   // (nodeId, key, value)
    std::function<void(const juce::String&, bool)>                                               onSetDmxBlackout;
    std::function<void(const juce::String&, bool)>                                               onRestoreDmxBlackout;
    std::function<void(const juce::String&, const juce::String&)>                                onRestoreDmxConsoleChannels;
    std::function<void(const juce::String&)>                                                     onResetDmxConsoleChannels;

    // ArtNet Console callbacks (mirrors DMX Console pattern)
    std::function<std::vector<ArtNetSnapshot>()>                                                 drainArtNetSnapshots;
    std::function<void(const juce::String&, int, uint8_t)>                                       onSetArtNetConsoleChannel;
    std::function<void(const juce::String&, bool)>                                               onSetArtNetBlackout;
    std::function<void(const juce::String&, bool)>                                               onRestoreArtNetBlackout;
    std::function<void(const juce::String&, const juce::String&)>                                onRestoreArtNetConsoleChannels;
    std::function<void(const juce::String&)>                                                     onResetArtNetConsoleChannels;
    std::function<void(const juce::String&, int)>                                                onSetArtNetUniverseFilter;

    // OSC Monitor
    std::function<std::vector<OscMonitorBatch>()>                                                 drainOscMonitor;

    // UDP Monitor
    std::function<std::vector<UdpMonitorBatch>()>                                                 drainUdpMonitor;
    std::function<std::vector<MqttMonitorBatch>()>                                                drainMqttMonitor;

private:
    // ── Resource provider (release) ───────────────────────────────────────
    std::optional<juce::WebBrowserComponent::Resource>
    getResource (const juce::String& url);

    // ── SinglePageBrowser — locks navigation to our app URL ───────────────
    struct Browser final : public juce::WebBrowserComponent
    {
        explicit Browser (WebBridge& ownerRef,
                          juce::WebBrowserComponent::Options options)
            : juce::WebBrowserComponent (std::move (options)),
              owner (ownerRef) {}

        bool pageAboutToLoad (const juce::String& newURL) override;

        WebBridge& owner;
    };

    // ── Message handler ───────────────────────────────────────────────────
    void handleMessage (const juce::String& json);

    // Extracted from handleMessage()'s dispatch chain (2026-08-09) — each
    // was a branch body inline in that one giant function; now a named
    // method per message type, verified via exact string-literal-aware
    // brace-matched extraction (no logic changes, purely mechanical). Kept
    // in the same relative order as the dispatch chain in handleMessage()
    // itself, for easy side-by-side navigation.
    void handleReady ();
    void handleAddNode (const juce::DynamicObject* obj);
    void handleSetPaxParameter (const juce::DynamicObject* obj);
    void handleSetNodeSettings (const juce::DynamicObject* obj);
    void handleCommitNodeSettings ();
    void handleCommitSettingsChange (const juce::DynamicObject* obj);
    void handleSetNodeLabel (const juce::DynamicObject* obj);
    void handleMidiKeyEvent (const juce::DynamicObject* obj);
    void handleRemoveNode (const juce::DynamicObject* obj);
    void handleAddConnection (const juce::DynamicObject* obj);
    void handleRemoveConnection (const juce::DynamicObject* obj);
    void handleMoveNode (const juce::DynamicObject* obj);
    void handleSetViewport (const juce::DynamicObject* obj);
    void handleSetNodeParam (const juce::DynamicObject* obj);

    // Extracted from handleSetNodeParam()'s own internal nested dispatch on
    // `key` (2026-08-09, Phase B — same method as the top-level extraction
    // above). Five of these preserve an early `return;` in the dispatcher
    // itself (dmxConsoleChannel/dmxBlackout/artNetConsoleChannel/
    // artNetBlackout/artNetUniverseFilter all deliberately skip the trailing
    // pushGraphToUI()/pushUndoState() calls in the original) — verified
    // against the exact original control flow, not just the extracted
    // bodies, before this was applied.
    void handleSetNodeParam_MidiDeviceId (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_AudioDeviceId (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_AudioDeviceChannels (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_UdpSettings (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_OscSettings (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_MqttSubscribeSettings (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_MqttPublishSettings (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_MqttConsoleSend (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_ArtNetSettings (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_DmxSettings (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_DmxConsoleChannel (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_DmxBlackout (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_ArtNetConsoleChannel (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_ArtNetBlackout (const juce::String& nodeId, const juce::String& value);
    void handleSetNodeParam_ArtNetUniverseFilter (const juce::String& nodeId, const juce::String& value);
    void handleExportSelection (const juce::DynamicObject* obj);
    void handleImportFragmentNodes (const juce::DynamicObject* obj);
    void handleSetAudioEngineSettings (const juce::DynamicObject* obj);

    GraphModel& graph;

    // Owned zip data (release mode) – keep alive as long as the browser lives
    std::unique_ptr<juce::MemoryInputStream> zipStream;
    std::unique_ptr<juce::ZipFile>           zipFile;

    std::unique_ptr<Browser> webView;

    bool         connected = false;
    juce::String devServerUrl;
    PaxRegistry*  registry         = nullptr;
    std::function<std::vector<MidiMonitorBatch>()> drainMonitor;

    std::function<std::vector<AudioSnapshot>()>    getAudioSnapshots;
    std::function<std::vector<PortActivity>()>  getPortActivity;
    std::function<void()> clearGraphTrash;
    std::function<void(const juce::String&, uint8_t, uint8_t, uint8_t)> onMidiKeyEvent;
    std::function<void(const juce::String&, const juce::String&)>          onSetNodeLabel;
    std::function<void(const juce::String&, int, float)> onSetPaxParameter;
    std::function<void(const juce::String&, const juce::String&)> onSetMidiDevice;
    std::function<void(const juce::String&, const juce::String&)> onSetAudioDevice;

    void timerCallback() override;
    void pushMidiMonitorEvents();
    void pushAudioSnapshots();
    void pushSpectrumSnapshots();
    void pushPortActivity();
    void pushAudioPlayerStatus();
    void pushPendingAudioPlayerFileLoads();
    void pushDmxSnapshots();
    void pushArtNetSnapshots();
    void pushOscMonitorEvents();
    void pushUdpMonitorEvents();
    void pushMqttMonitorEvents();

    void pushPaxList();

    // ── File operations ───────────────────────────────────────────────────────
    void saveToFile     (const juce::File& file);
    void showSaveDialog ();
    void showOpenDialog ();
    juce::String buildFileStateJson ();
    void handleAudioPlayerLoadFile (const juce::DynamicObject* obj);
    void handleAudioPlayerRequestFileInfo (const juce::DynamicObject* obj);

    // ── Fragment export / import ──────────────────────────────────────────────
    void showExportDialog (const juce::StringArray& selectedNodeIds,
                           const juce::String& suggestedName);
    void showImportDialog ();
    /** Remap all node/connection/port IDs to fresh UUIDs and strip
     *  connections that cross the fragment boundary. */
    static juce::var remapFragmentIds (const juce::var& fragment,
                                       const juce::StringArray& selectedNodeIds);

    juce::File currentFile;
    juce::File lastOpenDir;

    std::function<void()>                    onNewGraph;
    std::function<void(const juce::String&)> onLoadGraph;
    void pushMidiDevices();
    void pushAudioDevices();
    void pushSerialPorts();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebBridge)
};
