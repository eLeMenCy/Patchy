#include "MidiDeviceNodes.h"
#include "ProcessingGraph.h"
#include "../Pax/PaxRegistry.h"

// ─────────────────────────────────────────────────────────────────────────────
//  MidiDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

void MidiDeviceManager::applyDeviceSelections (ProcessingGraph& graph, ProcessingGraph* oldGraph)
{
    for (const auto& [nodeId, deviceId] : selections)
        applyToGraph (nodeId, deviceId, graph, oldGraph);
}

bool MidiDeviceManager::applyToGraph (const juce::String& nodeId,
                                       const juce::String& deviceIdentifier,
                                       ProcessingGraph&    graph,
                                       ProcessingGraph*    oldGraph)
{
    if (auto* n = graph.findMidiOutNode (nodeId))
    {
        if (deviceIdentifier.isEmpty())
        {
            n->closeDevice();
        }
        else
        {
            // Real fix, 2026-09-02 — see MidiOutDeviceNode's own
            // transferOrConfigure() comment for the full story.
            auto* oldNode = oldGraph != nullptr ? oldGraph->findMidiOutNode (nodeId) : nullptr;
            n->transferOrConfigure (deviceIdentifier, oldNode);
        }
        return true;
    }
    if (auto* n = graph.findMidiInNode (nodeId))
    {
        if (deviceIdentifier.isEmpty()) n->closeDevice();
        else                            n->openDevice (deviceIdentifier);
        return true;
    }
    return false;
}

juce::var MidiDeviceManager::getAvailableDevicesVar (bool isStandalone)
{
    juce::Array<juce::var> outArr;
    juce::Array<juce::var> inArr;

    // v0.0.919 (2026-10-02) — plugin builds: a virtual "DAW" device at the
    // top of both lists, like the audio device lists (AudioDeviceManager::
    // getAvailableDevicesVar()). MIDI In "DAW" = the host's MIDI into the
    // graph; MIDI Out "DAW" = back to the host.
    if (! isStandalone)
    {
        auto makeDaw = []() -> juce::var {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id",   "DAW");
            obj->setProperty ("name", "DAW");
            return obj;
        };
        outArr.add (makeDaw());
        inArr.add  (makeDaw());
    }

    for (const auto& d : juce::MidiOutput::getAvailableDevices())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id",   d.identifier);
        obj->setProperty ("name", d.name);
        outArr.add (obj);
    }

    for (const auto& d : juce::MidiInput::getAvailableDevices())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id",   d.identifier);
        obj->setProperty ("name", d.name);
        inArr.add (obj);
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("midiOutDevices", outArr);
    root->setProperty ("midiInDevices",  inArr);
    return root;
}
