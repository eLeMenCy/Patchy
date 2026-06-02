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
    bridge.getSpectrumSnapshots      = [&p]() { return p.getSpectrumSnapshots(); };
    bridge.onGetAddonAudioOutCount   = [&p](const juce::String& nid) { return p.getAddonAudioOutCount (nid); };
    bridge.onPruneAddonEdges         = [&p](const juce::String& nid) { p.pruneProcessingGraphEdges (nid); };
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

bool PatchyEditor::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    const bool cmd   = key.getModifiers().isCommandDown();
    const bool shift = key.getModifiers().isShiftDown();

    // Escape: forward to WebView so React ghost mode can cancel
    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        bridge.pushToUI ("onKeyEvent", "\"Escape\"");
        return false;  // don't consume — let WebView handle it too
    }

    if (! cmd) return false;

    if      (key.getKeyCode() == 'N')              { bridge.handleFileNew();    return true; }
    else if (key.getKeyCode() == 'O')              { bridge.handleFileOpen();   return true; }
    else if (key.getKeyCode() == 'S' && ! shift)   { bridge.handleFileSave();   return true; }
    else if (key.getKeyCode() == 'S' &&   shift)   { bridge.handleFileSaveAs(); return true; }

    return false;
}

void PatchyEditor::parentHierarchyChanged()
{
    // Register as key listener on the top-level window so we intercept
    // shortcuts even when WebBrowserComponent has keyboard focus
    if (auto* topLevel = getTopLevelComponent())
        topLevel->addKeyListener (this);
}
