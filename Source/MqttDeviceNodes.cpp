#include "MqttDeviceNodes.h"
#include "ProcessingGraph.h"

std::atomic<int> MosquittoLibraryRef::refCount { 0 };

bool MqttDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* sub = graph.findMqttSubscribeNode (nodeId))
    {
        sub->configure (s.host, s.port, s.topic, s.qos, s.username, s.password);
        return true;
    }
    if (auto* pub = graph.findMqttPublishNode (nodeId))
    {
        pub->configure (s.host, s.port, s.topic, s.qos, s.retain, s.username, s.password);
        return true;
    }
    return false;
}

void MqttDeviceManager::applyAllSettings (ProcessingGraph& graph, ProcessingGraph* previous)
{
    for (const auto& [nodeId, s] : settings)
    {
        // v0.0.919 — hand the live connection over before configure() runs
        // (see MqttSubscribeNode::takeConnectionFrom()).
        if (previous != nullptr && previous != &graph)
        {
            if (auto* sub = graph.findMqttSubscribeNode (nodeId))
                if (auto* oldSub = previous->findMqttSubscribeNode (nodeId))
                    sub->takeConnectionFrom (*oldSub);
            if (auto* pub = graph.findMqttPublishNode (nodeId))
                if (auto* oldPub = previous->findMqttPublishNode (nodeId))
                    pub->takeConnectionFrom (*oldPub);
        }
        applyToGraph (nodeId, graph);
    }
}
