#include "ArtNetDeviceNodes.h"
#include "ProcessingGraph.h"

bool ArtNetDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findArtNetInNode (nodeId))
    {
        in->configure (s.universe);
        return true;
    }
    if (auto* out = graph.findArtNetOutNode (nodeId))
    {
        out->configure (s.targetHost, s.universe);
        return true;
    }
    return false;
}

void ArtNetDeviceManager::applyAllSettings (ProcessingGraph& graph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph);
}
