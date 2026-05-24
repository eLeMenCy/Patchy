#include "PatchyProcessor.h"
#include "PatchyEditor.h"

PatchyEditor::PatchyEditor (PatchyProcessor& p)
    : AudioProcessorEditor (&p),
      proc (p),
      bridge (p.getGraphModel(), &p.getRegistry(),
               [&p](const juce::String& nodeId, const juce::String& devId)
               { p.setMidiDevice (nodeId, devId); },
               [&p](const juce::String& nodeId, const juce::String& devName)
               { p.setAudioDevice (nodeId, devName); },
               [&p]() { return p.drainAllMidiMonitorEvents(); },
               [&p]() { return p.getAudioSnapshots(); },
               [&p]() { p.clearGraphTrash(); },
               [&p]() { return p.getPortActivity(); },
               [&p](const juce::String& nid, uint8_t s, uint8_t d1, uint8_t d2)
               { p.pushMidiKeyEvent (nid, s, d1, d2); },
               [&p](const juce::String& nid, const juce::String& name)
               { p.setNodeCustomName (nid, name); },
               [&p](const juce::String& nid, int idx, float val)
               { p.setAddonParameter (nid, idx, val); },
               // New graph
               [&p]() { p.newGraph(); },
               // Load graph from JSON
               [&p](const juce::String& json) { p.loadGraphFromJson (json); })
{
    addAndMakeVisible (bridge);
    setSize (640, 400);
    setResizable (true, false);
    bridge.loadUI();
}

void PatchyEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0d0f14));
}

void PatchyEditor::resized()
{
    bridge.setBounds (getLocalBounds());
}
