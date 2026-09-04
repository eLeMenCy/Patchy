#include "ArtNetDeviceNodes.h"
#include "ProcessingGraph.h"

bool ArtNetDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findArtNetInNode (nodeId))
    {
        auto* oldIn = oldGraph != nullptr ? oldGraph->findArtNetInNode (nodeId) : nullptr;
        in->transferOrConfigure (s.universe, oldIn);
        return true;
    }
    if (auto* out = graph.findArtNetOutNode (nodeId))
    {
        auto* oldOut = oldGraph != nullptr ? oldGraph->findArtNetOutNode (nodeId) : nullptr;
        out->transferOrConfigure (s.targetHost, s.universe, oldOut);
        return true;
    }
    return false;
}

void ArtNetDeviceManager::applyAllSettings (ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph, oldGraph);

    // Auto-configure any nodes that have no stored settings yet (fresh drop)
    // — real fix, 2026-09-01: this loop re-runs configure() on every single
    // rebuild for any node still at its own default (never explicitly
    // configured), which would otherwise keep reopening its socket forever
    // even for a node the user never touched at all. Now attempts the same
    // genuine transfer as the explicit-settings path above, rather than
    // unconditionally calling configure().
    for (auto& node : graph.getNodes())
    {
        if (settings.find (node->id) != settings.end()) continue;  // already configured

        if (auto* in = graph.findArtNetInNode (node->id))
        {
            auto* oldIn = oldGraph != nullptr ? oldGraph->findArtNetInNode (node->id) : nullptr;
            in->transferOrConfigure (0, oldIn);   // default universe 0, start listening immediately
        }
        else if (auto* out = graph.findArtNetOutNode (node->id))
        {
            auto* oldOut = oldGraph != nullptr ? oldGraph->findArtNetOutNode (node->id) : nullptr;
            out->transferOrConfigure ("127.0.0.1", 0, oldOut);   // default target + universe 0
        }
    }
}
