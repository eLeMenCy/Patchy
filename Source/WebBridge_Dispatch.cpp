#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"
#include "SerialPort.h"
#include <unordered_map>

#if HAS_BUNDLED_UI
  #include "BinaryData.h"
#endif

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
        handleReady();
    else if (type == "addNode")
        handleAddNode (obj);
    else if (type == "setPaxParameter")
        handleSetPaxParameter (obj);
    else if (type == "setNodeSettings")
        handleSetNodeSettings (obj);
    else if (type == "commitNodeSettings")
        handleCommitNodeSettings();
    else if (type == "commitSettingsChange")
        handleCommitSettingsChange (obj);
    else if (type == "setNodeLabel")
        handleSetNodeLabel (obj);
    else if (type == "midiKeyEvent")
        handleMidiKeyEvent (obj);
    else if (type == "removeNode")
        handleRemoveNode (obj);
    else if (type == "addConnection")
        handleAddConnection (obj);
    else if (type == "removeConnection")
        handleRemoveConnection (obj);
    else if (type == "moveNode")
        handleMoveNode (obj);
    else if (type == "setViewport")
        handleSetViewport (obj);
    else if (type == "setNodeParam")
        handleSetNodeParam (obj);
    else if (type == "fileSave")
        handleFileSave();
    else if (type == "fileSaveAs")
        handleFileSaveAs();
    else if (type == "fileOpen")
        handleFileOpen();
    else if (type == "fileNew")
        handleFileNew();
    else if (type == "audioPlayerLoadFile")
        handleAudioPlayerLoadFile (obj);
    else if (type == "audioPlayerRequestFileInfo")
        handleAudioPlayerRequestFileInfo (obj);
    else if (type == "exportSelection")
        handleExportSelection (obj);
    else if (type == "importFragment")
        showImportDialog();
    else if (type == "undo")
        handleUndo();
    else if (type == "redo")
        handleRedo();
    else if (type == "listSerialPorts")
        pushSerialPorts();
    else if (type == "importFragmentNodes")
        handleImportFragmentNodes (obj);
    else if (type == "setAudioEngineSettings")
        handleSetAudioEngineSettings (obj);
}

void WebBridge::handleReady ()
{
    // Guard against double-ready (WebView sometimes fires twice on load)
    const bool wasConnected = connected;
    connected = true;
    juce::Logger::writeToLog ("WebBridge: UI ready.");
    pushPaxList();
    pushToUI ("onFileState", buildFileStateJson());
    pushMidiDevices();
    pushAudioDevices();
    pushSerialPorts();
    pushGraphToUI();
    pushUndoState();
    if (!wasConnected)
    {
        if (onUIReady) onUIReady();
        startTimerHz (30);
    }
}

void WebBridge::handleAddNode (const juce::DynamicObject* obj)
{
    pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
    graph.pushSnapshot();
    juce::String paxName = obj->getProperty ("paxName").toString();
    PaxPortSpec portSpec;

    // Look up port counts + per-port types from registry for Pax nodes
    if (registry && paxName.isNotEmpty())
    {
        for (const auto& e : registry->getEntries())
        {
            if (e.name == paxName)
            {
                portSpec.audioIn  = e.audioInputs;
                portSpec.audioOut = e.audioOutputs;
                portSpec.midiIn   = e.midiInputs;
                portSpec.midiOut  = e.midiOutputs;
                portSpec.valueIn  = e.valueInputs;
                portSpec.valueOut = e.valueOutputs;
                for (int tag : e.valueInputTypes)  portSpec.valueInTypes.push_back  (paxValueTypeFromTag (tag));
                for (int tag : e.valueOutputTypes) portSpec.valueOutTypes.push_back (paxValueTypeFromTag (tag));
                break;
            }
        }
    }

    graph.addNode (
        (int)   obj->getProperty ("nodeType"),
        (float) obj->getProperty ("x"),
        (float) obj->getProperty ("y"),
        paxName, portSpec);

    // Initialize DMX Monitor/Console with default settingsJson so undo doesn't wipe settings
    int nodeType = (int) obj->getProperty ("nodeType");
    if (nodeType == 16 || nodeType == 17)
    {
        const auto& nodes = graph.getNodes();
        if (! nodes.empty())
        {
            auto& newNode = const_cast<NodeData&> (nodes.back());
            newNode.settingsJson = "{\"visibleCount\":8,\"startChannel\":0,\"valueFormat\":\"dec\",\"customName\":\"\",\"blackout\":false}";
        }
    }
    else if (nodeType == 18 || nodeType == 19)
    {
        const auto& nodes = graph.getNodes();
        if (! nodes.empty())
        {
            auto& newNode = const_cast<NodeData&> (nodes.back());
            newNode.settingsJson = "{\"visibleCount\":8,\"startChannel\":0,\"valueFormat\":\"dec\",\"customName\":\"\",\"blackout\":false,\"universe\":0,\"filterUniverse\":false,\"filterUniverseValue\":0}";
        }
    }
}

