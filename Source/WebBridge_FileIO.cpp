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
         << "\"hasFile\":" << (currentFile.existsAsFile() ? "true" : "false") << ","
         << "\"unsaved\":" << (unsaved ? "true" : "false")   // v0.0.968 — the • after the name
         << "}";
    return json;
}

void WebBridge::saveToFile (const juce::File& file)
{
    auto json = juce::JSON::toString (graph.toVar(), true);
    if (file.replaceWithText (json))
    {
        currentFile = file;
        unsaved = false;   // v0.0.968 (pushed just below)
        pushFileState();
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

    // v0.0.967 — plugin: the DAW project holds the graph, so opening a
    // .patchy only replaces it — one undo step; no "current file", and the
    // Standalone's reopen-at-launch file is left alone (the recent list is shared)
    if (! isStandalone)
    {
        auto preSnapshot = graph.toVar();
        lastOpenDir = file.getParentDirectory();
        if (onLoadGraph) onLoadGraph (json);
        warnMissingPaxInGraph();
        graph.pushExistingSnapshot (std::move (preSnapshot));
        pushUndoState();
        AppSettings::addRecentFile (file);
        pushRecentFiles();
        diagLog ("Plugin: opened " + file.getFileName() + " (replaces the graph; undoable)");
        return true;
    }
    currentFile = file;
    lastOpenDir = file.getParentDirectory();
    if (onLoadGraph) onLoadGraph (json);
    warnMissingPaxInGraph();   // v0.0.941 — ghosts
    startClean();              // v0.0.968 — a freshly opened patch is clean (pushes the file state)
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

// v0.0.948 — Panic: { latched, blackout, mute } (options from Preferences)
void WebBridge::handlePanic()
{
    pushPanicState (onPanic ? onPanic() : false);
}

void WebBridge::pushPanicState (bool latched)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("latched",  latched);
    o->setProperty ("blackout", AppSettings::getPanicBlackout());
    o->setProperty ("mute",     AppSettings::getPanicMute());
    pushToUI ("onPanicState", juce::JSON::toString (juce::var (o), true));
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

void WebBridge::showSaveDialog (std::function<void()> onSaved)
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
        [this, chooser, onSaved] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{})
            {
                auto f = result.withFileExtension ("patchy");
                lastOpenDir = f.getParentDirectory();
                saveToFile (f);
                if (onSaved && ! unsaved) onSaved();   // v0.0.968 — only once it really saved
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
    // v0.0.967 — plugin: "Clear graph" — one undo step; v0.0.968: asks (native dialog)
    if (! isStandalone)
    {
        guardUnsaved ([this]
        {
            auto preSnapshot = graph.toVar();
            if (onNewGraph) onNewGraph();
            graph.pushExistingSnapshot (std::move (preSnapshot));
            pushUndoState();
        }, true);
        return;
    }
    guardUnsaved ([this]   // v0.0.968
    {
        currentFile = juce::File();
        AppSettings::setLastSessionFile ({});   // v0.0.923 — quit on a new graph = blank start next time
        if (onNewGraph) onNewGraph();
        startClean();
    });
}

void WebBridge::handleFileOpen()   { guardUnsaved ([this] { showOpenDialog(); }); }   // v0.0.968 — asks first if edited

// ── v0.0.968 — unsaved changes (Standalone) ──────────────────────────────────
void WebBridge::setUnsaved (bool u)
{
    if (unsaved == u) return;
    unsaved = u;
    pushFileState();
}

void WebBridge::pushFileState()
{
    pushToUI ("onFileState", buildFileStateJson());
    if (onFileStateChanged) onFileStateChanged (currentFileName(), currentFile.existsAsFile(), unsaved);
}

void WebBridge::exportGraphThen (std::function<void()> next)
{
    auto start = lastOpenDir.isDirectory() ? lastOpenDir : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    auto chooser = std::make_shared<juce::FileChooser> ("Export the current graph", start.getChildFile ("Patchy graph.patchy"), "*.patchy");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, chooser, next] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result == juce::File{}) return;   // cancelled: nothing replaced
            auto f = result.withFileExtension ("patchy");
            lastOpenDir = f.getParentDirectory();
            if (f.replaceWithText (juce::JSON::toString (graph.toVar(), true)))
            {
                diagLog ("Plugin: exported the graph to " + f.getFileName() + " before replacing it");
                next();
            }
        });
}

void WebBridge::startClean()
{
    unsaved = false;
    trackEditsFrom = juce::Time::getMillisecondCounter() + 1500;   // the load / rebuild settling isn't an edit
    pushFileState();
}

