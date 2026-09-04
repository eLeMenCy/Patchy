#include "PatchyProcessor.h"
#include "PatchyEditor.h"
#include "AudioDeviceNodes.h"
#include <utility>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────

PatchyProcessor::PatchyProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    // Scan for dynamic Pax at startup
    auto scanResults = PaxScanner::scan();
    registry.load (scanResults);
    juce::Logger::writeToLog (
        juce::String ("PatchyProcessor: ")
        + juce::String ((int) registry.getEntries().size())
        + juce::String (" Pax loaded"));

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
                             [this](const juce::String& nid) { return getOrCreateUdpMonitorBuffer(nid); },
                             [this](const juce::String& nid) { return getOrCreateMqttMonitorBuffer(nid); });
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

    // TEMPORARY diagnostic (2026-09-02) — a genuinely different question
    // from everything measured so far: not how long this function takes
    // once it's called, but whether it's being called ON TIME at all. The
    // user's own two rounds of precise timing data now show the swap,
    // prepare(), and even the very first process() call on fresh node
    // instances are all comparably fast to ordinary blocks — meaning
    // nothing measured so far explains a real, audible stutter. If the
    // audio callback itself is being delayed/starved by something
    // entirely outside this function (the UI thread, WebView rendering,
    // OS scheduling) specifically during a graph edit, that would sound
    // identical to a slow callback but require a completely different
    // fix — and wouldn't show up in anything timed from inside this
    // function. This measures the actual gap between consecutive calls
    // to this exact function, flagging it whenever it significantly
    // exceeds the expected block period for the current sample
    // rate/block size.
    {
        auto now = std::chrono::high_resolution_clock::now();
        if (lastCallbackTime.time_since_epoch().count() != 0)
        {
            auto gapMicros = std::chrono::duration_cast<std::chrono::microseconds> (now - lastCallbackTime).count();
            double expectedMicros = (lastSampleRate > 0.0)
                                     ? (1'000'000.0 * (double) buffer.getNumSamples() / lastSampleRate)
                                     : 11600.0;
            if ((double) gapMicros > expectedMicros * 1.5)
                juce::Logger::writeToLog ("PatchyProcessor: callback gap " + juce::String ((int) gapMicros)
                                          + " microseconds (expected ~" + juce::String ((int) expectedMicros) + ")");
        }
        lastCallbackTime = now;
    }

    // Swap in a newly built graph if one is pending
    if (graphPending.exchange (false))
    {
        // TEMPORARY diagnostic (2026-09-02), remove once resolved — 4
        // targeted fixes so far (the two documented below, plus a
        // MidiOutDeviceNode data race fix and making 3 per-block
        // containers persistent instead of reconstructed, both
        // elsewhere) have not conclusively resolved a real,
        // user-reported audio-stream glitch specifically on graph
        // edits. Rather than keep reasoning about what SHOULD be fast,
        // this measures exactly how long this whole swap block
        // genuinely takes, in microseconds, every single time it runs —
        // direct, empirical evidence rather than further speculation.
        auto timingStart = std::chrono::high_resolution_clock::now();

        // Real bug found and fixed 2026-09-02 — the previous version of
        // this swap called std::make_unique<ProcessingGraph>(...) directly
        // here, on the audio thread, every single time a graph rebuild's
        // own swap happened — a genuine, classic real-time-audio
        // anti-pattern: heap allocation is not guaranteed lock-free/
        // wait-free by the underlying allocator, and its timing can be
        // genuinely, unpredictably variable depending on the allocator's
        // own internal state and contention with other threads at that
        // exact moment. std::swap on two already-existing ProcessingGraph
        // objects (this member itself, and the one pendingGraph already
        // points to, both already fully constructed beforehand) only
        // needs a single stack-allocated temporary internally — genuinely
        // predictable, real-time-safe stack allocation, not a heap
        // allocation at all — since ProcessingGraph's own members are all
        // cheaply-movable standard containers (see its own defaulted move
        // constructor/assignment). pendingGraph itself, now correctly
        // holding the OLD graph's own contents after the swap, is moved
        // directly into graphTrash — no new allocation there either, just
        // a transfer of an already-existing unique_ptr's own ownership.
        std::swap (processingGraph, *pendingGraph);

        // Real fix, 2026-09-03 (3rd revision) — see AudioOutDeviceNode's
        // own transferCallbackTo() comment for the full story of why this
        // moved here specifically. At this exact point, *pendingGraph
        // holds the OLD graph's own nodes (via the swap just above) and
        // processingGraph holds the NEW, now-live one — the old graph is
        // no longer being processed by anyone (this swap is what made it
        // stop), and the message thread's own rebuildProcessingGraph()
        // finished constructing/configuring the new graph well before
        // this point — meaning the audio thread, right here, right now,
        // is the ONLY thread that could possibly touch either fifo,
        // eliminating the cross-thread race an earlier attempt at this
        // same transfer (on the message thread, during the rebuild
        // itself) risked. Only nodes the message thread already marked
        // wasTransferred() (meaning a genuinely matching, same-device
        // node exists in both graphs) are handled — everything else
        // (a genuinely new device selection, or no prior node at all)
        // correctly already went through a normal, full configure()/
        // openDevice() instead, with nothing here to transfer from.
        for (auto& newNode : processingGraph.getNodes())
        {
            if (auto* newOut = dynamic_cast<AudioOutDeviceNode*> (newNode.get()))
            {
                if (newOut->wasTransferred())
                    if (auto* oldOut = pendingGraph->findAudioOutNode (newOut->id))
                        newOut->transferFifoFrom (*oldOut);
            }
            else if (auto* newIn = dynamic_cast<AudioInDeviceNode*> (newNode.get()))
            {
                if (newIn->wasTransferred())
                    if (auto* oldIn = pendingGraph->findAudioInNode (newIn->id))
                        newIn->transferFifoFrom (*oldIn);
            }
        }

        graphTrash = std::move (pendingGraph);
        graphTrashPending.store (true);

        processingGraph.isStandaloneMode = isStandalone;
        processingGraph.graphModel        = &graphModel;
        // Real fix, 2026-09-02 — the actual, expensive buffer allocation
        // work this call used to do now already happened on the message
        // thread, before this graph was ever handed over (see
        // rebuildProcessingGraph()'s own matching call and its own
        // comment for the full story) — every buffer here should already
        // be the exact right size, so this now correctly, cheaply no-ops
        // via setSize()'s own size-matches-already check. Kept as a
        // genuine safety net (e.g. if sample rate/block size somehow
        // changed between the rebuild and this exact moment), not removed
        // — the cost of keeping it is negligible once it's a no-op.
        processingGraph.prepare (lastSampleRate, lastBlockSize);

        auto timingEnd = std::chrono::high_resolution_clock::now();
        auto timingMicros = std::chrono::duration_cast<std::chrono::microseconds> (timingEnd - timingStart).count();
        juce::Logger::writeToLog ("PatchyProcessor: graph swap took " + juce::String ((int) timingMicros) + " microseconds");

        // TEMPORARY diagnostic (2026-09-02) — flags that the very next
        // process() call below is the FIRST one on freshly swapped-in
        // node instances, so it can be timed and logged specifically,
        // separately from the regular, randomly-sampled baseline. The
        // swap logic above is now confirmed fast (comparable to or
        // faster than ordinary blocks), but that alone doesn't confirm
        // whether these brand-new node instances' own very first
        // process() call — cold in CPU cache, potentially doing
        // something their own subsequent calls don't — behaves any
        // differently from a typical, already-warm, steady-state block.
        firstProcessAfterSwap = true;
    }

    // Clear any output channels that aren't used by inputs
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    for (int i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    if (processingGraph.isEmpty())
    {
        // No nodes — pass audio through silently, pass MIDI through
        firstProcessAfterSwap = false;   // TEMPORARY diagnostic (2026-09-02) — avoid mislabeling a later block
        return;
    }

    // TEMPORARY diagnostic (2026-09-02), remove once resolved — same
    // investigation as the swap-block timing above; logs a baseline,
    // normal (non-swap) block's own processing time for direct
    // comparison, throttled to roughly once every few seconds to avoid
    // flooding the log. Also separately, always logs the very first
    // process() call specifically on the block right after a swap (see
    // firstProcessAfterSwap's own comment above), regardless of the
    // throttle — this is the one measurement not yet taken.
    static int baselineLogCounter = 0;
    bool logThisBaseline = (++baselineLogCounter % 200 == 0) || firstProcessAfterSwap;
    auto baselineStart = logThisBaseline ? std::chrono::high_resolution_clock::now()
                                          : std::chrono::high_resolution_clock::time_point{};

    processingGraph.process (buffer, midiMessages);

    if (logThisBaseline)
    {
        auto baselineEnd = std::chrono::high_resolution_clock::now();
        auto baselineMicros = std::chrono::duration_cast<std::chrono::microseconds> (baselineEnd - baselineStart).count();
        if (firstProcessAfterSwap)
            juce::Logger::writeToLog ("PatchyProcessor: FIRST process() after swap took " + juce::String ((int) baselineMicros) + " microseconds");
        else
            juce::Logger::writeToLog ("PatchyProcessor: baseline (non-swap) block took " + juce::String ((int) baselineMicros) + " microseconds");
    }

    // Reset regardless of whether this block's own timing was logged —
    // this flag must only ever describe the ONE block immediately
    // following a swap, never any block after that.
    firstProcessAfterSwap = false;
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
                    if (t == 24) getOrCreateMqttMonitorBuffer (nid);
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
        pendingGraph->closeUdpBasedProtocolDeviceSockets();
    }

    // Close the CURRENT graph's UDP-based protocol device sockets (UDP/OSC)
    // before newGraph binds its own — prevents a bind race where the new
    // graph's socket loses to this graph's still-open one on the same port
    // and silently goes dead. See ProcessingGraph::closeAllProtocolDeviceSockets().
    //
    // Real bug found and fixed 2026-09-02 — this used to call the FULL
    // closeAllProtocolDeviceSockets(), which also closed ArtNet and DMX
    // devices here, unconditionally, on every single graph rebuild — but
    // both gained their own genuine connection-transfer mechanism the same
    // week (see DmxIn/OutDeviceNode's own transferOrConfigure()), and this
    // early, unconditional close was silently defeating it every time,
    // before the transfer ever got a chance to run. Confirmed as the exact
    // cause via precise diagnostic logging showing "old node's serial not
    // open" on every single graph edit without exception — the very
    // connection the transfer was trying to reuse had already been closed
    // moments earlier, right here. Switched to the narrower,
    // UDP/OSC-only version — see its own declaration in ProcessingGraph.h
    // for the full reasoning on why ArtNet/DMX genuinely don't need this
    // pre-emptive close the way UDP/OSC still do.
    processingGraph.closeUdpBasedProtocolDeviceSockets();

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
                       [this](const juce::String& nid) { return getOrCreateUdpMonitorBuffer(nid); },
                       [this](const juce::String& nid) { return getOrCreateMqttMonitorBuffer(nid); });

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
    midiDeviceManager.applyDeviceSelections  (*newGraph, pendingGraph ? pendingGraph.get() : &processingGraph);
    audioDeviceManager.applyDeviceSelections (*newGraph);
    audioDeviceManager.applyAllChannelSelections (*newGraph);
    udpDeviceManager.applyAllSettings            (*newGraph);
    oscDeviceManager.applyAllSettings            (*newGraph);
    mqttDeviceManager.applyAllSettings           (*newGraph);
    artNetDeviceManager.applyAllSettings         (*newGraph, pendingGraph ? pendingGraph.get() : &processingGraph);
    dmxDeviceManager.applyAllSettings            (*newGraph, pendingGraph ? pendingGraph.get() : &processingGraph);

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

                    // Real bug found 2026-08-30, first fix attempt found
                    // NOT to work by the user's own direct testing, real
                    // root cause found and properly fixed 2026-08-31 —
                    // reconnecting a console after changing its value
                    // while disconnected left the monitor stuck at the
                    // OLD value; only a further, genuinely new change
                    // (a nudge) fixed it. First attempt directly
                    // transferred outputDmxFrame/outputDmxFrameValid here
                    // (reasoning: a fresh instance starts those at their
                    // own defaults, since only lastSent above was ever
                    // transferred) — that reasoning was correct as far as
                    // it went, but missed that ProcessingGraph::process()'s
                    // own resetBuffers() unconditionally clears
                    // outputDmxFrameValid at the START of every single
                    // block, for every node — including the very next
                    // block after this transfer runs, before anything
                    // downstream ever gets to read it. Since this
                    // console's own lastSent now already matches its
                    // current fader state (transferred above), process()'s
                    // own change-detection correctly, silently sees
                    // "nothing changed" and never re-sets the flag back to
                    // true — so the direct transfer got wiped before it
                    // could ever matter.
                    //
                    // The real fix uses the mechanism this project already
                    // built for exactly this situation: pendingOutput,
                    // which restoreChannels() below already sets when ITS
                    // OWN comparison detects a genuine settingsJson-vs-
                    // lastSent difference — process()'s own change-
                    // detection already checks this flag and forces a
                    // re-emission even when current==lastSent. The gap was
                    // that lastSent gets transferred to already match the
                    // restored value in this exact reconnection scenario,
                    // so restoreChannels()'s own comparison also correctly
                    // sees "no difference" and never sets pendingOutput
                    // either — nothing in the whole chain realises a fresh
                    // re-population is needed regardless, since any
                    // reconnection means a downstream node's own source
                    // cache was pruned to empty during the disconnection.
                    // Forcing it unconditionally on every reconnection
                    // closes that gap directly, verified against the
                    // user's own exact diagnostic with a standalone
                    // simulation before considering this correct.
                    newNode->forceReEmit();
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

                    // Same real bug and same fix as DmxConsoleNode's own
                    // above (see that block's own comment for the full
                    // story).
                    newNode->forceReEmit();
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

    // Real bug found and fixed 2026-09-02 — prepare() calls
    // juce::AudioBuffer::setSize() on every node's own input/output audio
    // buffers, for every single node in the graph. Every node instance is
    // freshly constructed on every single rebuild (this project's own
    // established, unavoidable pattern — see ProcessingGraph::rebuild()'s
    // own "destroys and recreates every node" behaviour), meaning each
    // one's own audio buffers start completely empty — confirmed directly
    // against JUCE's own real source: setSize() only skips its own
    // allocation when the requested size already exactly matches the
    // buffer's current size, which can never be true for a buffer that
    // starts at zero. This meant every node's own prepare() call was
    // GENUINELY, unavoidably allocating memory — and this whole sequence
    // used to run entirely on the audio thread, right after the graph
    // swap in processBlock() — for potentially dozens of nodes, every
    // single graph edit. Investigated while chasing a user report of a
    // longstanding, general, variable-severity audio-stream glitch
    // specifically and ONLY on graph edits (drop/remove/connect/
    // disconnect), confirmed via the user's own precise, methodical
    // testing to never happen on purely visual actions (moving nodes,
    // zooming, opening settings) that don't trigger a rebuild at all —
    // exactly the signature this mechanism would produce, and a stronger,
    // more complete match than either of this same day's two earlier
    // fixes. Doing this real allocation work here instead — on the
    // message thread, before this graph is ever handed to the audio
    // thread at all — means the identical prepare() call that still runs
    // on the audio thread afterwards (see processBlock()'s own swap
    // logic) finds every buffer already the exact right size, and
    // setSize()'s own check correctly, genuinely skips any further
    // allocation entirely.
    newGraph->isStandaloneMode = isStandalone;
    newGraph->graphModel        = &graphModel;
    newGraph->prepare (lastSampleRate, lastBlockSize);

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
        PaxPortSpec portSpec;
        if (paxName.isNotEmpty())
        {
            for (const auto& e : registry.getEntries())
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
        auto& restoredNode = graphModel.restoreNode (
            nd->getProperty ("id").toString(),
            (int)   nd->getProperty ("nodeType"),
            (float) nd->getProperty ("x"),
            (float) nd->getProperty ("y"),
            paxName, portSpec);
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
