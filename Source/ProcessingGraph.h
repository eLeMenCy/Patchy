#pragma once
#include "WebBridge.h"
#include <functional>
#include "NodeProcessor.h"
#include "MidiMonitorNode.h"   // for MidiMonitorEvent and MidiMonitorNode
#include "AudioMonitorNode.h"  // for AudioMonitorBuffer and AudioMonitorNode
#include "OscMonitorNode.h"    // for OscMonitorBuffer and OscMonitorNode
#include "UdpMonitorNode.h"    // for UdpMonitorBuffer and UdpMonitorNode
#include "MqttDeviceNodes.h"   // for MqttSubscribeNode and MqttDeviceManager
#include "MidiKeyboardNode.h"
#include "DmxConsoleNode.h"
#include "ArtNetConsoleNode.h"
#include "GraphModel.h"
#include "../Pax/PaxRegistry.h"
#include <memory>
#include <unordered_map>

// Forward declarations to break circular dependencies
class MidiOutDeviceNode;
class MidiInDeviceNode;
class AudioOutDeviceNode;
class AudioInDeviceNode;
class AudioMonitorNode;
class UdpInDeviceNode;
class UdpOutDeviceNode;
class OscInDeviceNode;
class OscOutDeviceNode;
class ArtNetInDeviceNode;
class ArtNetOutDeviceNode;
class DmxInDeviceNode;
class DmxOutDeviceNode;
class DmxMonitorNode;
class DmxConsoleNode;
class ArtNetMonitorNode;
class ArtNetConsoleNode;

class ProcessingGraph
{
public:
    ProcessingGraph()  = default;
    ProcessingGraph (ProcessingGraph&&) = default;
    ProcessingGraph& operator= (ProcessingGraph&&) = default;

    void rebuild (const GraphModel& model,
                 PaxRegistry* registry = nullptr,
                 std::function<MidiMonitorBuffer*(const juce::String&)>    getMidiBuffer          = nullptr,
                 std::function<AudioMonitorBuffer*(const juce::String&)>   getAudioBuffer         = nullptr,
                 std::function<MidiMonitorBuffer*(const juce::String&)>    getKeyboardBuffer      = nullptr,
                 std::function<DmxMonitorBuffer*(const juce::String&)>     getDmxMonitorBuffer    = nullptr,
                 std::function<DmxMonitorBuffer*(const juce::String&)>     getDmxConsoleBuffer    = nullptr,
                 std::function<ArtNetMonitorBuffer*(const juce::String&)>  getArtNetMonitorBuffer = nullptr,
                 std::function<ArtNetMonitorBuffer*(const juce::String&)>  getArtNetConsoleBuffer = nullptr,
                 std::function<OscMonitorBuffer*(const juce::String&)>     getOscMonitorBuffer    = nullptr,
                 std::function<UdpMonitorBuffer*(const juce::String&)>     getUdpMonitorBuffer    = nullptr,
                 std::function<MqttMonitorBuffer*(const juce::String&)>    getMqttMonitorBuffer   = nullptr);
    void prepare (double sampleRate, int maxBlockSize);
    void process (juce::AudioBuffer<float>& hostAudio, juce::MidiBuffer& hostMidi);

    bool isEmpty()    const { return nodes.empty(); }

    /** Remove edges involving ports that no longer exist on a node.
     *  Called after dynamic port count changes to keep edges in sync with GraphModel. */
    void pruneEdgesForNode (const juce::String& nodeId, const std::vector<juce::String>& validPortIds)
    {
        edges.erase (
            std::remove_if (edges.begin(), edges.end(),
                [&] (const Edge& e)
                {
                    if (e.srcNodeId == nodeId)
                        return std::find (validPortIds.begin(), validPortIds.end(), e.srcPortId) == validPortIds.end();
                    if (e.dstNodeId == nodeId)
                        return std::find (validPortIds.begin(), validPortIds.end(), e.dstPortId) == validPortIds.end();
                    return false;
                }),
            edges.end()
        );
        topologicalSort();
    }
    GraphModel* graphModel = nullptr;  // set by PatchyProcessor, used for dynamic port updates
    bool isStandaloneMode = false;  // set true only in standalone app
    int  getNodeCount() const { return (int) nodes.size(); }
    const std::vector<std::unique_ptr<NodeProcessor>>& getNodes() const { return nodes; }
    std::vector<std::unique_ptr<NodeProcessor>>&       getNodes()       { return nodes; }

