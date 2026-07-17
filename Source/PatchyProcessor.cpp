#include "PatchyProcessor.h"
#include "PatchyEditor.h"
#include "AudioDeviceNodes.h"

// ─────────────────────────────────────────────────────────────────────────────

PatchyProcessor::PatchyProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // Scan for dynamic addons at startup
    auto scanResults = PaxScanner::scan();
    registry.load (scanResults);
    juce::Logger::writeToLog (
        juce::String ("PatchyProcessor: ")
        + juce::String ((int) registry.getEntries().size())
        + juce::String (" addon(s) loaded"));

    // When the graph changes: rebuild processing graph AND push to UI
    graphModel.onChange = [this]
    {
        // Clear any old trashed graph first (safe on message thread)
        if (graphTrashPending.load())
        {
            graphTrashPending.store (false);
            graphTrash.reset();
        }
        rebuildProcessingGraph();
        // Notify the editor to push updated graph to the WebView
        if (auto* ed = dynamic_cast<PatchyEditor*> (getActiveEditor()))
        {
            ed->getBridge().pushGraphToUI();
            ed->getBridge().pushUndoState();
        }
    };

    // After undo/redo restores a snapshot, resync device managers so
    // applyDeviceSelections() uses the restored selectedDeviceId values
    // rather than the stale pre-undo selections.
    // onAfterRestore is not needed — rebuildProcessingGraph syncs device
    // selections directly from graphModel before applying them.
}

// ─────────────────────────────────────────────────────────────────────────────

bool PatchyProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Accept stereo in/out or mono in/out
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────

void PatchyProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    lastSampleRate = sampleRate;
    lastBlockSize  = samplesPerBlock;
    processingGraph.rebuild (graphModel, &registry,
                             [this](const juce::String& nid) { return getOrCreateMidiMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateAudioMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateKeyboardMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateDmxMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateDmxConsoleBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateArtNetMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateArtNetConsoleBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateOscMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateUdpMonitorBuffer(nid); });
    processingGraph.isStandaloneMode = isStandalone;
    processingGraph.graphModel        = &graphModel;
    processingGraph.prepare (sampleRate, samplesPerBlock);
midiDeviceManager.applyDeviceSelections  (processingGraph);
    audioDeviceManager.applyDeviceSelections (processingGraph);
    udpDeviceManager.applyAllSettings        (processingGraph);
    oscDeviceManager.applyAllSettings        (processingGraph);
    mqttDeviceManager.applyAllSettings       (processingGraph);
    artNetDeviceManager.applyAllSettings     (processingGraph);
    dmxDeviceManager.applyAllSettings        (processingGraph);
}

// ─────────────────────────────────────────────────────────────────────────────

void PatchyProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                     juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // Swap in a newly built graph if one is pending
    if (graphPending.exchange (false))
    {
        // Move old graph to trash bin — it will be destroyed on the message thread.
        // This prevents audio-device destructors from running on the audio thread.
        auto oldGraph = std::make_unique<ProcessingGraph> (std::move (processingGraph));
        graphTrash = std::move (oldGraph);
        graphTrashPending.store (true);

        processingGraph = std::move (*pendingGraph);
        pendingGraph.reset();
        processingGraph.isStandaloneMode = isStandalone;
        processingGraph.graphModel        = &graphModel;
        processingGraph.prepare (lastSampleRate, lastBlockSize);
    }

    // Clear any output channels that aren't used by inputs
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    if (processingGraph.isEmpty())
    {
        // No nodes — pass audio through silently, pass MIDI through
        return;
    }

    processingGraph.process (buffer, midiMessages);
}

// ─────────────────────────────────────────────────────────────────────────────

