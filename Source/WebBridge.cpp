#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"

#if HAS_BUNDLED_UI
  #include "BinaryData.h"
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static juce::String mimeFor (const juce::String& path)
{
    if (path.endsWith (".html")) return "text/html";
    if (path.endsWith (".js"))   return "application/javascript";
    if (path.endsWith (".css"))  return "text/css";
    if (path.endsWith (".svg"))  return "image/svg+xml";
    if (path.endsWith (".png"))  return "image/png";
    if (path.endsWith (".ico"))  return "image/x-icon";
    if (path.endsWith (".woff")) return "font/woff";
    if (path.endsWith (".woff2"))return "font/woff2";
    return "application/octet-stream";
}

static std::vector<std::byte> streamToVector (juce::InputStream& in)
{
    juce::MemoryOutputStream out;
    out.writeFromInputStream (in, -1);
    const auto& block = out.getMemoryBlock();
    std::vector<std::byte> result(block.getSize());
    std::memcpy(result.data(), block.getData(), block.getSize());
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Browser::pageAboutToLoad
//  Only allow our app URL and the resource provider root through.
// ─────────────────────────────────────────────────────────────────────────────

bool WebBridge::Browser::pageAboutToLoad (const juce::String& newURL)
{
    // Always allow the resource provider root (release mode)
    if (newURL == getResourceProviderRoot())
        return true;

    // Allow the Vite dev server (dev mode)
    if (owner.devServerUrl.isNotEmpty() && newURL.startsWith (owner.devServerUrl))
        return true;

    // Block everything else (prevents the webview navigating away)
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Resource provider (release mode)
//  Opens the bundled zip and serves files by their path inside the archive.
// ─────────────────────────────────────────────────────────────────────────────

std::optional<juce::WebBrowserComponent::Resource>
WebBridge::getResource (const juce::String& url)
{
    if (zipFile == nullptr)
        return std::nullopt;

    // JUCE may pass a full URL or just a path -- handle both
    juce::String path;
    if (url.startsWith ("http"))
        path = juce::URL (url).getSubPath().trimCharactersAtStart ("/");
    else
        path = url.trimCharactersAtStart ("/");

    if (path.isEmpty())
        path = "index.html";

    // Direct lookup first
    const juce::ZipFile::ZipEntry* entry = zipFile->getEntry (path);

    // Fallback: scan all entries matching by full name or trailing component
    if (entry == nullptr)
    {
        for (int i = 0; i < static_cast<int>(zipFile->getNumEntries()); ++i)
        {
            const juce::ZipFile::ZipEntry* e = zipFile->getEntry (i);
            if (e == nullptr) continue;
            if (e->filename == path || e->filename.endsWith ("/" + path))
            {
                entry = e;
                break;
            }
        }
    }

    if (entry == nullptr)
    {
        return std::nullopt;
    }

    auto stream = std::unique_ptr<juce::InputStream> (zipFile->createStreamForEntry (*entry));
    if (stream == nullptr)
        return std::nullopt;

    return juce::WebBrowserComponent::Resource {
        streamToVector (*stream),
        mimeFor (entry->filename)
    };
}

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor
// ─────────────────────────────────────────────────────────────────────────────

WebBridge::WebBridge (GraphModel& model, AddonRegistry* reg,
                       std::function<void(const juce::String&, const juce::String&)> setMidiCb,
                       std::function<void(const juce::String&, const juce::String&)> setAudioCb,
                       std::function<std::vector<MidiMonitorBatch>()> monitorFn,
                       std::function<std::vector<AudioSnapshot>()> audioFn,
                       std::function<void()> trashFn,
                       std::function<std::vector<PortActivity>()> activityFn,
                       std::function<void(const juce::String&, uint8_t, uint8_t, uint8_t)> keyFn,
                       std::function<void(const juce::String&, const juce::String&)>          labelFn,
                       std::function<void(const juce::String&, int, float)>                      paramFn,
                       std::function<void()>                                                          newGraphFn,
                       std::function<void(const juce::String&)>                                       loadGraphFn)
    : graph (model),
      registry            (reg),
      drainMonitor        (std::move (monitorFn)),
      getAudioSnapshots   (std::move (audioFn)),
      getPortActivity     (std::move (activityFn)),
      clearGraphTrash     (std::move (trashFn)),
      onMidiKeyEvent      (std::move (keyFn)),
      onSetNodeLabel      (std::move (labelFn)),
      onSetAddonParameter (std::move (paramFn)),
      onSetMidiDevice     (std::move (setMidiCb)),
      onSetAudioDevice    (std::move (setAudioCb)),
      onNewGraph          (std::move (newGraphFn)),
      onLoadGraph         (std::move (loadGraphFn))
{
#if HAS_BUNDLED_UI
    // ── Open the bundled zip ──────────────────────────────────────────────
    int         zipSize = 0;
    const char* zipData = BinaryData::getNamedResource ("ui_assets_zip", zipSize);

    jassert (zipData != nullptr && zipSize > 0);

    // Keep the raw memory alive in a MemoryInputStream that ZipFile reads from.
    zipStream = std::make_unique<juce::MemoryInputStream> (zipData, static_cast<size_t>(zipSize), false);
    zipFile   = std::make_unique<juce::ZipFile> (*zipStream);

    juce::Logger::writeToLog (
        juce::String ("WebBridge: zip loaded, ")
        + juce::String (zipFile->getNumEntries())
        + juce::String (" entries"));

    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled (true)
        .withResourceProvider (
            [this](const juce::String& url) { return getResource (url); },
            juce::URL (juce::WebBrowserComponent::getResourceProviderRoot()).getOrigin())
        .withEventListener ("graphMessage",
            [this](juce::var msg)
            {
                handleMessage (juce::JSON::toString (msg, true));
            })
#if JUCE_WINDOWS
        // Windows needs WebView2 with a writable user-data folder
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (
                    juce::File::getSpecialLocation (
                        juce::File::SpecialLocationType::tempDirectory)))
#endif
        ;

#else
    // ── Dev mode ──────────────────────────────────────────────────────────
    devServerUrl = "http://localhost:5173";

    auto options = juce::WebBrowserComponent::Options{}
        .withNativeIntegrationEnabled (true)
        .withEventListener ("graphMessage",
            [this](juce::var msg)
            {
                handleMessage (juce::JSON::toString (msg, true));
            })
#if JUCE_WINDOWS
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (
            juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (
                    juce::File::getSpecialLocation (
                        juce::File::SpecialLocationType::tempDirectory)))
#endif
        ;
#endif

    webView = std::make_unique<Browser> (*this, std::move (options));
    addAndMakeVisible (*webView);

    // NOTE: graph.onChange is set by PatchyProcessor and must not be overwritten here.
    // pushGraphToUI() is called from PatchyProcessor's onChange chain instead.
}

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::resized()
{
    if (webView != nullptr)
        webView->setBounds (getLocalBounds());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helper — escape and push a JSON payload to a named bridge function
// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushToUI (const juce::String& bridgeFn, juce::String json)
{
    if (! connected || webView == nullptr) return;
    json = json.replace ("\\", "\\\\").replace ("`", "\\`");
    juce::String script;
    script << "if(window.__bridge&&window.__bridge." << bridgeFn << "){"
           << "window.__bridge." << bridgeFn << "(`" << json << "`);}";
    webView->evaluateJavascript (script, {});
}

void WebBridge::loadUI()
{
#if HAS_BUNDLED_UI
    connected = true;
    // This is the exact URL used in JUCE's WebViewPluginDemo
    webView->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
#else
    webView->goToURL (devServerUrl);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  Message handling
// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::handleMessage (const juce::String& json)
{
    auto v = juce::JSON::parse (json);
    if (! v.isObject()) return;

    auto* obj  = v.getDynamicObject();
    auto  type = obj->getProperty ("type").toString();

    if (type == "ready")
    {
        connected = true;
        juce::Logger::writeToLog ("WebBridge: UI ready.");
        pushAddonList();
        pushToUI ("onFileState", buildFileStateJson());
        pushMidiDevices();
        pushAudioDevices();
        pushGraphToUI();
        startTimerHz (30);
    }
    else if (type == "addNode")
    {
        juce::String addonName = obj->getProperty ("addonName").toString();
        int audioIn = 0, audioOut = 0, midiIn = 0, midiOut = 0;

        // Look up port counts from registry for addon nodes
        if (registry && addonName.isNotEmpty())
        {
            for (const auto& e : registry->getEntries())
            {
                if (e.name == addonName)
                {
                    audioIn  = e.audioInputs;
                    audioOut = e.audioOutputs;
                    midiIn   = e.midiInputs;
                    midiOut  = e.midiOutputs;
                    break;
                }
            }
        }

        graph.addNode (
            (int)   obj->getProperty ("nodeType"),
            (float) obj->getProperty ("x"),
            (float) obj->getProperty ("y"),
            addonName, audioIn, audioOut, midiIn, midiOut);
    }
    else if (type == "setAddonParameter")
    {
        juce::String nodeId = obj->getProperty ("nodeId").toString();
        int   index = (int)   obj->getProperty ("index");
        float value = (float) obj->getProperty ("value");
        if (onSetAddonParameter)
            onSetAddonParameter (nodeId, index, value);
    }
    else if (type == "setNodeSettings")
    {
        juce::String nodeId    = obj->getProperty ("nodeId").toString();
        juce::String settingsJson = obj->getProperty ("settings").toString();
        if (nodeId.isNotEmpty())
            graph.setNodeSettings (nodeId, settingsJson);
        // Note: no onChange call — settings are UI-only, no need to rebuild graph
    }
    else if (type == "setNodeLabel")
    {
        juce::String nodeId   = obj->getProperty ("nodeId").toString();
        juce::String newLabel = obj->getProperty ("label").toString();
        if (nodeId.isNotEmpty() && onSetNodeLabel)
            onSetNodeLabel (nodeId, newLabel);
        // Note: we do NOT call graph.renameNode() — the node label in the
        // NODE column always shows the original type name. The custom name
        // only appears in the NAME column of MidiMonitor.
    }
    else if (type == "midiKeyEvent")
    {
        juce::String nid = obj->getProperty ("nodeId").toString();
        uint8_t status = (uint8_t) (int) obj->getProperty ("status");
        uint8_t data1  = (uint8_t) (int) obj->getProperty ("data1");
        uint8_t data2  = (uint8_t) (int) obj->getProperty ("data2");
        if (onMidiKeyEvent) onMidiKeyEvent (nid, status, data1, data2);
    }
    else if (type == "removeNode")
    {
        graph.removeNode (obj->getProperty ("nodeId").toString());
    }
    else if (type == "addConnection")
    {
        graph.addConnection (
            obj->getProperty ("sourceNodeId").toString(),
            obj->getProperty ("sourcePortId").toString(),
            obj->getProperty ("targetNodeId").toString(),
            obj->getProperty ("targetPortId").toString());
    }
    else if (type == "removeConnection")
    {
        graph.removeConnection (obj->getProperty ("connectionId").toString());
    }
    else if (type == "moveNode")
    {
        if (auto* node = graph.findNode (obj->getProperty ("nodeId").toString()))
        {
            node->x = (float) obj->getProperty ("x");
            node->y = (float) obj->getProperty ("y");
        }
    }
    else if (type == "setViewport")
    {
        graph.viewportX    = (float) obj->getProperty ("x");
        graph.viewportY    = (float) obj->getProperty ("y");
        graph.viewportZoom = (float) obj->getProperty ("zoom");
    }
    else if (type == "setNodeParam")
    {
        juce::String nodeId = obj->getProperty ("nodeId").toString();
        juce::String key    = obj->getProperty ("key").toString();
        juce::String value  = obj->getProperty ("value").toString();

        if (key == "midiDeviceId" && onSetMidiDevice)
            onSetMidiDevice (nodeId, value);
        else if (key == "audioDeviceId" && onSetAudioDevice)
            onSetAudioDevice (nodeId, value);
    }
    else if (type == "fileSave")
    {
        if (currentFile.existsAsFile())
            saveToFile (currentFile);
        else
            showSaveDialog();
    }
    else if (type == "fileSaveAs")
    {
        showSaveDialog();
    }
    else if (type == "fileOpen")
    {
        showOpenDialog();
    }
    else if (type == "fileNew")
    {
        currentFile = juce::File();
        if (onNewGraph) onNewGraph();
        pushToUI ("onFileState", buildFileStateJson());
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushAddonList()
{
    if (! connected || registry == nullptr) return;

    // Build JSON: { addons: [ {name, vendor, version, nodeType}, ... ] }
    juce::Array<juce::var> arr;
    for (const auto& e : registry->getEntries())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name",        e.name);
        obj->setProperty ("vendor",      e.vendor);
        obj->setProperty ("version",     e.version);
        obj->setProperty ("nodeType",    e.nodeType);
        // Effective port counts (0 = default for nodeType)
        int ngaType = e.nodeType;
        obj->setProperty ("audioInputs",  e.audioInputs  > 0 ? e.audioInputs  : (ngaType == 2 || ngaType == 3 ? 1 : 0));
        obj->setProperty ("audioOutputs", e.audioOutputs > 0 ? e.audioOutputs : (ngaType == 2 || ngaType == 3 ? 1 : 0));
        obj->setProperty ("midiInputs",   e.midiInputs   > 0 ? e.midiInputs   : (ngaType == 1 || ngaType == 3 ? 1 : 0));
        obj->setProperty ("midiOutputs",  e.midiOutputs  > 0 ? e.midiOutputs  : (ngaType == 1 || ngaType == 3 ? 1 : 0));

        // Include parameter descriptors so UI can render sliders
        juce::Array<juce::var> params;
        if (e.create && e.getParamCount && e.getParamInfo)
        {
            auto* tmp = e.create();
            int count = e.getParamCount (tmp);
            for (int p = 0; p < count; ++p)
            {
                NGA_ParameterInfo info {};
                e.getParamInfo (tmp, p, &info);
                auto* po = new juce::DynamicObject();
                po->setProperty ("index",        p);
                po->setProperty ("name",         juce::String (info.name));
                po->setProperty ("min",          info.minValue);
                po->setProperty ("max",          info.maxValue);
                po->setProperty ("defaultValue", info.defaultValue);
                po->setProperty ("step",         info.step);
                params.add (po);
            }
            e.destroy (tmp);
        }
        obj->setProperty ("params", params);
        arr.add (obj);
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("addons", arr);

    auto json = juce::JSON::toString (root, true);
    json = json.replace ("\\", "\\\\").replace ("`", "\\`");

    pushToUI ("onAddonList", json);
}


void WebBridge::pushMidiDevices()
{
    pushToUI ("onMidiDevices", juce::JSON::toString (MidiDeviceManager::getAvailableDevicesVar(), true));
}

void WebBridge::pushAudioDevices()
{
    pushToUI ("onAudioDevices", juce::JSON::toString (AudioDeviceManager::getAvailableDevicesVar(), true));
}

void WebBridge::timerCallback()
{
    // Clear old graph trash on message thread before any snapshot/drain
    if (clearGraphTrash) clearGraphTrash();
    if (connected)
    {
        pushMidiMonitorEvents();
        pushAudioSnapshots();
        pushPortActivity();
    }
}

void WebBridge::pushMidiMonitorEvents()
{
    if (! connected || ! drainMonitor || webView == nullptr) return;

    auto batches = drainMonitor();

    if (batches.empty()) return;

    // Build JSON using explicit quote character to avoid escape confusion.
    const juce::juce_wchar Q = '"';

    juce::String json;
    json << "[";

    bool firstBatch = true;
    for (const auto& batch : batches)
    {
        if (! firstBatch) json << ",";
        firstBatch = false;

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << batch.nodeId << Q << ","
             << Q << "events" << Q << ":[";

        bool firstEv = true;
        for (const auto& ev : batch.events)
        {
            if (! firstEv) json << ",";
            firstEv = false;

            // Escape backslashes and quotes in string fields
            juce::String sn = ev.sourceNode.replace  ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String sd = ev.sourceDevice.replace ("\\", "\\\\").replace ("\"", "\\\"");

            json << "{"
                 << Q << "ts" << Q << ":" << ev.timestampMs  << ","
                 << Q << "sn" << Q << ":" << Q << sn << Q    << ","
                 << Q << "sd" << Q << ":" << Q << sd << Q    << ","
                 << Q << "st" << Q << ":" << (int) ev.statusByte << ","
                 << Q << "d1" << Q << ":" << (int) ev.data1      << ","
                 << Q << "d2" << Q << ":" << (int) ev.data2
                 << "}";
        }
        json << "]}";
    }
    json << "]";

    pushToUI ("onMidiMonitorEvents", json);
}

void WebBridge::pushAudioSnapshots()
{
    if (! connected || ! getAudioSnapshots || webView == nullptr) return;

    auto snapshots = getAudioSnapshots();

    if (snapshots.empty()) return;

    // Performance: downsample to kDisplaySamples points before encoding.
    // The canvas is ~280px wide so more samples are invisible.
    // Encode as integers (value * 1000, clamped ±1000) to avoid decimal points —
    // reduces payload by ~3x versus floating-point strings.
    static constexpr int kDisplaySamples = 512;

    const juce::juce_wchar Q = '"';
    juce::String json;
    json << "[";
    bool firstSnap = true;

    for (const auto& snap : snapshots)
    {
        if (! firstSnap) json << ",";
        firstSnap = false;

        const int srcCount = (int) snap.left.size();
        const int outCount = std::min (kDisplaySamples, srcCount);

        juce::String lStr, rStr;
        lStr.preallocateBytes (static_cast<size_t>(outCount) * 6);
        rStr.preallocateBytes (static_cast<size_t>(outCount) * 6);

        for (int i = 0; i < outCount; ++i)
        {
            // Downsample: pick evenly-spaced samples from the source buffer
            int idx = (srcCount > outCount)
                        ? (int) ((int64_t) i * srcCount / outCount)
                        : i;
            idx = juce::jlimit (0, srcCount - 1, idx);

            // Encode as integer (×1000), saves ~3× vs "0.1234"
            int lv = juce::jlimit (-1000, 1000, (int) (snap.left[static_cast<size_t>(idx)]  * 1000.0f));
            int rv = juce::jlimit (-1000, 1000, (int) (snap.right[static_cast<size_t>(idx)] * 1000.0f));

            if (i > 0) { lStr << ","; rStr << ","; }
            lStr << lv;
            rStr << rv;
        }

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << snap.nodeId << Q << ","
             << Q << "sr"     << Q << ":" << (int) snap.sampleRate << ","
             << Q << "n"      << Q << ":" << outCount << ","
             << Q << "l"      << Q << ":" << Q << lStr << Q << ","
             << Q << "r"      << Q << ":" << Q << rStr << Q
             << "}";
    }
    json << "]";
    pushToUI ("onAudioSnapshot", json);
}

void WebBridge::pushPortActivity()
{
    if (! connected || ! getPortActivity || webView == nullptr) return;

    auto activities = getPortActivity();

    if (activities.empty()) return;

    const juce::juce_wchar Q = '"';
    juce::String json;
    json << "[";
    bool first = true;
    for (const auto& a : activities)
    {
        if (! first) json << ",";
        first = false;
        // Encode RMS as integer (×1000) for compactness
        int lv = juce::jlimit (0, 1000, (int) (a.audioRmsL * 1000.0f));
        int rv = juce::jlimit (0, 1000, (int) (a.audioRmsR * 1000.0f));
        // Encode incoming notes as "s,n s,n ..." e.g. "144,60 128,60"
        juce::String notesStr;
        for (const auto& [st, n] : a.incomingNotes)
        {
            if (notesStr.isNotEmpty()) notesStr << " ";
            notesStr << (int) st << "," << (int) n;
        }

        json << "{"
             << Q << "id"    << Q << ":" << Q << a.nodeId       << Q << ","
             << Q << "midi"  << Q << ":" << a.midiOutEvents             << ","
             << Q << "l"     << Q << ":" << lv                          << ","
             << Q << "r"     << Q << ":" << rv                          << ","
             << Q << "notes" << Q << ":" << Q << notesStr        << Q
             << "}";
    }
    json << "]";
    pushToUI ("onPortActivity", json);
}

void WebBridge::pushGraphToUI()
{
    pushToUI ("onGraphUpdate", juce::JSON::toString (graph.toVar(), true));
}

// ── File operations ───────────────────────────────────────────────────────────

juce::String WebBridge::buildFileStateJson()
{
    juce::String name = currentFile.existsAsFile()
                        ? currentFile.getFileNameWithoutExtension()
                        : "Untitled";
    juce::String json;
    json << "{"
         << "\"fileName\":\"" << name << "\","
         << "\"hasFile\":" << (currentFile.existsAsFile() ? "true" : "false")
         << "}";
    return json;
}

void WebBridge::saveToFile (const juce::File& file)
{
    auto json = juce::JSON::toString (graph.toVar(), true);
    if (file.replaceWithText (json))
    {
        currentFile = file;
        pushToUI ("onFileState", buildFileStateJson());
    }
}

void WebBridge::showSaveDialog()
{
    auto chooser = std::make_shared<juce::FileChooser> (
        "Save Patch", currentFile.existsAsFile()
            ? currentFile
            : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                  .getChildFile ("Untitled.patchy"),
        "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{})
            {
                auto f = result.withFileExtension ("patchy");
                saveToFile (f);
            }
        });
}

void WebBridge::showOpenDialog()
{
    auto chooser = std::make_shared<juce::FileChooser> (
        "Open Patch",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{} && result.existsAsFile())
            {
                auto json = result.loadFileAsString();
                if (json.isNotEmpty())
                {
                    currentFile = result;
                    if (onLoadGraph) onLoadGraph (json);
                    pushToUI ("onFileState", buildFileStateJson());
                }
            }
        });
}