    // Device node accessors
    MidiOutDeviceNode*  findMidiOutNode  (const juce::String& nodeId);


    // Drain all monitor nodes and return their events (message thread)

    MidiInDeviceNode*   findMidiInNode   (const juce::String& nodeId);
    AudioOutDeviceNode* findAudioOutNode (const juce::String& nodeId);
    AudioInDeviceNode*  findAudioInNode  (const juce::String& nodeId);
    UdpInDeviceNode*    findUdpInNode    (const juce::String& nodeId);
    UdpOutDeviceNode*   findUdpOutNode   (const juce::String& nodeId);
    OscInDeviceNode*    findOscInNode    (const juce::String& nodeId);
    MqttSubscribeNode*  findMqttSubscribeNode (const juce::String& nodeId);
    MqttPublishNode*    findMqttPublishNode   (const juce::String& nodeId);
    MqttMonitorNode*    findMqttMonitorNode   (const juce::String& nodeId);
    MqttConsoleNode*    findMqttConsoleNode   (const juce::String& nodeId);
    OscOutDeviceNode*   findOscOutNode   (const juce::String& nodeId);
    ArtNetInDeviceNode*  findArtNetInNode  (const juce::String& nodeId);
    ArtNetOutDeviceNode* findArtNetOutNode (const juce::String& nodeId);
    DmxInDeviceNode*     findDmxInNode      (const juce::String& nodeId);
    DmxOutDeviceNode*    findDmxOutNode     (const juce::String& nodeId);
    DmxConsoleNode*      findDmxConsoleNode    (const juce::String& nodeId);
    ArtNetMonitorNode*   findArtNetMonitorNode (const juce::String& nodeId);
    ArtNetConsoleNode*   findArtNetConsoleNode (const juce::String& nodeId);

    // ── Monitor ───────────────────────────────────────────────────────────
    void closeAllAudioDevices();
    void closeAllTransferredAudioDevices();


    /** Transfer open audio device callbacks from an existing graph to this one.
     *  For nodes that exist in both graphs with the same device selected,
     *  moves the callback registration instead of closing and reopening —
     *  eliminating the audio gap on graph rebuilds. */
    void transferAudioDevicesFrom (ProcessingGraph& source);

    /** Closes every protocol device node's live socket/serial port (UDP, OSC,
     *  ArtNet, DMX In/Out) synchronously on the message thread. Called on the
     *  CURRENT graph right before a rebuild binds a new graph's sockets —
     *  without this, the new graph's bind can race against this graph's still-
     *  open socket on the same port and silently lose, permanently, until the
     *  node is reconfigured or the app restarts. See Architecture.md Phase 3
     *  known-issue note. */
    void closeAllProtocolDeviceSockets();

private:
    struct Edge
    {
        juce::String srcNodeId, srcPortId;
        juce::String dstNodeId, dstPortId;
    };

    std::vector<std::unique_ptr<NodeProcessor>> nodes;
    std::vector<Edge>                           edges;
    std::vector<NodeProcessor*>                 sortedNodes;
    std::unordered_map<juce::String, NodeProcessor*> nodeMap;
    std::unordered_map<juce::String, juce::String>       labelMap;  // nodeId → display label

    // portId → raw declaration index among that node's value-ish output
    // (or input) ports, in creation order — matches the index a Pax's
    // PAX_getValueOutputType(portIndex)/PAX_Value::portIndex are keyed on.
    // Not simply parseable from the port's label text the way audio's
    // per-port index is: label numbering counts "same-type occurrences"
    // for UI clarity (e.g. "MQTT Out 2" = the 2nd MQTT port), which only
    // happens to equal the raw declaration index for audio because every
    // audio port shares one type — for value ports, which can mix types,
    // those two numbers genuinely diverge (e.g. DMX, DMX, MQTT — the MQTT
    // port's "1st MQTT" label number is 1, but its raw index is 2). Built
    // fresh each rebuild() from the node snapshot's own port array order,
    // which does preserve true creation/declaration order.
    std::unordered_map<juce::String, int>                 valuePortIndexMap;

    double preparedSampleRate = 44100.0;
    int    preparedBlockSize  = 512;
    bool   isPrepared         = false;

    void topologicalSort();

    static bool isAudioPort (const juce::String& portId)
    {
        return portId.containsIgnoreCase ("Audio");
    }
};