void PatchyProcessor::rebuildProcessingGraph()
{

    // Pre-create monitor buffers for type-5 (MIDI) and type-6 (Audio) nodes.
    auto snapshot = graphModel.toVar();
    if (auto* root = snapshot.getDynamicObject())
    {
        if (auto* nodesArr = root->getProperty ("nodes").getArray())
        {
            for (const auto& nv : *nodesArr)
            {
                if (auto* nd = nv.getDynamicObject())
                {
                    int t = (int) nd->getProperty ("nodeType");
                    juce::String nid = nd->getProperty ("id").toString();
                    if (t == 5) getOrCreateMidiMonitorBuffer      (nid);
                    if (t == 6) getOrCreateAudioMonitorBuffer (nid);
                    if (t == 20) getOrCreateOscMonitorBuffer  (nid);
                    if (t == 21) getOrCreateUdpMonitorBuffer  (nid);
                }
            }
        }
    }

    // Clear any previously trashed graph now (message thread — safe for destructors)
    if (graphTrashPending.load()) { graphTrashPending.store(false); graphTrash.reset(); }

    // Only close a pendingGraph that was never swapped in — its audio nodes
    // have open callbacks that need to be released before we replace it.
    if (pendingGraph != nullptr)
    {
        pendingGraph->closeAllAudioDevices();
        pendingGraph->closeAllProtocolDeviceSockets();
    }

    // Close the CURRENT graph's protocol device sockets (UDP/OSC/ArtNet/DMX)
    // before newGraph binds its own — prevents a bind race where the new
    // graph's socket loses to this graph's still-open one on the same port
    // and silently goes dead. See ProcessingGraph::closeAllProtocolDeviceSockets().
    processingGraph.closeAllProtocolDeviceSockets();

    auto newGraph = std::make_unique<ProcessingGraph>();
    newGraph->rebuild (graphModel, &registry,
                       [this](const juce::String& nid) { return getOrCreateMidiMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateAudioMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateKeyboardMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateDmxMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateDmxConsoleBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateArtNetMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateArtNetConsoleBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateOscMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateUdpMonitorBuffer(nid); });

    // Transfer existing open audio device connections to the new graph nodes
    // rather than closing and reopening — this avoids the ~1 second audio gap.
    // Only nodes that exist in both graphs get their callback transferred.
    newGraph->transferAudioDevicesFrom (processingGraph);

    // Sync device manager selections from current graphModel state.
    // This ensures undo/redo restores correctly — the model has already been
    // updated before rebuildProcessingGraph runs, so we always apply the
    // right selections including empty ones (which trigger closeDevice).
    bool selectionsChanged = false;
    struct ChannelRestore { juce::String nodeId, settingsJson; int nodeType = 17; };
    std::vector<ChannelRestore> channelRestores;
    for (const auto& n : graphModel.getNodes())
    {
        if (n.nodeType == 1 || n.nodeType == 2)
            midiDeviceManager.storeSelection (n.id, n.selectedDeviceId);
        else if (n.nodeType == 3 || n.nodeType == 4)
        {
            juce::String devName = n.selectedDeviceId;
            if (! isStandalone && devName.isNotEmpty()) devName = "DAW";
            juce::String prev = audioDeviceManager.getSelection (n.id);
            if (prev != devName)
            {
                audioDeviceManager.storeSelection (n.id, devName);
                selectionsChanged = true;
            }

            // Restore selectedChannels from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    if (auto* arr = parsed["selectedChannels"].getArray())
                    {
                        std::vector<int> channels;
                        channels.reserve (static_cast<size_t>(arr->size()));
                        for (auto& v : *arr)
                            channels.push_back ((int) v);
                        if (! channels.empty())
                            audioDeviceManager.storeChannelSelection (n.id, channels);
                    }
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 8 || n.nodeType == 9)
        {
            // Restore UDP settings from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    UdpDeviceManager::Settings s;
                    s.port          = (int) parsed["udpPort"];
                    s.mode          = static_cast<UdpMode> ((int) parsed["udpMode"]);
                    s.targetHost    = parsed["udpTargetHost"].toString();
                    s.multicastAddr = parsed["udpMulticastAddr"].toString();
                    if (s.port > 0)
                        udpDeviceManager.storeSettings (n.id, s);
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 10 || n.nodeType == 11)
        {
            // Restore OSC settings from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    OscDeviceManager::Settings s;
                    s.port       = (int) parsed["oscPort"];
                    s.targetHost = parsed["oscTargetHost"].toString();
                    s.oscAddress = parsed["oscAddress"].toString();
                    if (s.oscAddress.isEmpty()) s.oscAddress = "/patchy";
                    if (s.port > 0)
                        oscDeviceManager.storeSettings (n.id, s);
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 22)
        {
            // Restore MQTT Subscribe settings from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    MqttDeviceManager::Settings s;
                    s.host     = parsed["mqttHost"].toString();
                    s.port     = (int) parsed["mqttPort"];
                    s.topic    = parsed["mqttTopic"].toString();
                    s.qos      = (int) parsed["mqttQos"];
                    s.username = parsed["mqttUsername"].toString();
                    s.password = parsed["mqttPassword"].toString();
                    if (s.port <= 0) s.port = 1883;
                    if (s.host.isNotEmpty() && s.topic.isNotEmpty())
                        mqttDeviceManager.storeSettings (n.id, s);
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 23)
        {
            // Restore MQTT Publish settings from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    MqttDeviceManager::Settings s;
                    s.host     = parsed["mqttHost"].toString();
                    s.port     = (int) parsed["mqttPort"];
                    s.topic    = parsed["mqttTopic"].toString();
                    s.qos      = (int) parsed["mqttQos"];
                    s.retain   = (bool) parsed["mqttRetain"];
                    s.username = parsed["mqttUsername"].toString();
                    s.password = parsed["mqttPassword"].toString();
                    if (s.port <= 0) s.port = 1883;
                    if (s.host.isNotEmpty() && s.topic.isNotEmpty())
                        mqttDeviceManager.storeSettings (n.id, s);
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 12 || n.nodeType == 13)
        {
            // Restore ArtNet settings from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    ArtNetDeviceManager::Settings s;
                    s.universe   = (int) parsed["artNetUniverse"];
                    s.targetHost = parsed["artNetTargetHost"].toString();
                    artNetDeviceManager.storeSettings (n.id, s);
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 14 || n.nodeType == 15)
        {
            // Restore DMX settings from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty())
            {
                try
                {
                    auto parsed = juce::JSON::parse (n.settingsJson);
                    DmxDeviceManager::Settings s;
                    s.devicePath = parsed["dmxDevicePath"].toString();
                    s.universe   = (int) parsed["dmxUniverse"];
                    if (s.devicePath.isNotEmpty())
                        dmxDeviceManager.storeSettings (n.id, s);
                }
                catch (...) {}
            }
        }
        else if (n.nodeType == 17)
        {
            // Restore DMX Console channel values from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty() && n.settingsJson.contains ("dmxChannels"))
                channelRestores.push_back ({ n.id, n.settingsJson, 17 });
        }
        else if (n.nodeType == 19)
        {
            // Restore ArtNet Console channel values from settingsJson (undo/redo safe)
            if (n.settingsJson.isNotEmpty() && n.settingsJson.contains ("artNetChannels"))
                channelRestores.push_back ({ n.id, n.settingsJson, 19 });
        }
    }

    // If selections changed (e.g. after undo), close all transferred devices
    // so applyDeviceSelections can open the correct ones freely.
    if (selectionsChanged)
        newGraph->closeAllTransferredAudioDevices();

    // Apply selections to new graph (opens/closes devices as needed)
    midiDeviceManager.applyDeviceSelections  (*newGraph);
    audioDeviceManager.applyDeviceSelections (*newGraph);
    audioDeviceManager.applyAllChannelSelections (*newGraph);
    udpDeviceManager.applyAllSettings            (*newGraph);
    oscDeviceManager.applyAllSettings            (*newGraph);
    mqttDeviceManager.applyAllSettings           (*newGraph);
    artNetDeviceManager.applyAllSettings         (*newGraph);
    dmxDeviceManager.applyAllSettings            (*newGraph);

    // Transfer lastSent from most recent graph to ALL DMX/ArtNet Console nodes
    for (const auto& n : graphModel.getNodes())
    {
        if (n.nodeType == 17)
        {
            auto* oldNode = pendingGraph ? pendingGraph->findDmxConsoleNode (n.id)
                                         : processingGraph.findDmxConsoleNode (n.id);
            if (!oldNode) oldNode = processingGraph.findDmxConsoleNode (n.id);
            if (auto* newNode = newGraph->findDmxConsoleNode (n.id))
                if (oldNode)
                {
                    newNode->transferLastSent (oldNode->getLastSent());
                    // Read blackout from settingsJson (always up to date) not from node atomic
                    bool bo = false;
                    try { bo = (bool) juce::JSON::parse (n.settingsJson)["blackout"]; } catch (...) {}
                    newNode->transferBlackout (bo);
                }
        }
        else if (n.nodeType == 19)
        {
            auto* oldNode = pendingGraph ? pendingGraph->findArtNetConsoleNode (n.id)
                                         : processingGraph.findArtNetConsoleNode (n.id);
            if (!oldNode) oldNode = processingGraph.findArtNetConsoleNode (n.id);
            if (auto* newNode = newGraph->findArtNetConsoleNode (n.id))
                if (oldNode)
                {
                    newNode->transferLastSent (oldNode->getLastSent());
                    bool bo = false;
                    try { bo = (bool) juce::JSON::parse (n.settingsJson)["blackout"]; } catch (...) {}
                    newNode->transferBlackout (bo);
                }
        }
    }

    // Restore Console channel values AFTER rebuild
    for (const auto& r : channelRestores)
    {
        if (r.nodeType == 17)
            restoreDmxConsoleChannels (r.nodeId, r.settingsJson, newGraph.get());
        else if (r.nodeType == 19)
            restoreArtNetConsoleChannels (r.nodeId, r.settingsJson, newGraph.get());
    }

    pendingGraph = std::move (newGraph);
    graphPending.store (true);
}
juce::AudioProcessorEditor* PatchyProcessor::createEditor()
{
    auto* ed = new PatchyEditor (*this);
    ed->getBridge().isStandalone = isStandalone;
    return ed;
}

