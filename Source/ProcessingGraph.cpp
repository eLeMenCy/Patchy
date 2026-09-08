#include "ProcessingGraph.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "UdpDeviceNodes.h"
#include "OscDeviceNodes.h"
#include "ArtNetDeviceNodes.h"
#include "DmxDeviceNodes.h"
#include "DmxConsoleNode.h"
#include "ArtNetConsoleNode.h"
#include "MidiMonitorNode.h"
#include "AudioMonitorNode.h"
#include "OscMonitorNode.h"
#include "UdpMonitorNode.h"
#include "MqttDeviceNodes.h"
#include <unordered_set>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// rebuild — called on message thread after graph edit
// ─────────────────────────────────────────────────────────────────────────────

void ProcessingGraph::rebuild (const GraphModel& model, PaxRegistry* reg,
                               std::function<MidiMonitorBuffer*(const juce::String&)>    getMidiBuffer,
                               std::function<AudioMonitorBuffer*(const juce::String&)>   getAudioBuffer,
                               std::function<MidiMonitorBuffer*(const juce::String&)>    getKeyboardBuffer,
                               std::function<DmxMonitorBuffer*(const juce::String&)>     getDmxMonitorBuffer,
                               std::function<DmxMonitorBuffer*(const juce::String&)>     getDmxConsoleBuffer,
                               std::function<ArtNetMonitorBuffer*(const juce::String&)>  getArtNetMonitorBuffer,
                               std::function<ArtNetMonitorBuffer*(const juce::String&)>  getArtNetConsoleBuffer,
                               std::function<OscMonitorBuffer*(const juce::String&)>     getOscMonitorBuffer,
                               std::function<UdpMonitorBuffer*(const juce::String&)>     getUdpMonitorBuffer,
                               std::function<MqttMonitorBuffer*(const juce::String&)>    getMqttMonitorBuffer,
                               std::function<AudioPlayerState*(const juce::String&)>     getAudioPlayerState)
{
    auto snapshot = model.toVar();
    auto* root    = snapshot.getDynamicObject();
    if (root == nullptr) return;

    nodes.clear();
    edges.clear();
    nodeMap.clear();
    labelMap.clear();
    valuePortIndexMap.clear();
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
            // Dynamic Pax node — ONLY use registry, never fall through to built-ins.
            // Pax nodeType (1=MIDI, 2=Audio, 3=AV) is separate from
            // built-in nodeType (1=MidiInDevice … 4=AudioOutDevice).
            if (registry != nullptr)
                proc = registry->createNode (id, paxName);

            if (proc == nullptr)
                juce::Logger::writeToLog ("ProcessingGraph: Pax not found: " + paxName);
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
                case 8:  proc = std::make_unique<UdpInDeviceNode>   (id); break;
                case 9:  proc = std::make_unique<UdpOutDeviceNode>  (id); break;
                case 10: proc = std::make_unique<OscInDeviceNode>    (id); break;
                case 11: proc = std::make_unique<OscOutDeviceNode>   (id); break;
                case 12: proc = std::make_unique<ArtNetInDeviceNode>  (id); break;
                case 13: proc = std::make_unique<ArtNetOutDeviceNode> (id); break;
                case 14: proc = std::make_unique<DmxInDeviceNode>     (id); break;
                case 15: proc = std::make_unique<DmxOutDeviceNode>    (id); break;
                case 16: proc = std::make_unique<DmxMonitorNode>      (id, getDmxMonitorBuffer  ? getDmxMonitorBuffer(id)  : nullptr); break;
                case 17: proc = std::make_unique<DmxConsoleNode>      (id, getDmxConsoleBuffer  ? getDmxConsoleBuffer(id)  : nullptr); break;
                case 18: proc = std::make_unique<ArtNetMonitorNode>   (id, getArtNetMonitorBuffer ? getArtNetMonitorBuffer(id) : nullptr); break;
                case 19: proc = std::make_unique<ArtNetConsoleNode>   (id, getArtNetConsoleBuffer ? getArtNetConsoleBuffer(id) : nullptr); break;
                case 20: proc = std::make_unique<OscMonitorNode>      (id, getOscMonitorBuffer  ? getOscMonitorBuffer(id)  : nullptr); break;
                case 21: proc = std::make_unique<UdpMonitorNode>      (id, getUdpMonitorBuffer  ? getUdpMonitorBuffer(id)  : nullptr); break;
                case 22: proc = std::make_unique<MqttSubscribeNode>   (id); break;
                case 23: proc = std::make_unique<MqttPublishNode>     (id); break;
                case 24: proc = std::make_unique<MqttMonitorNode>     (id, getMqttMonitorBuffer ? getMqttMonitorBuffer(id) : nullptr); break;
                case 25: proc = std::make_unique<MqttConsoleNode>     (id); break;
                case 26: proc = std::make_unique<AudioPlayerNode>     (id, getAudioPlayerState ? getAudioPlayerState(id) : nullptr); break;
                default:
                    juce::Logger::writeToLog ("ProcessingGraph: unknown built-in type " + juce::String (type));
                    break;
            }
        }

        if (proc != nullptr)
        {
            // Restore Pax parameters from settingsJson so a graph rebuild
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

            // Build the raw declaration index for this node's value-ish
            // ports — separate running counters for input vs output side,
            // in port-array order (which preserves true creation order).
            // "Value-ish" here matches toVar()'s counting definition
            // (value/osc/dmx/mqtt/udp — deliberately excludes midi, same
            // narrow documented limitation as elsewhere: a MIDI-typed
            // value port is indistinguishable from a legacy MIDI port at
            // this level, so it's routed via the legacy blind-copy path
            // instead — harmless, no existing Pax combines both).
            if (auto* portsArr = nd->getProperty ("ports").getArray())
            {
                int valueInIdx = 0, valueOutIdx = 0;
                for (auto& pv : *portsArr)
                {
                    auto* po = pv.getDynamicObject();
                    if (! po) continue;
                    juce::String pType = po->getProperty ("type").toString();
                    bool isValueLike = (pType == "value" || pType == "osc" || pType == "dmx" ||
                                        pType == "mqtt"  || pType == "udp");
                    if (! isValueLike) continue;
                    juce::String pId  = po->getProperty ("id").toString();
                    bool isOut = po->getProperty ("direction").toString() == "output";
                    valuePortIndexMap[pId] = isOut ? valueOutIdx++ : valueInIdx++;
                }
            }

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

    // Build connectivity sets (used in steps 2 and 4) — cleared, not
    // reconstructed, so the underlying bucket capacity from a previous
    // block is reused rather than requiring a fresh heap allocation every
    // single block (see these members' own declaration in
    // ProcessingGraph.h for the full story).
    hasInputScratch.clear();
    for (auto& e : edges)
        hasInputScratch.insert (e.dstNodeId);

    hasOutputScratch.clear();
    for (auto& e : edges)
        hasOutputScratch.insert (e.srcNodeId);

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
        // Pax/processing nodes with no connections stay silent
        if (hasInputScratch.count (n->id) == 0)
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
            // Pax/other nodes with no incoming connections stay silent
        }
    }

    // ── 3. Process each node in topological order ─────────────────────────
    for (auto* n : sortedNodes)
    {
        // Every source id actually wired into n this block, across all
        // port types — used after the edge loop below to prune stale
        // entries out of n->dmxSourceFrames (a source that's since been
        // disconnected shouldn't keep contributing its last-known frame
        // forever). Harmless that this includes non-DMX/ArtNet sources
        // too — dmxSourceFrames/artNetSourceFrames only ever contain
        // entries their respective source types put there, so an
        // unrelated source's id here is simply never looked up against
        // either. Named generically since both prune passes share it.
        // Cleared, not reconstructed, every node every block — see this
        // member's own declaration in ProcessingGraph.h for the full story.
        currentUpstreamSourcesScratch.clear();

        // Route edges: copy upstream outputAudio → this node's inputAudio
        for (auto& e : edges)
        {
            if (e.dstNodeId != n->id) continue;

            auto srcIt = nodeMap.find (e.srcNodeId);
            if (srcIt == nodeMap.end()) continue;
            auto* src = srcIt->second;
            currentUpstreamSourcesScratch.insert (src->id);

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

                // ── Propagate value events, filtered by source port index ──
                // Was a blind copy of every value to every downstream node
                // regardless of which port-to-port connection the edge
                // represents — harmless for any node with a single value
                // output (the overwhelming majority, built-in and Pax
                // alike), but wrong the moment a node has more than one.
                // Mirrors audio's per-port routing above, except the index
                // comes from valuePortIndexMap (built fresh each rebuild()
                // from the node snapshot's port order) rather than parsed
                // from label text — label numbering counts "same-type
                // occurrences" for UI clarity, which only equals the raw
                // declaration index for audio because every audio port
                // shares one type; value ports can mix types, so those
                // two numbers genuinely diverge (see the map's own comment
                // in ProcessingGraph.h for a worked example).
                auto srcIdxIt   = valuePortIndexMap.find (e.srcPortId);
                int  srcPortIdx = (srcIdxIt != valuePortIndexMap.end()) ? srcIdxIt->second : 0;
                auto dstIdxIt   = valuePortIndexMap.find (e.dstPortId);
                int  dstPortIdx = (dstIdxIt != valuePortIndexMap.end()) ? dstIdxIt->second : 0;

                for (int vi = 0; vi < src->outputValueCount; ++vi)
                {
                    if (src->outputValues[static_cast<size_t>(vi)].portIndex != srcPortIdx)
                        continue;
                    if (n->inputValueCount >= NodeProcessor::kMaxValueEvents) break;

                    // Re-tag with the *destination's* own port index on the
                    // way in — the source's portIndex is meaningless to the
                    // receiving node; what matters to a future multi-input
                    // Pax is which of its own declared input ports this
                    // arrived through.
                    PAX_Value routed = src->outputValues[static_cast<size_t>(vi)];
                    routed.portIndex = static_cast<uint8_t> (dstPortIdx);
                    n->inputValues[static_cast<size_t>(n->inputValueCount++)] = routed;
                }

                // ── Cache the source's DMX frame, if it wrote one ──────────
                // Separate wide-payload path from the Value loop just above —
                // PAX_Value.data[] (56 bytes) can't carry a full 512-channel
                // universe (see PaxAPI.h v4 / NodeProcessor.h). No port-index
                // gating yet: every DMX-capable node today has exactly one
                // DMX port, so "did the source write a frame this block" is
                // an unambiguous enough test — revisit if a node ever grows
                // more than one.
                //
                // Cached per-source (dmxSourceFrames) rather than merged
                // directly into n->inputDmxFrame here — the actual HTP merge
                // across every known source happens once, after this whole
                // edge loop finishes, from the cache (see below). Merging
                // inline per-edge from only "sources valid this exact block"
                // was the original approach and it had a real bug: a source
                // that only emits on change (DmxConsoleNode) would vanish
                // from the merge the instant a continuously-emitting source
                // (AudioToDmxPax, audio-rate) re-initialised it on the very
                // next block — confirmed via DAW testing as a one-block
                // flash immediately overridden back to 0 on every fader move.
                if (src->outputDmxFrameValid)
                    n->dmxSourceFrames[src->id] = src->outputDmxFrame;

                // Same caching for ArtNet, plus the universe it's tagged
                // with — see NodeProcessor.h's ArtNetSourceFrame comment
                // for why merging later must respect that number.
                if (src->outputArtNetFrameValid)
                    n->artNetSourceFrames[src->id] = { src->outputArtNetUniverse, src->outputArtNetFrame };

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

                if (auto* oscMon = dynamic_cast<OscMonitorNode*> (n))
                {
                    juce::String srcLabel = labelMap.count (src->id) ? labelMap.at (src->id) : src->id;
                    if (auto* oscIn = dynamic_cast<OscInDeviceNode*> (src))
                        oscMon->pushFromSource (oscIn->lastRawMessages, srcLabel);
                    else if (src->outputValueCount > 0)
                        oscMon->pushFallbackValue (src->outputValues[0], srcLabel);
                }

                if (auto* udpMon = dynamic_cast<UdpMonitorNode*> (n))
                {
                    juce::String srcLabel = labelMap.count (src->id) ? labelMap.at (src->id) : src->id;
                    if (auto* udpIn = dynamic_cast<UdpInDeviceNode*> (src))
                        udpMon->pushFromSource (udpIn->lastRawPackets, srcLabel);
                    else if (src->outputValueCount > 0)
                        udpMon->pushFallbackValue (src->outputValues[0], srcLabel);
                }
            }
        }

        // ── DMX: prune stale sources, then HTP-merge every remaining one ──
        // Runs once per node, after all of n's incoming edges have been
        // processed above (not per-edge) — the merge needs every currently-
        // cached source's latest frame, not just whichever one happened to
        // be processed last.
        //
        // Prune first: erase any dmxSourceFrames entry whose source isn't
        // in currentUpstreamSourcesScratch (built from this block's actual edges,
        // above) — otherwise a disconnected source's last-known frame
        // would keep contributing to the merge forever.
        if (! n->dmxSourceFrames.empty())
        {
            for (auto it = n->dmxSourceFrames.begin(); it != n->dmxSourceFrames.end(); )
            {
                if (currentUpstreamSourcesScratch.count (it->first) == 0)
                    it = n->dmxSourceFrames.erase (it);
                else
                    ++it;
            }
        }

        // Merge HTP-style (Highest Takes Precedence — the standard
        // convention real DMX consoles/mergers use for combining multiple
        // sources on one universe): byte-wise max across every source
        // currently feeding n. Since a single-channel Pax zeroes every byte
        // it doesn't own each block (PaxRegistry.cpp's
        // outputDmxFrame.fill(0) before each PAX_process call), a channel
        // only one source touches passes straight through untouched, and
        // only a channel genuinely fought over by two sources at once
        // resolves via HTP rather than an arbitrary last-writer-wins.
        if (! n->dmxSourceFrames.empty())
        {
            n->inputDmxFrame.fill (0);
            for (auto& kv : n->dmxSourceFrames)
                for (size_t b = 0; b < n->inputDmxFrame.size(); ++b)
                    n->inputDmxFrame[b] = std::max (n->inputDmxFrame[b], kv.second[b]);
            n->inputDmxFrameValid = true;
        }
        else
        {
            // Real bug found and fixed 2026-08-30, discovered while
            // investigating a user report that a console's own port/edge
            // glow (and a monitor's own bargraph) stayed lit at their last
            // value forever after disconnection, never dimming back down.
            // The pruning above (see its own comment) correctly empties
            // dmxSourceFrames once a source disconnects — but this merge
            // itself was only ever entered when the cache was non-empty,
            // meaning inputDmxFrame/inputDmxFrameValid were never actually
            // reset once the LAST remaining source disconnected — both
            // simply stuck at whatever they held from the last time a
            // source really was connected. This isn't just a display
            // issue: inputDmxFrame is the same frame DmxOutDeviceNode
            // sends to real hardware, so a disconnected source could leave
            // a physical fixture holding a stale, frozen DMX value
            // indefinitely, not just a UI glow. Explicit reset here closes
            // that gap correctly.
            n->inputDmxFrame.fill (0);
            n->inputDmxFrameValid = false;
        }

        // ── ArtNet: same prune-then-merge, plus a universe gate ───────────
        // Same reasoning and structure as the DMX pass above — the only
        // real difference is that a merge must only ever combine cached
        // entries that share the SAME universe (see NodeProcessor.h's
        // ArtNetSourceFrame comment for why blending different universes'
        // bytes together would be meaningless). The chosen universe is
        // whichever the first cached entry happens to have — for a
        // correctly-wired rig every cached entry already shares one
        // universe anyway (matching how DMX itself expects one universe
        // per destination), so which one "picks" is moot; a mismatched
        // entry stays cached for later but is excluded from this block's
        // merge rather than blended in wrong.
        if (! n->artNetSourceFrames.empty())
        {
            for (auto it = n->artNetSourceFrames.begin(); it != n->artNetSourceFrames.end(); )
            {
                if (currentUpstreamSourcesScratch.count (it->first) == 0)
                    it = n->artNetSourceFrames.erase (it);
                else
                    ++it;
            }
        }

        if (! n->artNetSourceFrames.empty())
        {
            const int chosenUniverse = n->artNetSourceFrames.begin()->second.universe;
            n->inputArtNetFrame.fill (0);
            bool anyMerged = false;
            for (auto& kv : n->artNetSourceFrames)
            {
                if (kv.second.universe != chosenUniverse) continue;
                anyMerged = true;
                for (size_t b = 0; b < n->inputArtNetFrame.size(); ++b)
                    n->inputArtNetFrame[b] = std::max (n->inputArtNetFrame[b], kv.second.data[b]);
            }
            if (anyMerged)
            {
                n->inputArtNetFrameValid = true;
                n->inputArtNetUniverse   = chosenUniverse;
            }
            else
            {
                // Same real bug as DMX's own merge above (see that
                // block's own comment for the full story) — a genuinely
                // empty universe-matched set (either no sources cached
                // at all, or every cached source belongs to a different
                // universe than chosenUniverse) never explicitly reset
                // this node's own frame/valid state before, leaving it
                // stuck at its last real value indefinitely.
                n->inputArtNetFrame.fill (0);
                n->inputArtNetFrameValid = false;
            }
        }
        else
        {
            // Same fix, for the case where artNetSourceFrames itself is
            // already empty before this block even runs.
            n->inputArtNetFrame.fill (0);
            n->inputArtNetFrameValid = false;
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
                if (outNode->getIsDawDevice() && hasInputScratch.count (n->id) > 0)
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
        // Other sink nodes (monitors, Pax) are observers only — they must NOT
        // contribute to hostAudio. Audio only reaches the physical output via
        // an explicit AudioOutDeviceNode connection.
        // hostMidi sinks however are still collected.
        for (auto* n : sortedNodes)
        {
            if (hasOutputScratch.count (n->id) > 0) continue;
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

AudioPlayerNode* ProcessingGraph::findAudioPlayerNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<AudioPlayerNode*> (it->second);
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

UdpInDeviceNode* ProcessingGraph::findUdpInNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<UdpInDeviceNode*> (it->second);
}

UdpOutDeviceNode* ProcessingGraph::findUdpOutNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<UdpOutDeviceNode*> (it->second);
}

