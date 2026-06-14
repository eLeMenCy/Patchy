#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"
#include <unordered_map>

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

WebBridge::WebBridge (GraphModel& model, PaxRegistry* reg,
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
    // connected will be set true when UI sends "ready" message
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
        // Guard against double-ready (WebView sometimes fires twice on load)
        const bool wasConnected = connected;
        connected = true;
        juce::Logger::writeToLog ("WebBridge: UI ready.");
        pushPaxList();
        pushToUI ("onFileState", buildFileStateJson());
        pushMidiDevices();
        pushAudioDevices();
        pushGraphToUI();
        pushUndoState();
        if (!wasConnected)
        {
            if (onUIReady) onUIReady();
            startTimerHz (30);
        }
    }
    else if (type == "addNode")
    {
        pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
        graph.pushSnapshot();
        juce::String paxName = obj->getProperty ("paxName").toString();
        int audioIn = 0, audioOut = 0, midiIn = 0, midiOut = 0;

        // Look up port counts from registry for addon nodes
        if (registry && paxName.isNotEmpty())
        {
            for (const auto& e : registry->getEntries())
            {
                if (e.name == paxName)
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
            paxName, audioIn, audioOut, midiIn, midiOut);
    }
    else if (type == "setPaxParameter")
    {
        juce::String nodeId = obj->getProperty ("nodeId").toString();
        int   index = (int)   obj->getProperty ("index");
        float value = (float) obj->getProperty ("value");
        if (onSetAddonParameter)
            onSetAddonParameter (nodeId, index, value);

        // Only update port count when band count changes (index 0)
        // Frequency boundary changes (index 1+) do NOT need any graph update
        if (index == 0 && onGetPaxAudioOutCount)
        {
            int newCount = onGetPaxAudioOutCount (nodeId);
            if (newCount > 0)
            {
                // Check current port count — only update if it actually changed
                // (avoids pushGraphToUI on every slider tick for fixed-port nodes like Amp)
                int currentCount = 0;
                for (auto& n : graph.getNodes())
                    if (n.id == nodeId) { for (auto& p : n.ports) if (p.type == PortType::Audio && p.direction == PortDirection::Output) currentCount++; break; }

                if (newCount != currentCount)
                {
                    graph.suspendNotifications();
                    graph.updateNodeAudioOutputCount (nodeId, newCount);
                    graph.resumeNotificationsQuiet();
                    if (onPrunePaxEdges) onPrunePaxEdges (nodeId);
                    pushGraphToUI();
                }
            }
        }
    }
    else if (type == "setNodeSettings")
    {
        juce::String nodeId       = obj->getProperty ("nodeId").toString();
        juce::String settingsJson = obj->getProperty ("settings").toString();
        if (nodeId.isNotEmpty())
        {
            // Capture snapshot before FIRST change for this node interaction.
            // For discrete controls (checkbox, combo) each sends setNodeSettings+commitNodeSettings
            // as a pair — pendingSettingsNodeId is cleared by commitNodeSettings between clicks.
            // For sliders, only the first tick captures the pre-drag state.
            if (pendingSettingsNodeId != nodeId)
            {
                pendingSettingsNodeId   = nodeId;
                pendingSettingsSnapshot = graph.toVar();
            }
            graph.setNodeSettings (nodeId, settingsJson);
            pushSettingsToUI (nodeId, settingsJson);
        }
    }
    else if (type == "commitNodeSettings")
    {
        // User finished adjusting slider/stepper — push the pre-drag snapshot
        // so undo restores the state BEFORE the slider was moved, not after.
        if (pendingSettingsSnapshot.isObject())
        {
            auto* node = graph.findNode (pendingSettingsNodeId);
            juce::String snapSettings;
            if (auto* snapObj = pendingSettingsSnapshot.getDynamicObject())
                if (auto* arr = snapObj->getProperty ("nodes").getArray())
                    for (auto& nv : *arr)
                        if (auto* nobj = nv.getDynamicObject())
                            if (nobj->getProperty ("id").toString() == pendingSettingsNodeId)
                                snapSettings = nobj->getProperty ("settingsJson").toString();

            if (node && node->settingsJson != snapSettings)
            {
                pushSettingsToUI (pendingSettingsNodeId, node->settingsJson);
                graph.pushExistingSnapshot (std::move (pendingSettingsSnapshot));
                pushUndoState();
            }
            pendingSettingsSnapshot = juce::var();
            pendingSettingsNodeId.clear();
        }
    }
    else if (type == "commitSettingsChange")
    {
        // Atomic set+commit for discrete controls (checkbox, combo, etc.)
        // Captures pre-change snapshot, applies change, pushes snapshot — all in one message.
        juce::String nodeId       = obj->getProperty ("nodeId").toString();
        juce::String settingsJson = obj->getProperty ("settings").toString();
        if (nodeId.isNotEmpty())
        {
            // Clear any pending slider interaction first
            pendingSettingsSnapshot = juce::var();
            pendingSettingsNodeId.clear();

            auto preSnapshot = graph.toVar();
            graph.setNodeSettings (nodeId, settingsJson);

            // Only push if actually changed
            juce::String snapSettings;
            if (auto* snapObj = preSnapshot.getDynamicObject())
                if (auto* arr = snapObj->getProperty ("nodes").getArray())
                    for (auto& nv : *arr)
                        if (auto* nobj = nv.getDynamicObject())
                            if (nobj->getProperty ("id").toString() == nodeId)
                                snapSettings = nobj->getProperty ("settingsJson").toString();

            if (snapSettings != settingsJson)
            {
                pushSettingsToUI (nodeId, settingsJson);
                graph.pushExistingSnapshot (std::move (preSnapshot));
                pushUndoState();
            }
        }
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
        pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
        graph.pushSnapshot();
        graph.removeNode (obj->getProperty ("nodeId").toString());
    }
    else if (type == "addConnection")
    {
        pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
        graph.pushSnapshot();
        graph.addConnection (
            obj->getProperty ("sourceNodeId").toString(),
            obj->getProperty ("sourcePortId").toString(),
            obj->getProperty ("targetNodeId").toString(),
            obj->getProperty ("targetPortId").toString());
    }
    else if (type == "removeConnection")
    {
        juce::String connId = obj->getProperty ("connectionId").toString();
        // Only snapshot if the connection exists — ReactFlow fires removeConnection
        // for each edge when a node is deleted, but C++ already removed them
        // as part of removeNode. Avoid phantom snapshots.
        if (graph.hasConnection (connId))
        {
            graph.pushSnapshot();
        }
        graph.removeConnection (connId);
    }
    else if (type == "moveNode")
    {
        juce::String nodeId = obj->getProperty ("nodeId").toString();
        if (auto* node = graph.findNode (nodeId))
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
        // Clear any pending settings snapshot — device change is a new action
        pendingSettingsSnapshot = juce::var();
        pendingSettingsNodeId.clear();
        graph.pushSnapshot();
        if (key == "midiDeviceId" && onSetMidiDevice)
            onSetMidiDevice (nodeId, value);
        else if (key == "audioDeviceId" && onSetAudioDevice)
        {
            onSetAudioDevice (nodeId, value);

            // After a device change, validate stored selectedChannels.
            // We can't know the new channel count synchronously here (the device
            // opens on the audio thread), so we push a special message to React
            // that tells it to re-check once the new device info arrives via
            // onAudioDevices. The UI will warn and reset if channels are invalid.
            auto* nd = graph.findNode (nodeId);
            if (nd != nullptr && nd->settingsJson.isNotEmpty())
            {
                auto* msgObj = new juce::DynamicObject();
                msgObj->setProperty ("nodeId",      nodeId);
                msgObj->setProperty ("settingsJson", nd->settingsJson);
                pushToUI ("onAudioDeviceChanged",
                          juce::JSON::toString (juce::var (msgObj), true));
            }
        }
        else if (key == "audioDeviceChannels" && onSetAudioDeviceChannels)
        {
            // Parse the JSON array of channel indices sent from React
            std::vector<int> channels;
            auto parsed = juce::JSON::parse (value);
            if (auto* arr = parsed.getArray())
                for (auto& v : *arr) channels.push_back ((int) v);

            onSetAudioDeviceChannels (nodeId, channels);

            // Persist into settingsJson — merge with existing settings
            juce::var existing;
            if (auto* nd = graph.findNode (nodeId))
            {
                try { existing = juce::JSON::parse (nd->settingsJson); } catch (...) {}
                if (existing.getDynamicObject() == nullptr)
                    existing = new juce::DynamicObject();

                juce::Array<juce::var> arr;
                for (int ch : channels) arr.add (ch);
                existing.getDynamicObject()->setProperty ("selectedChannels", arr);

                juce::String newJson = juce::JSON::toString (existing, true);
                graph.setNodeSettings (nodeId, newJson);
                pushSettingsToUI (nodeId, newJson);
            }
        }
        // Push updated graph so React reflects the new selectedDeviceId / settings
        pushGraphToUI();
        pushUndoState();
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
    else if (type == "exportSelection")
    {
        auto nodeIdsVar = obj->getProperty ("selectedNodeIds");
        auto suggestedName = obj->getProperty ("suggestedName").toString();
        juce::StringArray selectedNodeIds;
        if (auto* arr = nodeIdsVar.getArray())
            for (auto& v : *arr)
                selectedNodeIds.add (v.toString());
        if (selectedNodeIds.isEmpty()) return;
        showExportDialog (selectedNodeIds, suggestedName);
    }
    else if (type == "importFragment")
    {
        showImportDialog();
    }
    else if (type == "undo")
    {
        handleUndo();
    }
    else if (type == "redo")
    {
        handleRedo();
    }
    else if (type == "importFragmentNodes")
    {
        graph.pushSnapshot();
        // React has placed the nodes on canvas — now commit them to C++ graph model.
        // We reuse the existing loadGraph path: merge fragment into current graph JSON.
        auto* nodesArr = obj->getProperty ("nodes").getArray();
        auto* connsArr = obj->getProperty ("connections").getArray();
        if (nodesArr == nullptr) return;

        graph.suspendNotifications();

        for (auto& n : *nodesArr)
        {
            auto* nObj = n.getDynamicObject();
            if (! nObj) continue;

            juce::String savedId  = nObj->getProperty ("id").toString();
            int          nodeType = (int) nObj->getProperty ("nodeType");
            float        x        = (float) (double) nObj->getProperty ("x");
            float        y        = (float) (double) nObj->getProperty ("y");
            juce::String paxName= nObj->getProperty ("addonName").toString();
            int audioIn  = (int) nObj->getProperty ("audioInputs");
            int audioOut = (int) nObj->getProperty ("audioOutputs");
            int midiIn   = (int) nObj->getProperty ("midiInputs");
            int midiOut  = (int) nObj->getProperty ("midiOutputs");

            auto& nd = graph.restoreNode (savedId, nodeType, x, y, paxName,
                                          audioIn, audioOut, midiIn, midiOut);
            // Restore device selection if present
            juce::String devId = nObj->getProperty ("selectedDeviceId").toString();
            if (devId.isNotEmpty()) nd.selectedDeviceId = devId;
            // Restore settings blob if present
            juce::String settings = nObj->getProperty ("settingsJson").toString();
            if (settings.isNotEmpty()) nd.settingsJson = settings;
            // Restore custom label if present
            juce::String label = nObj->getProperty ("label").toString();
            if (label.isNotEmpty()) nd.label = label;
        }

        if (connsArr)
            for (auto& c : *connsArr)
            {
                auto* cObj = c.getDynamicObject();
                if (! cObj) continue;
                graph.addConnection (
                    cObj->getProperty ("sourceNodeId").toString(),
                    cObj->getProperty ("sourcePortId").toString(),
                    cObj->getProperty ("targetNodeId").toString(),
                    cObj->getProperty ("targetPortId").toString()
                );
            }

        graph.resumeNotifications();   // fires onChange → rebuild
    }
    else if (type == "setAudioEngineSettings")
    {
        double sr   = (double) obj->getProperty ("sampleRate");
        int    buf  = (int)    obj->getProperty ("bufferSize");
        bool   mute = (bool)   obj->getProperty ("muteFeedback");
        if (onSetAudioEngineSettings)
            onSetAudioEngineSettings (sr, buf, mute);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushUndoState()
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("canUndo", graph.canUndo());
    obj->setProperty ("canRedo", graph.canRedo());
    pushToUI ("onUndoState", juce::JSON::toString (juce::var (obj.release()), false));
}

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushPaxList()
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
                PAX_ParameterInfo info {};
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
    root->setProperty ("paxItems", arr);

    auto json = juce::JSON::toString (root, true);
    json = json.replace ("\\", "\\\\").replace ("`", "\\`");

    pushToUI ("onPaxList", json);
}

void WebBridge::pushMidiDevices()
{
    pushToUI ("onMidiDevices", juce::JSON::toString (MidiDeviceManager::getAvailableDevicesVar(), true));
}

void WebBridge::pushAudioDevices()
{
    pushToUI ("onAudioDevices", juce::JSON::toString (AudioDeviceManager::getAvailableDevicesVar (isStandalone), true));
}

void WebBridge::timerCallback()
{
    // Clear old graph trash on message thread before any snapshot/drain
    if (clearGraphTrash) clearGraphTrash();
    if (connected)
    {
        pushMidiMonitorEvents();
        pushAudioSnapshots();
        pushSpectrumSnapshots();
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

        // Build portRms array for multi-port nodes
        juce::String portRmsStr = "[";
        for (size_t pi = 0; pi < a.portRms.size(); ++pi)
        {
            if (pi > 0) portRmsStr << ",";
            portRmsStr << juce::jlimit (0, 1000, (int) (a.portRms[pi] * 1000.0f));
        }
        portRmsStr << "]";

        json << "{"
             << Q << "id"       << Q << ":" << Q << a.nodeId       << Q << ","
             << Q << "midi"     << Q << ":" << a.midiOutEvents             << ","
             << Q << "l"        << Q << ":" << lv                          << ","
             << Q << "r"        << Q << ":" << rv                          << ","
             << Q << "portRms"  << Q << ":" << portRmsStr                  << ","
             << Q << "notes"    << Q << ":" << Q << notesStr        << Q
             << "}";
    }
    json << "]";
    pushToUI ("onPortActivity", json);
}

void WebBridge::pushGraphToUI()
{
    pushToUI ("onGraphUpdate", juce::JSON::toString (graph.toVar(), true));
}

void WebBridge::pushSettingsToUI (const juce::String& nodeId, const juce::String& settingsJson)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("nodeId",       nodeId);
    obj->setProperty ("settingsJson", settingsJson);
    pushToUI ("onNodeSettings", juce::JSON::toString (juce::var (obj.release()), false));
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
                lastOpenDir = f.getParentDirectory();
                saveToFile (f);
            }
        });
}

