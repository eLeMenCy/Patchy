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

    // Auto-configure any nodes that have no stored settings yet (fresh drop)
    for (auto& node : graph.getNodes())
    {
        if (settings.find (node->id) != settings.end()) continue;  // already configured

        if (auto* in = graph.findArtNetInNode (node->id))
        {
            in->configure (0);   // default universe 0, start listening immediately
        }
        else if (auto* out = graph.findArtNetOutNode (node->id))
        {
            out->configure ("127.0.0.1", 0);   // default target + universe 0
        }
    }
}
