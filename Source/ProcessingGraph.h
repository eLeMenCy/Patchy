#pragma once
#include "WebBridge.h"
#include <functional>
#include "NodeProcessor.h"
#include "MidiMonitorNode.h"   // for MidiMonitorEvent and MidiMonitorNode
#include "AudioMonitorNode.h"  // for AudioMonitorBuffer and AudioMonitorNode
#include "MidiKeyboardNode.h"
#include "GraphModel.h"
#include "../Addons/AddonRegistry.h"
#include <memory>
#include <unordered_map>

// Forward declarations to break circular dependencies
class MidiOutDeviceNode;
class MidiInDeviceNode;
class AudioOutDeviceNode;
class AudioInDeviceNode;
class AudioMonitorNode;

class ProcessingGraph
{
public:
    ProcessingGraph()  = default;
    ProcessingGraph (ProcessingGraph&&) = default;
    ProcessingGraph& operator= (ProcessingGraph&&) = default;

    void rebuild (const GraphModel& model,
                 AddonRegistry* registry = nullptr,
                 std::function<MidiMonitorBuffer*(const juce::String&)>  getMidiBuffer     = nullptr,
                 std::function<AudioMonitorBuffer*(const juce::String&)> getAudioBuffer    = nullptr,
                 std::function<MidiMonitorBuffer*(const juce::String&)>  getKeyboardBuffer = nullptr);
    void prepare (double sampleRate, int maxBlockSize);
    void process (juce::AudioBuffer<float>& hostAudio, juce::MidiBuffer& hostMidi);

    bool isEmpty()    const { return nodes.empty(); }
    int  getNodeCount() const { return (int) nodes.size(); }
    const std::vector<std::unique_ptr<NodeProcessor>>& getNodes() const { return nodes; }

    // Device node accessors
    MidiOutDeviceNode*  findMidiOutNode  (const juce::String& nodeId);


    // Drain all monitor nodes and return their events (message thread)

    MidiInDeviceNode*   findMidiInNode   (const juce::String& nodeId);
    AudioOutDeviceNode* findAudioOutNode (const juce::String& nodeId);
    AudioInDeviceNode*  findAudioInNode  (const juce::String& nodeId);

    // ── Monitor ───────────────────────────────────────────────────────────
    void closeAllAudioDevices();


    /** Transfer open audio device callbacks from an existing graph to this one.
     *  For nodes that exist in both graphs with the same device selected,
     *  moves the callback registration instead of closing and reopening —
     *  eliminating the audio gap on graph rebuilds. */
    void transferAudioDevicesFrom (ProcessingGraph& source);

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

    double preparedSampleRate = 44100.0;
    int    preparedBlockSize  = 512;
    bool   isPrepared         = false;

    void topologicalSort();

    static bool isAudioPort (const juce::String& portId)
    {
        return portId.containsIgnoreCase ("Audio");
    }
};