void WebBridge::showOpenDialog()
{
    auto startDir = currentFile.existsAsFile()
                    ? currentFile.getParentDirectory()
                    : (lastOpenDir.isDirectory()
                       ? lastOpenDir
                       : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    auto chooser = std::make_shared<juce::FileChooser> ("Open Patch", startDir, "*.patchy");

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
                    lastOpenDir  = result.getParentDirectory();
                    if (onLoadGraph) onLoadGraph (json);
                    pushToUI ("onFileState", buildFileStateJson());

                }
            }
        });
}

// ── Push spectrum snapshots (FFT magnitudes for Spectrumyser nodes) ───────────
void WebBridge::pushSpectrumSnapshots()
{
    if (! connected || ! getSpectrumSnapshots || webView == nullptr) return;

    auto snaps = getSpectrumSnapshots();
    if (snaps.empty()) return;

    const juce::String Q = "\"";
    juce::String json = "[";
    bool first = true;
    for (auto& s : snaps)
    {
        if (! first) json += ",";
        first = false;

        // Magnitude bins — compress to 64 log-spaced values for UI performance
        constexpr int UI_BINS = 64;
        juce::String mags = "[";
        int total = (int) s.magnitudes.size();
        for (int i = 0; i < UI_BINS; ++i)
        {
            float t   = (float) i / UI_BINS;
            int   idx = (int) (std::pow (10.f, t * std::log10 ((float) total)) - 1);
            idx = std::max (0, std::min (total - 1, idx));
            float mag = s.magnitudes[static_cast<size_t> (idx)];
            float db  = mag > 0.f ? 20.f * std::log10 (mag) : -80.f;
            float v   = std::max (0.f, std::min (1.f, (db + 80.f) / 80.f));
            if (i > 0) mags += ",";
            mags += juce::String ((int) (v * 1000.f));
        }
        mags += "]";

        // Band boundaries
        juce::String bands = "[";
        for (int b = 0; b < s.bandCount; ++b)
        {
            if (b > 0) bands += ",";
            juce::String loStr = juce::String ((int) s.bandLow  [static_cast<size_t> (b)]);
            juce::String hiStr = juce::String ((int) s.bandHigh [static_cast<size_t> (b)]);
            bands += "{" + Q + "lo" + Q + ":" + loStr
                  + "," + Q + "hi" + Q + ":" + hiStr + "}";
        }
        bands += "]";

        json += "{"   + Q + "id"    + Q + ":" + Q + s.nodeId + Q
              + ","   + Q + "sr"    + Q + ":" + juce::String ((int) s.sampleRate)
              + ","   + Q + "mags"  + Q + ":" + mags
              + ","   + Q + "bands" + Q + ":" + bands
              + "}";
    }
    json += "]";

    pushToUI ("onSpectrumSnapshots", json);
}
// ── C++-callable file operations (e.g. from keyboard shortcuts) ──────────────
void WebBridge::handleFileNew()
{
    currentFile = juce::File();
    if (onNewGraph) onNewGraph();
    pushToUI ("onFileState", buildFileStateJson());
}

