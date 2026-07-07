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
                       std::function<void(const juce::String&, int, float)>                      onSetAddonParameter = nullptr,
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
    std::function<int(const juce::String&)>        onGetPaxAudioOutCount;
    std::function<void(const juce::String&)>       onPrunePaxEdges;
    std::function<void(double, int, bool)> onSetAudioEngineSettings;
    std::function<void()>                  onUIReady;
    std::function<void(const juce::String&, const std::vector<int>&)> onSetAudioDeviceChannels;
    std::function<void(const juce::String&, int, int, const juce::String&, const juce::String&)> onSetUdpSettings;
    std::function<void(const juce::String&, int, const juce::String&, const juce::String&)>      onSetOscSettings;
    std::function<void(const juce::String&, int, const juce::String&)>                           onSetArtNetSettings;
    std::function<void(const juce::String&, const juce::String&, int)>                          onSetDmxSettings;
    std::function<std::vector<DmxSnapshot>()>                                                    drainDmxSnapshots;
    std::function<void(const juce::String&, int, uint8_t)>                                       onSetDmxConsoleChannel;
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
    std::function<void(const juce::String&, int, float)> onSetAddonParameter;
    std::function<void(const juce::String&, const juce::String&)> onSetMidiDevice;
    std::function<void(const juce::String&, const juce::String&)> onSetAudioDevice;

    void timerCallback() override;
    void pushMidiMonitorEvents();
    void pushAudioSnapshots();
    void pushSpectrumSnapshots();
    void pushPortActivity();
    void pushDmxSnapshots();
    void pushArtNetSnapshots();
    void pushOscMonitorEvents();

    void pushPaxList();

    // ── File operations ───────────────────────────────────────────────────────
    void saveToFile     (const juce::File& file);
    void showSaveDialog ();
    void showOpenDialog ();
    juce::String buildFileStateJson ();

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