void WebBridge::handleSetPaxParameter (const juce::DynamicObject* obj)
{
    juce::String nodeId = obj->getProperty ("nodeId").toString();
    int   index = (int)   obj->getProperty ("index");
    float value = (float) obj->getProperty ("value");
    if (onSetPaxParameter)
        onSetPaxParameter (nodeId, index, value);

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

void WebBridge::handleSetNodeSettings (const juce::DynamicObject* obj)
{
    juce::String nodeId       = obj->getProperty ("nodeId").toString();
    juce::String settingsJson = obj->getProperty ("settings").toString();
    if (nodeId.isNotEmpty())
    {
        if (pendingSettingsNodeId != nodeId)
        {
            pendingSettingsNodeId   = nodeId;
            pendingSettingsSnapshot = graph.toVar();
        }
        graph.setNodeSettings (nodeId, settingsJson);
        pushSettingsToUI (nodeId, settingsJson);
    }
}

void WebBridge::handleCommitNodeSettings ()
{
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

void WebBridge::handleCommitSettingsChange (const juce::DynamicObject* obj)
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

void WebBridge::handleSetNodeLabel (const juce::DynamicObject* obj)
{
    juce::String nodeId   = obj->getProperty ("nodeId").toString();
    juce::String newLabel = obj->getProperty ("label").toString();
    if (nodeId.isNotEmpty() && onSetNodeLabel)
        onSetNodeLabel (nodeId, newLabel);
    // Note: we do NOT call graph.renameNode() — the node label in the
    // NODE column always shows the original type name. The custom name
    // only appears in the NAME column of MidiMonitor.
}

void WebBridge::handleMidiKeyEvent (const juce::DynamicObject* obj)
{
    juce::String nid = obj->getProperty ("nodeId").toString();
    uint8_t status = (uint8_t) (int) obj->getProperty ("status");
    uint8_t data1  = (uint8_t) (int) obj->getProperty ("data1");
    uint8_t data2  = (uint8_t) (int) obj->getProperty ("data2");
    if (onMidiKeyEvent) onMidiKeyEvent (nid, status, data1, data2);
}

void WebBridge::handleRemoveNode (const juce::DynamicObject* obj)
{
    pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
    graph.pushSnapshot();
    graph.removeNode (obj->getProperty ("nodeId").toString());
}

void WebBridge::handleAddConnection (const juce::DynamicObject* obj)
{
    pendingSettingsSnapshot = juce::var(); pendingSettingsNodeId.clear();
    graph.pushSnapshot();
    graph.addConnection (
        obj->getProperty ("sourceNodeId").toString(),
        obj->getProperty ("sourcePortId").toString(),
        obj->getProperty ("targetNodeId").toString(),
        obj->getProperty ("targetPortId").toString());
}

void WebBridge::handleRemoveConnection (const juce::DynamicObject* obj)
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

void WebBridge::handleMoveNode (const juce::DynamicObject* obj)
{
    juce::String nodeId = obj->getProperty ("nodeId").toString();
    if (auto* node = graph.findNode (nodeId))
    {
        node->x = (float) obj->getProperty ("x");
        node->y = (float) obj->getProperty ("y");
    }
}

void WebBridge::handleSetViewport (const juce::DynamicObject* obj)
{
    graph.viewportX    = (float) obj->getProperty ("x");
    graph.viewportY    = (float) obj->getProperty ("y");
    graph.viewportZoom = (float) obj->getProperty ("zoom");
}

void WebBridge::handleSetNodeParam (const juce::DynamicObject* obj)
{
    juce::String nodeId = obj->getProperty ("nodeId").toString();
    juce::String key    = obj->getProperty ("key").toString();
    juce::String value  = obj->getProperty ("value").toString();

    // dmxConsoleChannel / artNetConsoleChannel are real-time audio updates during drag —
    // preserve the pending snapshot captured by the preceding setNodeSettings.
    // dmxBlackout / artNetBlackout undo is handled by commitSettingsChange — skip pushSnapshot here.
    // audioPlayer* keys are either real-time playback controls (play/pause/seek/
    // return-to-start — same reasoning as dmxConsoleChannel exactly) or a live
    // mirror of a settings change already committed and snapshotted separately
    // via commitSettingsChange (see AudioPlayerNode.tsx's own dispatchLiveParams) —
    // pushing another snapshot here was a real, confirmed bug: since it happened
    // right after the very same change, pressing undo once reverted to a state
    // indistinguishable from the current one, making undo appear to do nothing.
    if (key != "dmxConsoleChannel" && key != "artNetConsoleChannel"
        && key != "dmxBlackout"    && key != "artNetBlackout"
        && ! key.startsWith ("audioPlayer"))
    {
        // Clear any pending settings snapshot — device change is a new action
        pendingSettingsSnapshot = juce::var();
        pendingSettingsNodeId.clear();
        graph.pushSnapshot();
    }

    if (key == "midiDeviceId" && onSetMidiDevice)
        handleSetNodeParam_MidiDeviceId (nodeId, value);
    else if (key == "audioDeviceId" && onSetAudioDevice)
        handleSetNodeParam_AudioDeviceId (nodeId, value);
    else if (key == "audioDeviceChannels" && onSetAudioDeviceChannels)
        handleSetNodeParam_AudioDeviceChannels (nodeId, value);
    else if (key == "udpSettings" && onSetUdpSettings)
        handleSetNodeParam_UdpSettings (nodeId, value);
    else if (key == "oscSettings" && onSetOscSettings)
        handleSetNodeParam_OscSettings (nodeId, value);
    else if (key == "mqttSubscribeSettings" && onSetMqttSubscribeSettings)
        handleSetNodeParam_MqttSubscribeSettings (nodeId, value);
    else if (key == "mqttPublishSettings" && onSetMqttPublishSettings)
        handleSetNodeParam_MqttPublishSettings (nodeId, value);
    else if (key == "mqttConsoleSend" && onMqttConsoleSend)
        handleSetNodeParam_MqttConsoleSend (nodeId, value);
    else if (key == "artNetSettings" && onSetArtNetSettings)
        handleSetNodeParam_ArtNetSettings (nodeId, value);
    else if (key == "dmxSettings" && onSetDmxSettings)
        handleSetNodeParam_DmxSettings (nodeId, value);
    else if (key == "dmxConsoleChannel" && onSetDmxConsoleChannel)
    {
        handleSetNodeParam_DmxConsoleChannel (nodeId, value);
        return;
    }
    else if (key == "dmxBlackout" && onSetDmxBlackout)
    {
        handleSetNodeParam_DmxBlackout (nodeId, value);
        return;
    }
    else if (key == "artNetConsoleChannel" && onSetArtNetConsoleChannel)
    {
        handleSetNodeParam_ArtNetConsoleChannel (nodeId, value);
        return;
    }
    else if (key == "artNetBlackout" && onSetArtNetBlackout)
    {
        handleSetNodeParam_ArtNetBlackout (nodeId, value);
        return;
    }
    else if (key == "artNetUniverseFilter")
    {
        handleSetNodeParam_ArtNetUniverseFilter (nodeId, value);
        return;
    }
    else if (key == "audioPlayerPlaying" && onAudioPlayerSetPlaying)
    {
        onAudioPlayerSetPlaying (nodeId, value == "1" || value == "true");
        return;
    }
    else if (key == "audioPlayerSeek" && onAudioPlayerSeek)
    {
        onAudioPlayerSeek (nodeId, value.getDoubleValue());
        return;
    }
    else if (key == "audioPlayerReturnToStart" && onAudioPlayerReturnToStart)
    {
        onAudioPlayerReturnToStart (nodeId);
        return;
    }
    else if ((key == "audioPlayerMode" || key == "audioPlayerSineFrequency" ||
              key == "audioPlayerNoiseType" || key == "audioPlayerLevel" ||
              key == "audioPlayerLoop") && onAudioPlayerSetLiveParam)
    {
        // Live update, bypassing the settingsJson/rebuild-only restoration
        // path entirely — see restoreAudioPlayerSettings()'s own comment.
        // A settings commit alone (setNodeSettings/commitSettingsChange)
        // only ever updates what gets SAVED; it was never enough on its
        // own to reach a currently-running AudioPlayerState, since that
        // only happens during a full graph rebuild (undo/redo, or
        // opening a project) — every other node type's own settings are
        // purely cosmetic, so this need never came up before.
        onAudioPlayerSetLiveParam (nodeId, key, value);
        return;
    }

    // Push updated graph so React reflects the new selectedDeviceId / settings
    pushGraphToUI();
    pushUndoState();
}

void WebBridge::handleSetNodeParam_MidiDeviceId (const juce::String& nodeId, const juce::String& value)
{
    onSetMidiDevice (nodeId, value);
}

void WebBridge::handleSetNodeParam_AudioDeviceId (const juce::String& nodeId, const juce::String& value)
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

void WebBridge::handleSetNodeParam_AudioDeviceChannels (const juce::String& nodeId, const juce::String& value)
{
    // Parse the JSON array of channel indices sent from React
    std::vector<int> channels;
    auto parsed = juce::JSON::parse (value);
    if (auto* arr = parsed.getArray())
        for (auto& elem : *arr) channels.push_back ((int) elem);

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

void WebBridge::handleSetNodeParam_UdpSettings (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { port, mode, targetHost, multicastAddr }
    auto parsed = juce::JSON::parse (value);
    int port                   = (int) parsed["port"];
    int mode                   = (int) parsed["mode"];
    juce::String targetHost    = parsed["targetHost"].toString();
    juce::String multicastAddr = parsed["multicastAddr"].toString();

    onSetUdpSettings (nodeId, port, mode, targetHost, multicastAddr);

    // PatchyProcessor::setUdpSettings already persists to settingsJson on
    // the GraphModel node directly — push the updated value to the UI.
    if (auto* nd = graph.findNode (nodeId))
        pushSettingsToUI (nodeId, nd->settingsJson);
}

void WebBridge::handleSetNodeParam_OscSettings (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { port, targetHost, oscAddress }
    auto parsed = juce::JSON::parse (value);
    int port                  = (int) parsed["port"];
    juce::String targetHost   = parsed["targetHost"].toString();
    juce::String oscAddress   = parsed["oscAddress"].toString();

    onSetOscSettings (nodeId, port, targetHost, oscAddress);

    if (auto* nd = graph.findNode (nodeId))
        pushSettingsToUI (nodeId, nd->settingsJson);
}

void WebBridge::handleSetNodeParam_MqttSubscribeSettings (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { host, port, topic, qos, username, password }
    auto parsed = juce::JSON::parse (value);
    juce::String host      = parsed["host"].toString();
    int port               = (int) parsed["port"];
    juce::String topic     = parsed["topic"].toString();
    int qos                = (int) parsed["qos"];
    juce::String username  = parsed["username"].toString();
    juce::String password  = parsed["password"].toString();

    onSetMqttSubscribeSettings (nodeId, host, port, topic, qos, username, password);

    if (auto* nd = graph.findNode (nodeId))
        pushSettingsToUI (nodeId, nd->settingsJson);
}

void WebBridge::handleSetNodeParam_MqttPublishSettings (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { host, port, topic, qos, retain, username, password }
    auto parsed = juce::JSON::parse (value);
    juce::String host      = parsed["host"].toString();
    int port               = (int) parsed["port"];
    juce::String topic     = parsed["topic"].toString();
    int qos                = (int) parsed["qos"];
    bool retain            = (bool) parsed["retain"];
    juce::String username  = parsed["username"].toString();
    juce::String password  = parsed["password"].toString();

    onSetMqttPublishSettings (nodeId, host, port, topic, qos, retain, username, password);

    if (auto* nd = graph.findNode (nodeId))
        pushSettingsToUI (nodeId, nd->settingsJson);
}

void WebBridge::handleSetNodeParam_MqttConsoleSend (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { topic, payload } — a one-shot trigger,
    // not a persisted setting, so no settingsJson push-back here (the
    // frontend already updated its own topic-history state locally
    // before sending, same as any other UI-only state).
    auto parsed = juce::JSON::parse (value);
    juce::String topic = parsed["topic"].toString();
    float payload       = (float) parsed["payload"];

    onMqttConsoleSend (nodeId, topic, payload);
}

void WebBridge::handleSetNodeParam_ArtNetSettings (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { universe, targetHost }
    auto parsed = juce::JSON::parse (value);
    int          universe   = (int) parsed["universe"];
    juce::String targetHost = parsed["targetHost"].toString();

    onSetArtNetSettings (nodeId, universe, targetHost);

    if (auto* nd = graph.findNode (nodeId))
        pushSettingsToUI (nodeId, nd->settingsJson);
}

void WebBridge::handleSetNodeParam_DmxSettings (const juce::String& nodeId, const juce::String& value)
{
    // value is a JSON object: { devicePath, universe }
    auto parsed = juce::JSON::parse (value);
    juce::String devicePath = parsed["devicePath"].toString();
    int          universe   = (int) parsed["universe"];

    onSetDmxSettings (nodeId, devicePath, universe);

    if (auto* nd = graph.findNode (nodeId))
        pushSettingsToUI (nodeId, nd->settingsJson);
}

void WebBridge::handleSetNodeParam_DmxConsoleChannel (const juce::String& nodeId, const juce::String& value)
{
    auto parsed  = juce::JSON::parse (value);
    int channel  = (int) parsed["channel"];
    int val      = (int) parsed["value"];
    onSetDmxConsoleChannel (nodeId, channel, (uint8_t) juce::jlimit (0, 255, val));
}

void WebBridge::handleSetNodeParam_DmxBlackout (const juce::String& nodeId, const juce::String& value)
{
    bool active = value.trim() == "true";
    onSetDmxBlackout (nodeId, active);
    pushGraphToUI();
}

void WebBridge::handleSetNodeParam_ArtNetConsoleChannel (const juce::String& nodeId, const juce::String& value)
{
    auto parsed  = juce::JSON::parse (value);
    int channel  = (int) parsed["channel"];
    int val      = (int) parsed["value"];
    onSetArtNetConsoleChannel (nodeId, channel, (uint8_t) juce::jlimit (0, 255, val));
}

void WebBridge::handleSetNodeParam_ArtNetBlackout (const juce::String& nodeId, const juce::String& value)
{
    bool active = value.trim() == "true";
    onSetArtNetBlackout (nodeId, active);
    pushGraphToUI();
}

void WebBridge::handleSetNodeParam_ArtNetUniverseFilter (const juce::String& nodeId, const juce::String& value)
{
    // Set universe filter on ArtNetMonitorNode (-1 = show all)
    int filter = value.trim().getIntValue();
    if (onSetArtNetUniverseFilter)
        onSetArtNetUniverseFilter (nodeId, filter);
}


void WebBridge::handleExportSelection (const juce::DynamicObject* obj)
{
    auto nodeIdsVar = obj->getProperty ("selectedNodeIds");
    auto suggestedName = obj->getProperty ("suggestedName").toString();
    juce::StringArray selectedNodeIds;
    if (auto* arr = nodeIdsVar.getArray())
        for (auto& elem : *arr)
            selectedNodeIds.add (elem.toString());
    if (selectedNodeIds.isEmpty()) return;
    showExportDialog (selectedNodeIds, suggestedName);
}

void WebBridge::handleImportFragmentNodes (const juce::DynamicObject* obj)
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
        juce::String paxName= nObj->getProperty ("paxName").toString();
        PaxPortSpec spec;
        spec.audioIn  = (int) nObj->getProperty ("audioInputs");
        spec.audioOut = (int) nObj->getProperty ("audioOutputs");
        spec.midiIn   = (int) nObj->getProperty ("midiInputs");
        spec.midiOut  = (int) nObj->getProperty ("midiOutputs");
        spec.valueIn  = (int) nObj->getProperty ("valueInputs");
        spec.valueOut = (int) nObj->getProperty ("valueOutputs");
        if (auto* arr = nObj->getProperty ("valueInputTypes").getArray())
            for (auto& tag : *arr) spec.valueInTypes.push_back (parseValueTypeTag (tag.toString()));
        if (auto* arr = nObj->getProperty ("valueOutputTypes").getArray())
            for (auto& tag : *arr) spec.valueOutTypes.push_back (parseValueTypeTag (tag.toString()));

        auto& nd = graph.restoreNode (savedId, nodeType, x, y, paxName, spec);
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

void WebBridge::handleSetAudioEngineSettings (const juce::DynamicObject* obj)
{
    double sr   = (double) obj->getProperty ("sampleRate");
    int    buf  = (int)    obj->getProperty ("bufferSize");
    bool   mute = (bool)   obj->getProperty ("muteFeedback");
    if (onSetAudioEngineSettings)
        onSetAudioEngineSettings (sr, buf, mute);
}
