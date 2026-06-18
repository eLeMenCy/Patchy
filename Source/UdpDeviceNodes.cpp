#include "UdpDeviceNodes.h"
#include "ProcessingGraph.h"

bool UdpDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findUdpInNode (nodeId))
    {
        in->configure (s.port, s.mode, s.multicastAddr);
        return true;
    }
    if (auto* out = graph.findUdpOutNode (nodeId))
    {
        out->configure (s.port, s.mode, s.targetHost, s.multicastAddr);
        return true;
    }
    return false;
}

void UdpDeviceManager::applyAllSettings (ProcessingGraph& graph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph);
}
