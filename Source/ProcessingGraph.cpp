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

void ProcessingGraph::rebuild (const GraphModel& model, AddonRegistry* reg,
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

        juce::String addonName = nd->getProperty ("addonName").toString();

        std::unique_ptr<NodeProcessor> proc;

        if (addonName.isNotEmpty())
        {
            // Dynamic addon node — ONLY use registry, never fall through to built-ins.
            // Addon nodeType (1=MIDI, 2=Audio, 3=AV) is separate from
            // built-in nodeType (1=MidiInDevice … 4=AudioOutDevice).
            if (registry != nullptr)
                proc = registry->createNode (id, addonName);

            if (proc == nullptr)
                juce::Logger::writeToLog ("ProcessingGraph: addon not found: " + addonName);
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

    // ── 2. Feed host input into source nodes (no incoming connections) ────
    //    Build set of nodes that have at least one incoming edge
    std::unordered_set<juce::String> hasInput;
    for (auto& e : edges)
        hasInput.insert (e.dstNodeId);

    for (auto* n : sortedNodes)
    {
        if (hasInput.count (n->id) == 0)
        {
            // Monitor nodes are pass-through observers — skip host input injection.
            // They only receive data from explicitly connected upstream nodes.
            if (dynamic_cast<MidiMonitorNode*>  (n) != nullptr) continue;
            if (dynamic_cast<AudioMonitorNode*>  (n) != nullptr) continue;
            if (dynamic_cast<MidiKeyboardNode*>  (n) != nullptr) continue;

            // Source node: give it the host's audio and MIDI
            int chansToFeed = std::min (hostAudio.getNumChannels(),
                                        n->inputAudio.getNumChannels());
            for (int ch = 0; ch < chansToFeed; ++ch)
                n->inputAudio.copyFrom (ch, 0, hostAudio, ch, 0, numSamples);

            n->inputMidi = hostMidi;
        }
    }

    // ── 3. Process each node in topological order ─────────────────────────
    for (auto* n : sortedNodes)
    {
        // Copy outputs from upstream neighbours into this node's inputs
        for (auto& e : edges)
        {
            if (e.dstNodeId != n->id) continue;

            auto srcIt = nodeMap.find (e.srcNodeId);
            if (srcIt == nodeMap.end()) continue;
            auto* src = srcIt->second;

            if (isAudioPort (e.srcPortId))
            {
                // Determine which port index this connection uses
                // Port ID format: "nodeId_Audio Out_out" or "nodeId_Audio Out 2_out"
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

                // Use per-port buffers when available, otherwise single buffer
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
                // Merge MIDI from source output into this node's input
                for (auto meta : src->outputMidi)
                    n->inputMidi.addEvent (meta.getMessage(), meta.samplePosition);

                // For MidiMonitorNode: push events with per-edge source info
                // so each event correctly shows which node/device it came from
                if (auto* mon = dynamic_cast<MidiMonitorNode*> (n))
                {
                    juce::String srcLabel = labelMap.count (src->id) ? labelMap.at (src->id) : src->id;
                    juce::String srcName;
                    if (auto* midiIn = dynamic_cast<MidiInDeviceNode*> (src))
                        srcName = midiIn->selectedDeviceName;
                    else if (auto* kbd = dynamic_cast<MidiKeyboardNode*> (src))
                        srcName = kbd->customName;
                    else if (auto* dyn = dynamic_cast<DynamicNodeProcessor*> (src))
                        srcName = dyn->customName;
                    mon->pushFromSource (src->outputMidi, srcLabel, srcName);
                }
            }
        }

        n->process (numSamples);

    }

    // ── 4. Collect sink node outputs → host buffer ────────────────────────
    //    Sink = nodes with no outgoing connections
    std::unordered_set<juce::String> hasOutput;
    for (auto& e : edges)
        hasOutput.insert (e.srcNodeId);

    hostAudio.clear();
    hostMidi.clear();

    for (auto* n : sortedNodes)
    {
        if (hasOutput.count (n->id) > 0) continue;   // not a sink

        // Mix audio output into host buffer
        int chans = std::min (n->outputAudio.getNumChannels(),
                              hostAudio.getNumChannels());
        for (int ch = 0; ch < chans; ++ch)
            hostAudio.addFrom (ch, 0, n->outputAudio, ch, 0, numSamples);

        // Merge MIDI into host MIDI
        for (auto meta : n->outputMidi)
            hostMidi.addEvent (meta.getMessage(), meta.samplePosition);
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