void WebBridge::handleFileOpen()   { showOpenDialog(); }
void WebBridge::handleFileSave()   { if (currentFile.existsAsFile()) saveToFile (currentFile); else showSaveDialog(); }
void WebBridge::handleFileSaveAs() { showSaveDialog(); }

void WebBridge::handleUndo()
{
    if (graph.undo())
        pushUndoState();
}

void WebBridge::handleRedo()
{
    if (graph.redo())
        pushUndoState();
}

// ── Fragment export ───────────────────────────────────────────────────────────
void WebBridge::showExportDialog (const juce::StringArray& selectedNodeIds,
                                   const juce::String& suggestedName)
{
    auto startDir = currentFile.existsAsFile()
                    ? currentFile.getParentDirectory()
                    : (lastOpenDir.isDirectory()
                       ? lastOpenDir
                       : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

    auto defaultFile = startDir.getChildFile (suggestedName + ".patchy");

    auto chooser = std::make_shared<juce::FileChooser> (
        "Export Fragment", defaultFile, "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser, selectedNodeIds] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result == juce::File{}) return;

            auto f = result.withFileExtension ("patchy");
            lastOpenDir = f.getParentDirectory();

            // Build fragment: filter graph to selected nodes only,
            // keeping connections that are entirely within the selection.
            auto fullGraph = graph.toVar();
            auto* fullObj  = fullGraph.getDynamicObject();
            if (fullObj == nullptr) return;

            // Collect selected nodes
            juce::Array<juce::var> fragNodes;
            auto* allNodes = fullObj->getProperty ("nodes").getArray();
            if (allNodes)
                for (auto& n : *allNodes)
                    if (auto* nObj = n.getDynamicObject())
                        if (selectedNodeIds.contains (nObj->getProperty ("id").toString()))
                            fragNodes.add (n);

            // Collect internal connections only (both ends in selection)
            juce::Array<juce::var> fragConns;
            auto* allConns = fullObj->getProperty ("connections").getArray();
            if (allConns)
                for (auto& c : *allConns)
                    if (auto* cObj = c.getDynamicObject())
                        if (selectedNodeIds.contains (cObj->getProperty ("sourceNodeId").toString()) &&
                            selectedNodeIds.contains (cObj->getProperty ("targetNodeId").toString()))
                            fragConns.add (c);

            // Compute bounding box for ghost sizing on import
            float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
            for (auto& n : fragNodes)
                if (auto* nObj = n.getDynamicObject())
                {
                    float x = (float) (double) nObj->getProperty ("x");
                    float y = (float) (double) nObj->getProperty ("y");
                    minX = std::min (minX, x); minY = std::min (minY, y);
                    maxX = std::max (maxX, x); maxY = std::max (maxY, y);
                }
            // Add approximate node dimensions
            float fragW = (maxX - minX) + 160.f;
            float fragH = (maxY - minY) + 80.f;

            // Normalise positions relative to top-left of bounding box
            for (auto& n : fragNodes)
                if (auto* nObj = n.getDynamicObject())
                {
                    nObj->setProperty ("x", (double) nObj->getProperty ("x") - minX);
                    nObj->setProperty ("y", (double) nObj->getProperty ("y") - minY);
                }

            auto fragObj = std::make_unique<juce::DynamicObject>();
            fragObj->setProperty ("nodes",       juce::var (fragNodes));
            fragObj->setProperty ("connections", juce::var (fragConns));
            fragObj->setProperty ("width",       (double) fragW);
            fragObj->setProperty ("height",      (double) fragH);

            auto json = juce::JSON::toString (juce::var (fragObj.release()), true);
            f.replaceWithText (json);
        });
}