OscInDeviceNode* ProcessingGraph::findOscInNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<OscInDeviceNode*> (it->second);
}

MqttSubscribeNode* ProcessingGraph::findMqttSubscribeNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<MqttSubscribeNode*> (it->second);
}

MqttPublishNode* ProcessingGraph::findMqttPublishNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<MqttPublishNode*> (it->second);
}

MqttMonitorNode* ProcessingGraph::findMqttMonitorNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<MqttMonitorNode*> (it->second);
}

MqttConsoleNode* ProcessingGraph::findMqttConsoleNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<MqttConsoleNode*> (it->second);
}

OscOutDeviceNode* ProcessingGraph::findOscOutNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<OscOutDeviceNode*> (it->second);
}

ArtNetInDeviceNode* ProcessingGraph::findArtNetInNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<ArtNetInDeviceNode*> (it->second);
}

ArtNetOutDeviceNode* ProcessingGraph::findArtNetOutNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<ArtNetOutDeviceNode*> (it->second);
}

DmxInDeviceNode* ProcessingGraph::findDmxInNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<DmxInDeviceNode*> (it->second);
}

DmxOutDeviceNode* ProcessingGraph::findDmxOutNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<DmxOutDeviceNode*> (it->second);
}

DmxConsoleNode* ProcessingGraph::findDmxConsoleNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<DmxConsoleNode*> (it->second);
}

