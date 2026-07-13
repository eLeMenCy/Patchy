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
               { p.setPaxParameter (nid, idx, val); },
               // New graph
               [&p]() { p.newGraph(); },
               // Load graph from JSON
               [&p](const juce::String& json) { p.loadGraphFromJson (json); })
{
    bridge.getSpectrumSnapshots      = [&p]() { return p.getSpectrumSnapshots(); };
    bridge.onGetPaxAudioOutCount   = [&p](const juce::String& nid) { return p.getPaxAudioOutCount (nid); };
    bridge.onPrunePaxEdges         = [&p](const juce::String& nid) { p.pruneProcessingGraphEdges (nid); };
    bridge.onSetAudioDeviceChannels  = [&p](const juce::String& nid, const std::vector<int>& ch)
                                       { p.setAudioDeviceChannels (nid, ch); };
    bridge.onSetUdpSettings          = [&p](const juce::String& nid, int port, int mode,
                                            const juce::String& targetHost, const juce::String& multicastAddr)
                                       { p.setUdpSettings (nid, port, mode, targetHost, multicastAddr); };
    bridge.onSetOscSettings          = [&p](const juce::String& nid, int port,
                                            const juce::String& targetHost, const juce::String& oscAddress)
                                       { p.setOscSettings (nid, port, targetHost, oscAddress); };
    bridge.onSetMqttSubscribeSettings = [&p](const juce::String& nid, const juce::String& host, int port,
                                             const juce::String& topic, int qos,
                                             const juce::String& username, const juce::String& password)
                                       { p.setMqttSubscribeSettings (nid, host, port, topic, qos, username, password); };
    bridge.onSetMqttPublishSettings   = [&p](const juce::String& nid, const juce::String& host, int port,
                                             const juce::String& topic, int qos, bool retain,
                                             const juce::String& username, const juce::String& password)
                                       { p.setMqttPublishSettings (nid, host, port, topic, qos, retain, username, password); };
    bridge.onSetArtNetSettings       = [&p](const juce::String& nid, int universe,
                                            const juce::String& targetHost)
                                       { p.setArtNetSettings (nid, universe, targetHost); };
    bridge.onSetDmxSettings          = [&p](const juce::String& nid, const juce::String& devicePath, int universe)
                                       { p.setDmxSettings (nid, devicePath, universe); };
    bridge.drainDmxSnapshots         = [&p]() { return p.drainAllDmxSnapshots(); };
    bridge.drainOscMonitor           = [&p]() { return p.drainAllOscMonitorEvents(); };
    bridge.drainUdpMonitor           = [&p]() { return p.drainAllUdpMonitorEvents(); };
    bridge.onSetDmxConsoleChannel    = [&p](const juce::String& nid, int channel, uint8_t value)
                                       {
                                           auto* node = p.getProcessingGraph().findDmxConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findDmxConsoleNode (nid);
                                           if (node) node->setChannel (channel, value);
                                           p.saveDmxConsoleChannels (nid);
                                       };
    bridge.onRestoreDmxConsoleChannels = [&p](const juce::String& nid, const juce::String& json)
                                       { p.restoreDmxConsoleChannels (nid, json); };
    bridge.onResetDmxConsoleChannels   = [&p](const juce::String& nid)
                                       {
                                           auto* node = p.getProcessingGraph().findDmxConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findDmxConsoleNode (nid);
                                           if (node) node->resetChannels();
                                       };
    bridge.onSetDmxBlackout          = [&p](const juce::String& nid, bool active)
                                       {
                                           auto* node = p.getProcessingGraph().findDmxConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findDmxConsoleNode (nid);
                                           if (node) node->setBlackout (active);
                                       };
    bridge.onRestoreDmxBlackout      = [&p](const juce::String& nid, bool active)
                                       {
                                           auto* node = p.getProcessingGraph().findDmxConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findDmxConsoleNode (nid);
                                           if (node) node->restoreBlackout (active);
                                       };

    // ── ArtNet Console callbacks (mirrors DMX Console pattern) ────────────────
    bridge.drainArtNetSnapshots        = [&p]() { return p.drainAllArtNetSnapshots(); };
    bridge.onSetArtNetConsoleChannel   = [&p](const juce::String& nid, int channel, uint8_t value)
                                       {
                                           auto* node = p.getProcessingGraph().findArtNetConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findArtNetConsoleNode (nid);
                                           if (node) node->setChannel (channel, value);
                                           p.saveArtNetConsoleChannels (nid);
                                       };
    bridge.onRestoreArtNetConsoleChannels = [&p](const juce::String& nid, const juce::String& json)
                                       { p.restoreArtNetConsoleChannels (nid, json); };
    bridge.onResetArtNetConsoleChannels   = [&p](const juce::String& nid)
                                       {
                                           auto* node = p.getProcessingGraph().findArtNetConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findArtNetConsoleNode (nid);
                                           if (node) node->resetChannels();
                                       };
    bridge.onSetArtNetBlackout         = [&p](const juce::String& nid, bool active)
                                       {
                                           auto* node = p.getProcessingGraph().findArtNetConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findArtNetConsoleNode (nid);
                                           if (node) node->setBlackout (active);
                                       };
    bridge.onRestoreArtNetBlackout     = [&p](const juce::String& nid, bool active)
                                       {
                                           auto* node = p.getProcessingGraph().findArtNetConsoleNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findArtNetConsoleNode (nid);
                                           if (node) node->restoreBlackout (active);
                                       };
    bridge.onSetArtNetUniverseFilter   = [&p](const juce::String& nid, int filter)
                                       {
                                           auto* node = p.getProcessingGraph().findArtNetMonitorNode (nid);
                                           if (!node && p.getPendingGraph())
                                               node = p.getPendingGraph()->findArtNetMonitorNode (nid);
                                           if (node) node->setUniverseFilter (filter);
                                       };
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
    else if (key.getKeyCode() == 'Z' && ! shift)   { bridge.handleUndo(); return true; }
    else if (key.getKeyCode() == 'Z' &&   shift)   { bridge.handleRedo(); return true; }

    return false;
}

void PatchyEditor::parentHierarchyChanged()
{
    // Register as key listener on the top-level window so we intercept
    // shortcuts even when WebBrowserComponent has keyboard focus
    if (auto* topLevel = getTopLevelComponent())
        topLevel->addKeyListener (this);
}
