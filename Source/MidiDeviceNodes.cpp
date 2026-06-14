#include "MidiDeviceNodes.h"
#include "ProcessingGraph.h"
#include "../Pax/PaxRegistry.h"

// ─────────────────────────────────────────────────────────────────────────────
//  MidiDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

void MidiDeviceManager::applyDeviceSelections (ProcessingGraph& graph)
{
    for (const auto& [nodeId, deviceId] : selections)
        applyToGraph (nodeId, deviceId, graph);
}

bool MidiDeviceManager::applyToGraph (const juce::String& nodeId,
                                       const juce::String& deviceIdentifier,
                                       ProcessingGraph&    graph)
{
    if (auto* n = graph.findMidiOutNode (nodeId))
    {
        if (deviceIdentifier.isEmpty()) n->closeDevice();
        else                            n->openDevice (deviceIdentifier);
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

juce::var MidiDeviceManager::getAvailableDevicesVar()
{
    juce::Array<juce::var> outArr;
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id",   d.identifier);
        obj->setProperty ("name", d.name);
        outArr.add (obj);
    }

    juce::Array<juce::var> inArr;
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