ArtNetConsoleNode* ProcessingGraph::findArtNetConsoleNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<ArtNetConsoleNode*> (it->second);
}

ArtNetMonitorNode* ProcessingGraph::findArtNetMonitorNode (const juce::String& nodeId)
{
    auto it = nodeMap.find (nodeId);
    if (it == nodeMap.end()) return nullptr;
    return dynamic_cast<ArtNetMonitorNode*> (it->second);
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

void ProcessingGraph::closeAllProtocolDeviceSockets()
{
    // Closes every protocol device node's live socket/serial port on the
    // message thread, synchronously, before a new graph rebinds the same
    // port(s). Fixes a bind-race: if this graph's socket is still open when
    // a freshly-built graph tries to bind the same port, the new bind can
    // silently fail and that node goes dead until reconfigured or restarted.
    // See Architecture.md Phase 3 known-issue note (found 2026-07-07).
    for (auto& node : nodes)
    {
        if (auto* n = dynamic_cast<UdpInDeviceNode*>     (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<UdpOutDeviceNode*>    (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<OscInDeviceNode*>     (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<OscOutDeviceNode*>    (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<ArtNetInDeviceNode*>  (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<ArtNetOutDeviceNode*> (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<DmxInDeviceNode*>     (node.get())) n->closePort();
        if (auto* n = dynamic_cast<DmxOutDeviceNode*>    (node.get())) n->closePort();
    }
}

void ProcessingGraph::closeUdpBasedProtocolDeviceSockets()
{
    // See this method's own declaration in ProcessingGraph.h for the full
    // story — deliberately excludes ArtNet and DMX, both of which now have
    // a genuine connection-transfer mechanism that this same close would
    // otherwise defeat entirely, every single graph rebuild.
    for (auto& node : nodes)
    {
        if (auto* n = dynamic_cast<UdpInDeviceNode*>  (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<UdpOutDeviceNode*> (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<OscInDeviceNode*>  (node.get())) n->closeSocket();
        if (auto* n = dynamic_cast<OscOutDeviceNode*> (node.get())) n->closeSocket();
    }
}