// ─────────────────────────────────────────────────────────────────────────────

void PatchyProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Serialise the graph model to JSON and store it
    auto json = juce::JSON::toString (graphModel.toVar(), true);
    destData.replaceAll (json.toRawUTF8(), static_cast<size_t>(json.getNumBytesAsUTF8()));
}

// ─────────────────────────────────────────────────────────────────────────────
/** Migrates a legacy port ID that used the old generic "Value In"/"Value Out"
 *  label to the new protocol-specific label (e.g. "UDP Out", "MQTT In"),
 *  based on the owning node's current nodeType.
 *
 *  Needed because Port::id embeds the label text directly (see
 *  GraphModel::portsForType's mk() helper: `id = nodeId + "_" + label + "_"
 *  + dir`) — a saved connection referencing the old "..._Value Out_out"
 *  string would otherwise silently fail to reconnect once the label was
 *  changed to fix the cross-protocol port-typing gap (UDP and MQTT nodes
 *  used to share the generic PortType::Value, allowing them to be wired
 *  directly together — see Architecture.md's locked decisions for the full
 *  story). This keeps existing saved .patchy projects working unchanged. */
static juce::String migrateLegacyPortId (const juce::String& portId, int nodeType)
{
    static const std::unordered_map<int, juce::String> newOutLabel = {
        { 8,  "UDP Out"  },   // UdpInDeviceNode
        { 21, "UDP Out"  },   // UdpMonitorNode (also has an In, handled below)
        { 22, "MQTT Out" },   // MqttSubscribeNode
    };
    static const std::unordered_map<int, juce::String> newInLabel = {
        { 9,  "UDP In"  },    // UdpOutDeviceNode
        { 21, "UDP In"  },    // UdpMonitorNode
        { 23, "MQTT In" },    // MqttPublishNode
    };

    if (portId.endsWith ("_Value Out_out"))
    {
        auto it = newOutLabel.find (nodeType);
        if (it != newOutLabel.end())
            return portId.upToLastOccurrenceOf ("_Value Out_out", false, false) + "_" + it->second + "_out";
    }
    else if (portId.endsWith ("_Value In_in"))
    {
        auto it = newInLabel.find (nodeType);
        if (it != newInLabel.end())
            return portId.upToLastOccurrenceOf ("_Value In_in", false, false) + "_" + it->second + "_in";
    }
    return portId;   // unchanged — not a legacy Value port, or node isn't UDP/MQTT
}

void PatchyProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::String json (static_cast<const char*> (data), static_cast<size_t>(sizeInBytes));
    auto v = juce::JSON::parse (json);
    if (! v.isObject()) return;

    auto* root = v.getDynamicObject();
    if (root == nullptr) return;

    // Suspend onChange — resumeNotifications() MUST be called before any return.
    // We use a ScopeGuard to guarantee this even on early exit.
    graphModel.suspendNotifications();
    struct ResumeGuard {
        GraphModel& m;
        ~ResumeGuard() { m.resumeNotifications(); }
    } guard { graphModel };

    // Clear history — a freshly loaded graph starts with a clean undo stack.
    graphModel.clearHistory();

    // Restore nodes using their original saved IDs so connections match.
    const auto& nodesArr = *root->getProperty ("nodes").getArray();
    for (const auto& nv : nodesArr)
    {
        auto* nd = nv.getDynamicObject();
        if (nd == nullptr) continue;
        juce::String paxName = nd->getProperty ("paxName").toString();
        int audioIn = 0, audioOut = 0, midiIn = 0, midiOut = 0;
        if (paxName.isNotEmpty())
        {
            for (const auto& e : registry.getEntries())
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
        auto& restoredNode = graphModel.restoreNode (
            nd->getProperty ("id").toString(),
            (int)   nd->getProperty ("nodeType"),
            (float) nd->getProperty ("x"),
            (float) nd->getProperty ("y"),
            paxName, audioIn, audioOut, midiIn, midiOut);
        restoredNode.selectedDeviceId = nd->getProperty ("selectedDeviceId").toString();
        restoredNode.settingsJson     = nd->getProperty ("settingsJson").toString();
    }

    // Restore connections (IDs now match because we used restoreNode)
    const auto& connsArr = *root->getProperty ("connections").getArray();
    for (const auto& cv : connsArr)
    {
        auto* cd = cv.getDynamicObject();
        if (cd == nullptr) continue;

        juce::String srcNodeId = cd->getProperty ("sourceNodeId").toString();
        juce::String srcPortId = cd->getProperty ("sourcePortId").toString();
        juce::String tgtNodeId = cd->getProperty ("targetNodeId").toString();
        juce::String tgtPortId = cd->getProperty ("targetPortId").toString();

        if (auto* srcNode = graphModel.findNode (srcNodeId))
            srcPortId = migrateLegacyPortId (srcPortId, srcNode->nodeType);
        if (auto* tgtNode = graphModel.findNode (tgtNodeId))
            tgtPortId = migrateLegacyPortId (tgtPortId, tgtNode->nodeType);

        graphModel.addConnection (srcNodeId, srcPortId, tgtNodeId, tgtPortId);
    }

    // Restore viewport
    graphModel.viewportX    = (float) root->getProperty ("viewportX");
    graphModel.viewportY    = (float) root->getProperty ("viewportY");
    graphModel.viewportZoom = (float) root->getProperty ("viewportZoom");
    if (graphModel.viewportZoom <= 0.0f) graphModel.viewportZoom = 1.0f;

    // (resumeNotifications called by ResumeGuard destructor at end of scope)

    // Re-open any saved device selections now that the graph is built
    const auto& nodesArr2 = *root->getProperty ("nodes").getArray();
    for (const auto& nv : nodesArr2)
    {
        auto* nd = nv.getDynamicObject();
        if (nd == nullptr) continue;
        int type = (int) nd->getProperty ("nodeType");
        if (type == 1 || type == 2)
        {
            juce::String devId = nd->getProperty ("selectedDeviceId").toString();
            if (devId.isNotEmpty())
                setMidiDevice (nd->getProperty ("id").toString(), devId);
        }
        if (type == 3 || type == 4)
        {
            juce::String devName = nd->getProperty ("selectedDeviceId").toString();
            // In DAW mode, always use DAW device (override any saved hardware device)
            if (! isStandalone)
                devName = "DAW";
            if (devName.isNotEmpty())
                setAudioDevice (nd->getProperty ("id").toString(), devName);
        }
        if (type == 6)
            getOrCreateAudioMonitorBuffer (nd->getProperty ("id").toString());
    }
}

// ─────────────────────────────────────────────────────────────────────────────

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PatchyProcessor();
}
