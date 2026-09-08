#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"
#include "SerialPort.h"
#include <unordered_map>

#if HAS_BUNDLED_UI
  #include "BinaryData.h"
#endif

// ── File operations ───────────────────────────────────────────────────────────

juce::String WebBridge::buildFileStateJson()
{
    juce::String name = currentFile.existsAsFile()
                        ? currentFile.getFileNameWithoutExtension()
                        : "Untitled";
    juce::String json;
    json << "{"
         << "\"fileName\":\"" << name << "\","
         << "\"hasFile\":" << (currentFile.existsAsFile() ? "true" : "false")
         << "}";
    return json;
}

void WebBridge::saveToFile (const juce::File& file)
{
    auto json = juce::JSON::toString (graph.toVar(), true);
    if (file.replaceWithText (json))
    {
        currentFile = file;
        pushToUI ("onFileState", buildFileStateJson());
    }
}

void WebBridge::showSaveDialog()
{
    auto chooser = std::make_shared<juce::FileChooser> (
        "Save Patch", currentFile.existsAsFile()
            ? currentFile
            : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                  .getChildFile ("Untitled.patchy"),
        "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{})
            {
                auto f = result.withFileExtension ("patchy");
                lastOpenDir = f.getParentDirectory();
                saveToFile (f);
            }
        });
}

void WebBridge::showOpenDialog()
{
    auto startDir = currentFile.existsAsFile()
                    ? currentFile.getParentDirectory()
                    : (lastOpenDir.isDirectory()
                       ? lastOpenDir
                       : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    auto chooser = std::make_shared<juce::FileChooser> ("Open Patch", startDir, "*.patchy");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{} && result.existsAsFile())
            {
                auto json = result.loadFileAsString();
                if (json.isNotEmpty())
                {
                    currentFile = result;
                    lastOpenDir  = result.getParentDirectory();
                    if (onLoadGraph) onLoadGraph (json);
                    pushToUI ("onFileState", buildFileStateJson());

                }
            }
        });
}
// ── C++-callable file operations (e.g. from keyboard shortcuts) ──────────────
void WebBridge::handleFileNew()
{
    currentFile = juce::File();
    if (onNewGraph) onNewGraph();
    pushToUI ("onFileState", buildFileStateJson());
}

void WebBridge::handleFileOpen()   { showOpenDialog(); }
void WebBridge::handleFileSave()   { if (currentFile.existsAsFile()) saveToFile (currentFile); else showSaveDialog(); }
void WebBridge::handleFileSaveAs() { showSaveDialog(); }

void WebBridge::handleUndo()
{
    if (graph.undo())
    {
        for (const auto& n : graph.getNodes())
        {
            if (n.nodeType == 17)
            {
                if (n.settingsJson.isNotEmpty() && n.settingsJson.contains ("dmxChannels"))
                    pushSettingsToUI (n.id, n.settingsJson);
                else if (onResetDmxConsoleChannels)
                    onResetDmxConsoleChannels (n.id);
            }
            else if (n.nodeType == 19)
            {
                if (n.settingsJson.isNotEmpty() && n.settingsJson.contains ("artNetChannels"))
                    pushSettingsToUI (n.id, n.settingsJson);
                else if (onResetArtNetConsoleChannels)
                    onResetArtNetConsoleChannels (n.id);
            }
        }
        pushUndoState();
    }
}

void WebBridge::handleRedo()
{
    if (graph.redo())
    {
        for (const auto& n : graph.getNodes())
        {
            if (n.nodeType == 17)
            {
                if (n.settingsJson.isNotEmpty() && n.settingsJson.contains ("dmxChannels"))
                    pushSettingsToUI (n.id, n.settingsJson);
                else if (onResetDmxConsoleChannels)
                    onResetDmxConsoleChannels (n.id);
            }
            else if (n.nodeType == 19)
            {
                if (n.settingsJson.isNotEmpty() && n.settingsJson.contains ("artNetChannels"))
                    pushSettingsToUI (n.id, n.settingsJson);
                else if (onResetArtNetConsoleChannels)
                    onResetArtNetConsoleChannels (n.id);
            }
        }
        pushUndoState();
    }
}

void WebBridge::handleAudioPlayerLoadFile (const juce::DynamicObject* obj)
{
    juce::String nodeId = obj->getProperty ("nodeId").toString();
    if (nodeId.isEmpty()) return;

    // Same async FileChooser pattern as showOpenDialog()/showSaveDialog()
    // above — a shared_ptr keeps the chooser alive for the duration of
    // the native dialog, since launchAsync() returns immediately.
    auto chooser = std::make_shared<juce::FileChooser> (
        "Load Audio File",
        lastOpenDir.isDirectory() ? lastOpenDir
                                   : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser, nodeId] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{} && result.existsAsFile())
            {
                lastOpenDir = result.getParentDirectory();
                if (onAudioPlayerLoadFile)
                    onAudioPlayerLoadFile (nodeId, result.getFullPathName());
            }
        });
}

void WebBridge::handleAudioPlayerRequestFileInfo (const juce::DynamicObject* obj)
{
    juce::String nodeId = obj->getProperty ("nodeId").toString();
    if (nodeId.isEmpty()) return;
    if (onAudioPlayerRequestFileInfo) onAudioPlayerRequestFileInfo (nodeId);
}
