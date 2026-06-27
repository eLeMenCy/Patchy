#include "DmxDeviceNodes.h"
#include "ProcessingGraph.h"

bool DmxDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findDmxInNode (nodeId))
    {
        in->configure (s.devicePath);
        return true;
    }
    if (auto* out = graph.findDmxOutNode (nodeId))
    {
        out->configure (s.devicePath, s.universe);
        return true;
    }
    return false;
}

void DmxDeviceManager::applyAllSettings (ProcessingGraph& graph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph);
}
