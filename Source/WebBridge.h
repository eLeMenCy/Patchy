#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>
#include "GraphModel.h"
#include "../Addons/AddonRegistry.h"

#include "MidiMonitorNode.h"
#include "AudioMonitorNode.h"

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
                       AddonRegistry* registry = nullptr,
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

    // Called directly from C++ (e.g. keyboard shortcuts in PatchyEditor)
    void handleFileNew();
    void handleFileOpen();
    void handleFileSave();
    void handleFileSaveAs();
    void pushToUI (const juce::String& bridgeFn, juce::String json);
    bool isStandalone = false;  // true only in standalone app
    std::function<std::vector<SpectrumSnapshot>()> getSpectrumSnapshots;
    std::function<int(const juce::String&)>        onGetAddonAudioOutCount;
    std::function<void(const juce::String&)>       onPruneAddonEdges;
    std::function<void(double, int, bool)> onSetAudioEngineSettings;
    std::function<void()>                  onUIReady;

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
    AddonRegistry*  registry         = nullptr;
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

    void pushAddonList();

    // ── File operations ───────────────────────────────────────────────────────
    void saveToFile     (const juce::File& file);
    void showSaveDialog ();
    void showOpenDialog ();
    juce::String buildFileStateJson ();

    juce::File currentFile;
    juce::File lastOpenDir;

    std::function<void()>                    onNewGraph;
    std::function<void(const juce::String&)> onLoadGraph;
    void pushMidiDevices();
    void pushAudioDevices();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WebBridge)
};
