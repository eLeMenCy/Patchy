#include "ProcessingGraph.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "MidiMonitorNode.h"
#include "AudioMonitorNode.h"
#include <unordered_set>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// rebuild — called on message thread after graph edit
// ─────────────────────────────────────────────────────────────────────────────

void ProcessingGraph::rebuild (const GraphModel& model, PaxRegistry* reg,
                               std::function<MidiMonitorBuffer*(const juce::String&)>  getMidiBuffer,
                               std::function<AudioMonitorBuffer*(const juce::String&)> getAudioBuffer,
                               std::function<MidiMonitorBuffer*(const juce::String&)>  getKeyboardBuffer)
{
    auto snapshot = model.toVar();
    auto* root    = snapshot.getDynamicObject();
    if (root == nullptr) return;

    nodes.clear();
    edges.clear();
    nodeMap.clear();
    labelMap.clear();
    sortedNodes.clear();
    auto* registry = reg;

    // ── Build node processors ──────────────────────────────────────────────
    const auto& nodesArr = *root->getProperty ("nodes").getArray();
    for (const auto& nv : nodesArr)
    {
        auto* nd = nv.getDynamicObject();
        if (nd == nullptr) continue;

        juce::String id  = nd->getProperty ("id").toString();
        int          type = (int) nd->getProperty ("nodeType");

        juce::String paxName = nd->getProperty ("paxName").toString();

        std::unique_ptr<NodeProcessor> proc;

        if (paxName.isNotEmpty())
        {
            // Dynamic addon node — ONLY use registry, never fall through to built-ins.
            // Addon nodeType (1=MIDI, 2=Audio, 3=AV) is separate from
            // built-in nodeType (1=MidiInDevice … 4=AudioOutDevice).
            if (registry != nullptr)
                proc = registry->createNode (id, paxName);

            if (proc == nullptr)
                juce::Logger::writeToLog ("ProcessingGraph: addon not found: " + paxName);
            // Leave proc as nullptr — node will be skipped in processing
        }
        else
        {
            // Built-in device node (1=MidiIn, 2=MidiOut, 3=AudioIn, 4=AudioOut)
            switch (type)
            {
                case 1:  proc = std::make_unique<MidiInDeviceNode>   (id); break;
                case 2:  proc = std::make_unique<MidiOutDeviceNode>  (id); break;
                case 3:  proc = std::make_unique<AudioInDeviceNode>  (id); break;
                case 4:  proc = std::make_unique<AudioOutDeviceNode> (id); break;
                case 5:  proc = std::make_unique<MidiMonitorNode>     (id, getMidiBuffer   ? getMidiBuffer(id)   : nullptr); break;
                case 6:  proc = std::make_unique<AudioMonitorNode>  (id, getAudioBuffer  ? getAudioBuffer(id)  : nullptr); break;
                case 7:  proc = std::make_unique<MidiKeyboardNode>  (id, getKeyboardBuffer ? getKeyboardBuffer(id) : nullptr); break;
                default:
                    juce::Logger::writeToLog ("ProcessingGraph: unknown built-in type " + juce::String (type));
                    break;
            }
        }

        if (proc != nullptr)
        {
            // Restore addon parameters from settingsJson so a graph rebuild
            // (e.g. dropping a node or adding a connection) doesn't reset sliders
            if (auto* dyn = dynamic_cast<DynamicPaxProcessor*> (proc.get()))
            {
                auto settingsJson = nd->getProperty ("settingsJson").toString();
                if (settingsJson.isNotEmpty())
                {
                    auto parsed = juce::JSON::parse (settingsJson);
                    if (auto* arr = parsed.getArray())
                    {
                        for (int i = 0; i < arr->size(); ++i)
                            dyn->setParameter (i, (float) (double) (*arr)[i]);
                    }
                }
            }

            nodeMap[id]  = proc.get();
            labelMap[id] = nd->getProperty ("label").toString();
            nodes.push_back (std::move (proc));
        }
    }

    // ── Build edges ────────────────────────────────────────────────────────
    const auto& connsArr = *root->getProperty ("connections").getArray();
    for (const auto& cv : connsArr)
    {
        auto* cd = cv.getDynamicObject();
        if (cd == nullptr) continue;

        Edge e;
        e.srcNodeId = cd->getProperty ("sourceNodeId").toString();
        e.srcPortId = cd->getProperty ("sourcePortId").toString();
        e.dstNodeId = cd->getProperty ("targetNodeId").toString();
        e.dstPortId = cd->getProperty ("targetPortId").toString();
        edges.push_back (e);
    }

    topologicalSort();

    // Re-prepare if we already have host settings
    if (isPrepared)
        prepare (preparedSampleRate, preparedBlockSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// topologicalSort — Kahn's algorithm
// ─────────────────────────────────────────────────────────────────────────────

void ProcessingGraph::topologicalSort()
{
    sortedNodes.clear();

    // Count incoming edges per node
    std::unordered_map<juce::String, int> inDegree;
    for (auto& n : nodes)
        inDegree[n->id] = 0;

    for (auto& e : edges)
        inDegree[e.dstNodeId]++;

    // Queue nodes with no incoming edges (sources)
    std::vector<NodeProcessor*> queue;
    for (auto& n : nodes)
        if (inDegree[n->id] == 0)
            queue.push_back (n.get());

    while (! queue.empty())
    {
        auto* current = queue.back();
        queue.pop_back();
        sortedNodes.push_back (current);

        // Reduce in-degree of downstream nodes
        for (auto& e : edges)
        {
            if (e.srcNodeId == current->id)
            {
                if (--inDegree[e.dstNodeId] == 0)
                    if (auto it = nodeMap.find (e.dstNodeId); it != nodeMap.end())
                        queue.push_back (it->second);
            }
        }
    }

    // If sortedNodes.size() < nodes.size() there's a cycle — just append remaining
    if (sortedNodes.size() < nodes.size())
    {
        juce::Logger::writeToLog ("ProcessingGraph: cycle detected, appending remaining nodes");
        for (auto& n : nodes)
        {
            auto it = std::find (sortedNodes.begin(), sortedNodes.end(), n.get());
            if (it == sortedNodes.end())
                sortedNodes.push_back (n.get());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// prepare
// ─────────────────────────────────────────────────────────────────────────────

void ProcessingGraph::prepare (double sampleRate, int maxBlockSize)
{
    preparedSampleRate = sampleRate;
    preparedBlockSize  = maxBlockSize;
    isPrepared         = true;

    for (auto& n : nodes)
        n->prepare (sampleRate, maxBlockSize);
}

// ─────────────────────────────────────────────────────────────────────────────
// process — called on the audio thread
// ─────────────────────────────────────────────────────────────────────────────

void ProcessingGraph::process (juce::AudioBuffer<float>& hostAudio,
                                juce::MidiBuffer&         hostMidi)
{
    if (sortedNodes.empty()) return;

    const int numSamples = hostAudio.getNumSamples();

    // ── 1. Reset all node buffers ─────────────────────────────────────────
    for (auto* n : sortedNodes)
        n->resetBuffers (numSamples);

    // Build connectivity sets (used in steps 2 and 4)
    std::unordered_set<juce::String> hasInput;
    for (auto& e : edges)
        hasInput.insert (e.dstNodeId);

    std::unordered_set<juce::String> hasOutput;
    for (auto& e : edges)
        hasOutput.insert (e.srcNodeId);

    // ── 2. Feed inputs into source nodes ─────────────────────────────────

    for (auto* n : sortedNodes)
    {
        // AudioInDeviceNode: gets audio from physical device FIFO (standalone)
        // or from hostAudio (DAW mode via isDawDevice)
        if (auto* inNode = dynamic_cast<AudioInDeviceNode*> (n))
        {
            if (inNode->getIsDawDevice())
            {
                // DAW mode: inject host audio directly into outputAudio
                int ch = std::min (hostAudio.getNumChannels(), n->outputAudio.getNumChannels());
                for (int i = 0; i < ch; ++i)
                    n->outputAudio.copyFrom (i, 0, hostAudio, i, 0, numSamples);
            }
            // else: physical device — FIFO filled by audio callback, process() reads it
            continue;  // AudioInDeviceNode never gets hostAudio as inputAudio
        }

        // AudioOutDeviceNode: never gets hostAudio fed in — it's a pure sink
        if (dynamic_cast<AudioOutDeviceNode*> (n) != nullptr) continue;

        // Skip observer nodes — they only see what's explicitly connected
        if (dynamic_cast<MidiMonitorNode*>  (n) != nullptr) continue;
        if (dynamic_cast<AudioMonitorNode*>  (n) != nullptr) continue;
        if (dynamic_cast<MidiKeyboardNode*>  (n) != nullptr) continue;

        // Only AudioIn device nodes (non-DAW) get host audio as source
        // Addon/processing nodes with no connections stay silent
        if (hasInput.count (n->id) == 0)
        {
            if (dynamic_cast<AudioInDeviceNode*> (n) != nullptr)
            {
                // Physical AudioIn: host audio fed here (FIFO callback fills outputAudio)
                // inputAudio not used by AudioInDeviceNode — it reads from FIFO
                n->inputMidi = hostMidi;
            }
            else if (dynamic_cast<MidiInDeviceNode*> (n) != nullptr)
            {
                n->inputMidi = hostMidi;
            }
            // Addon/other nodes with no incoming connections stay silent
        }
    }

    // ── 3. Process each node in topological order ─────────────────────────
    for (auto* n : sortedNodes)
    {
        // Route edges: copy upstream outputAudio → this node's inputAudio
        for (auto& e : edges)
        {
            if (e.dstNodeId != n->id) continue;

            auto srcIt = nodeMap.find (e.srcNodeId);
            if (srcIt == nodeMap.end()) continue;
            auto* src = srcIt->second;

            if (isAudioPort (e.srcPortId))
            {
                int srcPortIdx = 0;
                auto srcLabel = e.srcPortId.fromLastOccurrenceOf (e.srcNodeId + "_", false, false)
                                           .upToLastOccurrenceOf ("_", false, false);
                if (srcLabel.containsIgnoreCase ("Audio Out "))
                    srcPortIdx = srcLabel.getTrailingIntValue() - 1;

                int dstPortIdx = 0;
                auto dstLabel = e.dstPortId.fromLastOccurrenceOf (e.dstNodeId + "_", false, false)
                                           .upToLastOccurrenceOf ("_", false, false);
                if (dstLabel.containsIgnoreCase ("Audio In "))
                    dstPortIdx = dstLabel.getTrailingIntValue() - 1;

                auto& srcBuf = (srcPortIdx < (int) src->outputAudioBuffers.size())
                               ? src->outputAudioBuffers[static_cast<size_t>(srcPortIdx)]
                               : src->outputAudio;
                auto& dstBuf = (dstPortIdx < (int) n->inputAudioBuffers.size())
                               ? n->inputAudioBuffers[static_cast<size_t>(dstPortIdx)]
                               : n->inputAudio;

                int chans = std::min (srcBuf.getNumChannels(), dstBuf.getNumChannels());
                for (int ch = 0; ch < chans; ++ch)
                    dstBuf.addFrom (ch, 0, srcBuf, ch, 0, numSamples);
            }
            else
            {
                for (auto meta : src->outputMidi)
                    n->inputMidi.addEvent (meta.getMessage(), meta.samplePosition);

                if (auto* mon = dynamic_cast<MidiMonitorNode*> (n))
                {
                    juce::String srcLabel = labelMap.count (src->id) ? labelMap.at (src->id) : src->id;
                    juce::String srcName;
                    if (auto* midiIn = dynamic_cast<MidiInDeviceNode*> (src))
                        srcName = midiIn->selectedDeviceName;
                    else if (auto* kbd = dynamic_cast<MidiKeyboardNode*> (src))
                        srcName = kbd->customName;
                    else if (auto* dyn = dynamic_cast<DynamicPaxProcessor*> (src))
                        srcName = dyn->customName;
                    mon->pushFromSource (src->outputMidi, srcLabel, srcName);
                }
            }
        }

        // DAW AudioIn: outputAudio already filled in step 2 — skip process()
        // to prevent FIFO read from overwriting it with silence
        if (auto* inNode = dynamic_cast<AudioInDeviceNode*> (n))
            if (inNode->getIsDawDevice()) continue;

        // DAW AudioOut: inputAudio will be collected below — skip process()
        // to prevent FIFO write (physical device not available in DAW)
        if (auto* outNode = dynamic_cast<AudioOutDeviceNode*> (n))
            if (outNode->getIsDawDevice()) continue;

        n->process (numSamples);
    }

    // ── 4. Collect outputs → hostAudio ───────────────────────────────────────
    hostMidi.clear();

    // DAW mode: ONLY replace hostAudio if a DAW AudioOut node has signal.
    // Otherwise always pass through — incomplete chains never block audio.
    // Standalone mode: collect non-device sinks into hostAudio.
    if (! isStandaloneMode)
    {
        // Only replace hostAudio if a DAW AudioOut node has an incoming connection
        // (i.e. it actually has signal to output). No connection = pass through.
        bool dawOutFound = false;
        for (auto* n : sortedNodes)
        {
            if (auto* outNode = dynamic_cast<AudioOutDeviceNode*> (n))
            {
                if (outNode->getIsDawDevice() && hasInput.count (n->id) > 0)
                {
                    if (! dawOutFound) { hostAudio.clear(); dawOutFound = true; }
                    int ch = std::min (n->inputAudio.getNumChannels(), hostAudio.getNumChannels());
                    for (int i = 0; i < ch; ++i)
                        hostAudio.addFrom (i, 0, n->inputAudio, i, 0, numSamples);
                }
            }
        }
        // No connected DAW AudioOut → hostAudio untouched (pass through)
    }
    else
    {
        // Standalone: AudioOutDeviceNode handles its own output via FIFO.
        // Other sink nodes (monitors, addons) are observers only — they must NOT
        // contribute to hostAudio. Audio only reaches the physical output via
        // an explicit AudioOutDeviceNode connection.
        // hostMidi sinks however are still collected.
        for (auto* n : sortedNodes)
        {
            if (hasOutput.count (n->id) > 0) continue;
            if (dynamic_cast<AudioOutDeviceNode*> (n) != nullptr) continue;
            if (dynamic_cast<AudioInDeviceNode*>  (n) != nullptr) continue;

            // Collect MIDI only — not audio
            for (auto meta : n->outputMidi)
                hostMidi.addEvent (meta.getMessage(), meta.samplePosition);
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
//  MIDI device node finders
// ─────────────────────────────────────────────────────────────────────────────

MidiOutDeviceNode* ProcessingGraph::findMidiOutNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<MidiOutDeviceNode*> (it->second);
}

MidiInDeviceNode* ProcessingGraph::findMidiInNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<MidiInDeviceNode*> (it->second);
}

AudioOutDeviceNode* ProcessingGraph::findAudioOutNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<AudioOutDeviceNode*> (it->second);
}

AudioInDeviceNode* ProcessingGraph::findAudioInNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<AudioInDeviceNode*> (it->second);
}

void ProcessingGraph::closeAllAudioDevices()
{
    // Close AudioIn and AudioOut device callbacks on the message thread
    // so they are not active when the graph is later destroyed/moved.
    for (auto& node : nodes)
    {
        if (auto* out = dynamic_cast<AudioOutDeviceNode*> (node.get()))
            out->closeDevice();
        if (auto* in  = dynamic_cast<AudioInDeviceNode*>  (node.get()))
            in->closeDevice();
    }
}

void ProcessingGraph::closeAllTransferredAudioDevices()
{
    // Close only nodes that were marked as transferred — used before
    // applyDeviceSelections when the desired device has changed (e.g. undo).
    // closeDevice() also resets the transferred flag so openDevice() works.
    for (auto& node : nodes)
    {
        if (auto* out = dynamic_cast<AudioOutDeviceNode*> (node.get()))
            if (out->wasTransferred()) out->closeDevice();
        if (auto* in  = dynamic_cast<AudioInDeviceNode*>  (node.get()))
            if (in->wasTransferred()) in->closeDevice();
    }
}

void ProcessingGraph::transferAudioDevicesFrom (ProcessingGraph& source)
{
    for (auto& newNode : nodes)
    {
        if (auto* newOut = dynamic_cast<AudioOutDeviceNode*> (newNode.get()))
        {
            if (auto* srcOut = source.findAudioOutNode (newOut->id))
            {
                // Transfer based on nodeId match — same node survives rebuild with same device.
                // New node's registeredDeviceName is empty at this point (openDevice not called yet),
                // so we compare source device name only — if it has one, transfer it.
                if (! srcOut->getSelectedDeviceName().isEmpty())
                {
                    srcOut->transferCallbackTo (*newOut);
                    newOut->markTransferred();
                }
            }
        }
        else if (auto* newIn = dynamic_cast<AudioInDeviceNode*> (newNode.get()))
        {
            if (auto* srcIn = source.findAudioInNode (newIn->id))
            {
                if (! srcIn->getSelectedDeviceName().isEmpty())
                {
                    srcIn->transferCallbackTo (*newIn);
                    newIn->markTransferred();
                }
            }
        }
    }
    source.closeAllAudioDevices();
}

