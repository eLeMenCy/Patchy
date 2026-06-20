#include "OscDeviceNodes.h"
#include "ProcessingGraph.h"

bool OscDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findOscInNode (nodeId))
    {
        in->configure (s.port);
        return true;
    }
    if (auto* out = graph.findOscOutNode (nodeId))
    {
        out->configure (s.port, s.targetHost, s.oscAddress);
        return true;
    }
    return false;
}

void OscDeviceManager::applyAllSettings (ProcessingGraph& graph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph);
}
