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
                             [this](const juce::String& nid) { return getOrCreateKeyboardMonitorBuffer(nid); });
    processingGraph.isStandaloneMode = isStandalone;
    processingGraph.graphModel        = &graphModel;
    processingGraph.prepare (sampleRate, samplesPerBlock);
midiDeviceManager.applyDeviceSelections  (processingGraph);
    audioDeviceManager.applyDeviceSelections (processingGraph);
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
                }
            }
        }
    }

    // Clear any previously trashed graph now (message thread — safe for destructors)
    if (graphTrashPending.load()) { graphTrashPending.store(false); graphTrash.reset(); }

    // Only close a pendingGraph that was never swapped in — its audio nodes
    // have open callbacks that need to be released before we replace it.
    if (pendingGraph != nullptr)
        pendingGraph->closeAllAudioDevices();

    auto newGraph = std::make_unique<ProcessingGraph>();
    newGraph->rebuild (graphModel, &registry,
                       [this](const juce::String& nid) { return getOrCreateMidiMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateAudioMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateKeyboardMonitorBuffer(nid); });

    // Transfer existing open audio device connections to the new graph nodes
    // rather than closing and reopening — this avoids the ~1 second audio gap.
    // Only nodes that exist in both graphs get their callback transferred.
    newGraph->transferAudioDevicesFrom (processingGraph);

    // Sync device manager selections from current graphModel state.
    // This ensures undo/redo restores correctly — the model has already been
    // updated before rebuildProcessingGraph runs, so we always apply the
    // right selections including empty ones (which trigger closeDevice).
    bool selectionsChanged = false;
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
    }

    // If selections changed (e.g. after undo), close all transferred devices
    // so applyDeviceSelections can open the correct ones freely.
    if (selectionsChanged)
        newGraph->closeAllTransferredAudioDevices();

    // Apply selections to new graph (opens/closes devices as needed)
    midiDeviceManager.applyDeviceSelections  (*newGraph);
    audioDeviceManager.applyDeviceSelections (*newGraph);
    audioDeviceManager.applyAllChannelSelections (*newGraph);

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
        juce::String paxName = nd->getProperty ("addonName").toString();
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
        graphModel.addConnection (
            cd->getProperty ("sourceNodeId").toString(),
            cd->getProperty ("sourcePortId").toString(),
            cd->getProperty ("targetNodeId").toString(),
            cd->getProperty ("targetPortId").toString());
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
