#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"
#include "SerialPort.h"
#include "AppSettings.h"
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
        AppSettings::addRecentFile (file);   // v0.0.923
        AppSettings::setLastSessionFile (file.getFullPathName());
        pushRecentFiles();
    }
}

// ── v0.0.923 (2026-10-04) — recent files / reopen last project ──────────────
// One path for every way a patch gets opened (Open dialog, Open Recent,
// reopen at launch, Locate…), so they all update the recent list alike.
bool WebBridge::openFile (const juce::File& file)
{
    if (! file.existsAsFile()) return false;
    auto json = file.loadFileAsString();
    if (json.isEmpty()) return false;
    currentFile = file;
    lastOpenDir = file.getParentDirectory();
    if (onLoadGraph) onLoadGraph (json);
    pushToUI ("onFileState", buildFileStateJson());
    AppSettings::addRecentFile (file);
    AppSettings::setLastSessionFile (file.getFullPathName());
    pushRecentFiles();
    return true;
}

void WebBridge::pushRecentFiles()
{
    juce::Array<juce::var> arr;
    for (auto& path : AppSettings::getRecentFiles())
    {
        const juce::File f (path);
        auto* o = new juce::DynamicObject();
        o->setProperty ("path",   path);
        o->setProperty ("name",   f.getFileNameWithoutExtension());
        o->setProperty ("folder", f.getParentDirectory().getFileName());
        arr.add (juce::var (o));
    }
    pushToUI ("onRecentFiles", juce::JSON::toString (juce::var (arr), true));
}

void WebBridge::pushAppSettings()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("reopenLastProject", AppSettings::getReopenLastProject());   // -1 / 0 / 1
    pushToUI ("onAppSettings", juce::JSON::toString (juce::var (o), true));
}

void WebBridge::handleLaunchActions()
{
    // handleReady() fires more than once per page load (seen twice in logs),
    // and the page can reload: launch actions run once per process.
    static bool done = false;
    if (done || ! isStandalone) return;
    done = true;

    const int reopen = AppSettings::getReopenLastProject();
    if (reopen < 0)
    {
        pushToUI ("onAskReopenLastProject", "{}");   // first launch: ask once
        return;
    }
    if (reopen == 0) return;

    // The project open when Patchy was last closed — "" after File → New
    // (blank start), not simply the newest recent file.
    const auto lastPath = AppSettings::getLastSessionFile();
    if (lastPath.isEmpty()) return;
    if (! openFile (juce::File (lastPath)))
    {
        // Off the list and no longer the session file, or every launch would
        // ask about it again ("Start empty" = stays blank; Locate… sets both
        // again through openFile()).
        AppSettings::removeRecentFile (lastPath);
        AppSettings::setLastSessionFile ({});
        pushRecentFiles();
        auto* o = new juce::DynamicObject();
        o->setProperty ("path", lastPath);
        o->setProperty ("context", "launch");
        pushToUI ("onFileMissing", juce::JSON::toString (juce::var (o), true));
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

void WebBridge::showOpenDialog (const juce::File& startDirOverride)
{
    auto startDir = startDirOverride.isDirectory() ? startDirOverride   // v0.0.923 — Locate…
                  : currentFile.existsAsFile()
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
            if (result != juce::File{})
                openFile (result);   // v0.0.923 — shared path (recent list)
        });
}
// ── C++-callable file operations (e.g. from keyboard shortcuts) ──────────────
void WebBridge::handleFileNew()
{
    currentFile = juce::File();
    AppSettings::setLastSessionFile ({});   // v0.0.923 — quit on a new graph = blank start next time
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
