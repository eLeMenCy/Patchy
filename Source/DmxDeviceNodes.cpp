#include "DmxDeviceNodes.h"
#include "ProcessingGraph.h"

bool DmxDeviceManager::applyToGraph (const juce::String& nodeId, ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    auto it = settings.find (nodeId);
    if (it == settings.end()) return false;
    const auto& s = it->second;

    if (auto* in = graph.findDmxInNode (nodeId))
    {
        // Real fix, 2026-09-01 — see DmxInDeviceNode's own
        // transferOrConfigure() for the full story. oldGraph is the most
        // recent previous graph (whichever node instance genuinely has
        // this exact device already open, if any) — reusing that
        // connection instead of closing and reopening it on every
        // single graph rebuild, anywhere, is what actually fixes the
        // real, graph-wide sluggishness this was built to address.
        auto* oldIn = oldGraph != nullptr ? oldGraph->findDmxInNode (nodeId) : nullptr;
        in->transferOrConfigure (s.devicePath, oldIn);
        return true;
    }
    if (auto* out = graph.findDmxOutNode (nodeId))
    {
        auto* oldOut = oldGraph != nullptr ? oldGraph->findDmxOutNode (nodeId) : nullptr;
        out->transferOrConfigure (s.devicePath, s.universe, oldOut);
        return true;
    }
    return false;
}

void DmxDeviceManager::applyAllSettings (ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    for (const auto& [nodeId, s] : settings)
        applyToGraph (nodeId, graph, oldGraph);
}