// ── Fragment import ───────────────────────────────────────────────────────────
void WebBridge::showImportDialog()
{
    auto startDir = currentFile.existsAsFile()
                    ? currentFile.getParentDirectory()
                    : (lastOpenDir.isDirectory()
                       ? lastOpenDir
                       : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));

    auto chooser = std::make_shared<juce::FileChooser> (
        "Import Fragment", startDir, "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result == juce::File{} || ! result.existsAsFile()) return;

            lastOpenDir = result.getParentDirectory();
            auto json = result.loadFileAsString();
            if (json.isEmpty()) return;

            juce::var parsed;
            if (juce::JSON::parse (json, parsed).failed()) return;

            // Remap all IDs to fresh UUIDs before sending to React
            auto remapped = remapFragmentIds (parsed, {});
            pushToUI ("onFragmentReady", juce::JSON::toString (remapped, false));
        });
}

// ── ID remapping — produces a fragment with fresh UUIDs ──────────────────────
juce::var WebBridge::remapFragmentIds (const juce::var& fragment,
                                        const juce::StringArray& /*selectedNodeIds*/)
{
    auto* srcObj = fragment.getDynamicObject();
    if (srcObj == nullptr) return fragment;

    // Build old→new ID map for nodes
    std::unordered_map<juce::String, juce::String> idMap;

    auto* srcNodes = srcObj->getProperty ("nodes").getArray();
    juce::Array<juce::var> newNodes;

    if (srcNodes)
    {
        for (auto& n : *srcNodes)
        {
            auto* nObj = n.getDynamicObject();
            if (! nObj) continue;

            juce::String oldId = nObj->getProperty ("id").toString();
            juce::String newId = juce::Uuid().toString();
            idMap[oldId] = newId;

            // Deep-copy node and remap its id + port ids
            auto newNode = std::make_unique<juce::DynamicObject>();
            auto props = nObj->getProperties();
            for (auto& prop : props)
                newNode->setProperty (prop.name, prop.value);

            newNode->setProperty ("id", newId);

            // Remap port IDs (format: oldNodeId_PortLabel_direction)
            if (auto* ports = nObj->getProperty ("ports").getArray())
            {
                juce::Array<juce::var> newPorts;
                for (auto& p : *ports)
                {
                    if (auto* pObj = p.getDynamicObject())
                    {
                        auto newPort = std::make_unique<juce::DynamicObject>();
                        auto pProps = pObj->getProperties();
                        for (auto& pp : pProps)
                            newPort->setProperty (pp.name, pp.value);

                        juce::String oldPortId = pObj->getProperty ("id").toString();
                        // Replace the node-id prefix in the port id
                        if (oldPortId.startsWith (oldId))
                            newPort->setProperty ("id", newId + oldPortId.substring (oldId.length()));

                        newPorts.add (juce::var (newPort.release()));
                    }
                }
                newNode->setProperty ("ports", juce::var (newPorts));
            }

            newNodes.add (juce::var (newNode.release()));
        }
    }

    // Remap connection IDs and node/port references
    auto* srcConns = srcObj->getProperty ("connections").getArray();
    juce::Array<juce::var> newConns;

    if (srcConns)
    {
        for (auto& c : *srcConns)
        {
            auto* cObj = c.getDynamicObject();
            if (! cObj) continue;

            juce::String oldSrcNode = cObj->getProperty ("sourceNodeId").toString();
            juce::String oldDstNode = cObj->getProperty ("targetNodeId").toString();

            // Skip connections that cross the boundary (node not in map)
            if (idMap.find (oldSrcNode) == idMap.end()) continue;
            if (idMap.find (oldDstNode) == idMap.end()) continue;

            auto newConn = std::make_unique<juce::DynamicObject>();
            newConn->setProperty ("id",           juce::Uuid().toString());
            newConn->setProperty ("sourceNodeId", idMap[oldSrcNode]);
            newConn->setProperty ("targetNodeId", idMap[oldDstNode]);

            // Remap port IDs in connections
            juce::String oldSrcPort = cObj->getProperty ("sourcePortId").toString();
            juce::String oldDstPort = cObj->getProperty ("targetPortId").toString();
            for (auto& [oldId, newId] : idMap)
            {
                if (oldSrcPort.startsWith (oldId))
                    oldSrcPort = newId + oldSrcPort.substring (oldId.length());
                if (oldDstPort.startsWith (oldId))
                    oldDstPort = newId + oldDstPort.substring (oldId.length());
            }
            newConn->setProperty ("sourcePortId", oldSrcPort);
            newConn->setProperty ("targetPortId", oldDstPort);

            newConns.add (juce::var (newConn.release()));
        }
    }

    auto result = std::make_unique<juce::DynamicObject>();
    result->setProperty ("nodes",       juce::var (newNodes));
    result->setProperty ("connections", juce::var (newConns));
    result->setProperty ("width",       srcObj->getProperty ("width"));
    result->setProperty ("height",      srcObj->getProperty ("height"));

    return juce::var (result.release());
}