void WebBridge::guardUnsaved (std::function<void()> proceed, bool clearing)
{
    // v0.0.968 — plugin: no "unsaved" (the DAW saves the song); ask whenever
    // there is a graph to lose (⌘Z would still bring it back)
    if (! isStandalone)
    {
        if (graph.getNodes().empty()) { proceed(); return; }
        if (guardOpen) return;
        guardOpen = true;
        auto opts = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::QuestionIcon)
                        .withAssociatedComponent (this);
        if (clearing)
            opts = opts.withTitle ("Clear the whole graph?")
                       .withMessage (juce::String::fromUTF8 ("Every node and wire goes. Undo (⌘Z) brings them back."))   // UTF-8 literals: fromUTF8, or they come out garbled ("Export firstâ€¦")
                       .withButton ("Clear").withButton ("Cancel");                         // 0 / 1
        else
            opts = opts.withTitle ("Replace the current graph?")
                       .withMessage (juce::String::fromUTF8 ("The patch you open replaces this graph. Undo (⌘Z) brings it back, or export a copy first."))
                       .withButton (juce::String::fromUTF8 ("Export first…")).withButton ("Replace").withButton ("Cancel");   // 0 / 1 / 2
        juce::NativeMessageBox::showAsync (opts,
            [this, proceed, clearing, sp = juce::Component::SafePointer<WebBridge> (this)] (int button)
            {
                if (sp == nullptr) return;
                guardOpen = false;
                if (clearing) { if (button == 0) proceed(); return; }
                if (button == 1) proceed();
                else if (button == 0) exportGraphThen (proceed);
            });
        return;
    }

    if (! unsaved) { proceed(); return; }
    if (guardOpen) return;   // already asking
    guardOpen = true;

    const auto name = currentFile.existsAsFile() ? currentFile.getFileNameWithoutExtension() : juce::String ("Untitled");
    auto opts = juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle ("Save changes to \"" + name + "\"?")
                    .withMessage ("Your changes will be lost if you don't save them.")
                    .withButton ("Save")          // 0
                    .withButton ("Don't Save")    // 1
                    .withButton ("Cancel")        // 2
                    .withAssociatedComponent (this);

    juce::NativeMessageBox::showAsync (opts,
        [this, proceed, sp = juce::Component::SafePointer<WebBridge> (this)] (int button)
        {
            if (sp == nullptr) return;
            guardOpen = false;
            if (button == 1) { proceed(); return; }      // Don't Save
            if (button != 0) return;                     // Cancel (or closed)
            // Save: to the current file, or ask for a name first; go on only once saved
            if (currentFile.existsAsFile())
            {
                saveToFile (currentFile);
                if (! unsaved) proceed();
            }
            else
                showSaveDialog (proceed);
        });
}

void WebBridge::handleQuit()
{
    if (! isStandalone) return;   // a plugin can't quit its host
    // the same path as the window's close button (saves its bounds first)
    if (auto* w = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
        juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<juce::DocumentWindow> (w)]
                                         { if (sp != nullptr) sp->closeButtonPressed(); });
    else if (auto* app = juce::JUCEApplicationBase::getInstance())
        app->systemRequestedQuit();
}
// v0.0.967 — ⌘S / ⌘⇧S beeped in the Standalone (the keystroke never reached
// keyPressed); the page now sends them too. Both copies of one keystroke can
// arrive: the second within 400 ms is dropped (two save dialogs otherwise).
static bool saveKeyBounce()
{
    static juce::uint32 last = 0;
    const auto now = juce::Time::getMillisecondCounter();
    const bool bounce = now - last < 400;
    last = now;
    return bounce;
}
void WebBridge::handleFileSave()   { if (saveKeyBounce()) return; if (currentFile.existsAsFile()) saveToFile (currentFile); else showSaveDialog(); }
void WebBridge::handleFileSaveAs() { if (saveKeyBounce()) return; showSaveDialog(); }

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

void WebBridge::handleMidiFilePlayerLoadFile (const juce::DynamicObject* obj)
{
    juce::String nodeId = obj->getProperty ("nodeId").toString();
    if (nodeId.isEmpty()) return;

    auto chooser = std::make_shared<juce::FileChooser> (
        "Load MIDI File",
        lastOpenDir.isDirectory() ? lastOpenDir
                                   : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.mid;*.midi;*.smf");

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles,
        [this, chooser, nodeId] (const juce::FileChooser& fc)
        {
            auto result = fc.getResult();
            if (result != juce::File{} && result.existsAsFile())
            {
                lastOpenDir = result.getParentDirectory();
                if (onMidiFilePlayerLoadFile)
                    onMidiFilePlayerLoadFile (nodeId, result.getFullPathName());
            }
        });
}
