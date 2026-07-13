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

void MqttDeviceManager::applyAllSettings (ProcessingGraph& graph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph);
}
