// Patchy — Custom Standalone Application

#if JUCE_STANDALONE_APPLICATION

#include "StandaloneApp.h"

// ─────────────────────────────────────────────────────────────────────────────
// StandaloneWindow
// ─────────────────────────────────────────────────────────────────────────────
StandaloneWindow::StandaloneWindow()
    : DocumentWindow (juce::String ("Patchy  v") + juce::String (JucePlugin_VersionString),
                      juce::Desktop::getInstance().getDefaultLookAndFeel()
                          .findColour (juce::ResizableWindow::backgroundColourId),
                      DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    setResizable (true, false);

    // Initialise audio device manager with default device
    auto err = deviceManager.initialiseWithDefaultDevices (32, 32);
    if (err.isNotEmpty())
        juce::Logger::writeToLog ("AudioDeviceManager init error: " + err);
    setupAudioDevice();

    // Create the processor and connect to audio device manager
    processor = std::make_unique<PatchyProcessor>();
    player.setProcessor (processor.get());
    deviceManager.addAudioCallback (&player);

    // Create editor via JUCE mechanism so getActiveEditor() works
    editor = dynamic_cast<PatchyEditor*> (processor->createEditorIfNeeded());

    // Wire bridge callbacks
    auto& bridge = editor->getBridge();
    bridge.isStandalone = true;
    processor->isStandalone = true;

    // Push standalone mode + audio settings when UI is ready
    bridge.onUIReady = [this, &bridge]()
    {
        bridge.pushToUI ("onStandaloneMode", "true");
        pushAudioSettingsToUI();
    };

    bridge.onSetAudioEngineSettings = [this](double sr, int buf, bool mute)
    {
        AudioSettings s;
        s.sampleRate   = sr;
        s.bufferSize   = buf;
        s.muteFeedback = mute;
        applyAudioSettings (s);
    };

    setContentNonOwned (editor, true);

    // Initialise settings storage
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "Patchy";
    opts.filenameSuffix      = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    appProperties.setStorageParameters (opts);

    setVisible (true);
    restoreWindowBounds();
    restoreAudioSettings();
    deviceManager.addChangeListener (this);   // v0.0.924 — after the restore (see changeListenerCallback)
}

StandaloneWindow::~StandaloneWindow()
{
    deviceManager.removeChangeListener (this);   // v0.0.924
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    clearContentComponent();
    if (editor)
    {
        processor->editorBeingDeleted (editor);
        delete editor;
        editor = nullptr;
    }
    processor.reset();
}

void StandaloneWindow::setupAudioDevice()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device)
    {
        audioSettings.sampleRate = device->getCurrentSampleRate();
        audioSettings.bufferSize = device->getCurrentBufferSizeSamples();
        juce::Logger::writeToLog ("Audio device: " + device->getName()
            + " SR=" + juce::String ((int) audioSettings.sampleRate)
            + " BS=" + juce::String (audioSettings.bufferSize));
    }
    else
    {
        juce::Logger::writeToLog ("No audio device available - using defaults");
    }
}

void StandaloneWindow::applyAudioSettings (const AudioSettings& s)
{
    audioSettings = s;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (!device) return;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    deviceManager.getAudioDeviceSetup (setup);
    setup.sampleRate  = s.sampleRate;
    setup.bufferSize  = s.bufferSize;
    deviceManager.setAudioDeviceSetup (setup, true);

    saveAudioSettings();
    pushAudioSettingsToUI();
}

juce::StringArray StandaloneWindow::getAvailableSampleRates() const
{
    juce::StringArray result;
    auto* device = deviceManager.getCurrentAudioDevice();
    if (!device) return result;

    for (auto sr : device->getAvailableSampleRates())
        result.add (juce::String ((int) sr));
    return result;
}

juce::StringArray StandaloneWindow::getAvailableBufferSizes() const
{
    juce::StringArray result;
    auto* device = deviceManager.getCurrentAudioDevice();
    if (!device) return result;

    for (auto bs : device->getAvailableBufferSizes())
        result.add (juce::String (bs));
    return result;
}

void StandaloneWindow::pushAudioSettingsToUI()
{
    if (editor == nullptr) return;
    auto& bridge = editor->getBridge();

    auto* device = deviceManager.getCurrentAudioDevice();

    // Use device rates if available, otherwise sensible defaults
    juce::String srList = "[";
    juce::String bsList = "[";

    if (device && device->getAvailableSampleRates().size() > 0)
    {
        bool first = true;
        for (auto sr : device->getAvailableSampleRates())
        {
            if (!first) srList += ",";
            srList += juce::String ((int) sr);
            first = false;
        }
        first = true;
        for (auto bs : device->getAvailableBufferSizes())
        {
            if (!first) bsList += ",";
            bsList += juce::String (bs);
            first = false;
        }
    }
    else
    {
        // Fallback defaults when no device is available
        srList += "44100,48000,88200,96000,176400,192000";
        bsList += "64,128,256,512,1024,2048";
    }
    srList += "]";
    bsList += "]";

    juce::String json;
    json << "{"
         << "\"sampleRate\":" << (int) audioSettings.sampleRate << ","
         << "\"bufferSize\":" << audioSettings.bufferSize << ","
         << "\"muteFeedback\":" << (audioSettings.muteFeedback ? "true" : "false") << ","
         << "\"availableSampleRates\":" << srList << ","
         << "\"availableBufferSizes\":" << bsList
         << "}";

    bridge.pushToUI ("onAudioSettings", json);
}

void StandaloneWindow::saveWindowBounds()
{
    if (auto* p = appProperties.getUserSettings())
    {
        p->setValue ("windowX",      getX());
        p->setValue ("windowY",      getY());
        p->setValue ("windowWidth",  getWidth());
        p->setValue ("windowHeight", getHeight());
        // Persist last open directory so file dialogs remember location
        auto& bridge = editor->getBridge();
        if (bridge.getLastOpenDir().isDirectory())
            p->setValue ("lastOpenDir", bridge.getLastOpenDir().getFullPathName());
        p->saveIfNeeded();
    }
}

void StandaloneWindow::restoreWindowBounds()
{
    if (auto* p = appProperties.getUserSettings())
    {
        int x = p->getIntValue ("windowX",      -1);
        int y = p->getIntValue ("windowY",      -1);
        int w = p->getIntValue ("windowWidth",  1200);
        int h = p->getIntValue ("windowHeight", 750);
        if (x >= 0 && y >= 0)
            setBounds (x, y, juce::jmax (500, w), juce::jmax (300, h));
        else
            centreWithSize (1200, 750);
        // Restore last open directory
        auto lastDir = juce::File (p->getValue ("lastOpenDir", ""));
        if (lastDir.isDirectory())
            editor->getBridge().setLastOpenDir (lastDir);
    }
    else
    {
        centreWithSize (1200, 750);
    }
}

void StandaloneWindow::closeButtonPressed()
{
    saveWindowBounds();
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

// User-requested feature, 2026-09-18 — buffer size remembered across app
// launches, same appProperties mechanism already used for window bounds
// and last-open-directory above. Saved immediately on change (not only on
// clean app close like window bounds) since a crash/force-quit shouldn't
// lose it.
// v0.0.924 (2026-10-05) — the Standalone's device kept its user settings only
// until something made JUCE restart it on its own: user logs (2026-10-04/05,
// three tests incl. the MacBook speakers as main device) show, every time,
// the engine starting at 512, switching to the user's 128, then — shortly
// after the Audio In node opened the FCA1616 — restarting at 512 while
// Patchy's settings still showed 128; every device followed (v0.0.915) and
// the FCA popped. JUCE re-opens a device with its own default buffer size
// when it restarts it itself (e.g. on macOS device notifications). Watch the
// device manager: if the running device no longer matches the user's
// settings, re-apply them (async — never from inside the notification), at
// most once per second and 3 times in a row, so a device that genuinely
// can't run at the requested setting isn't fought in a loop.
void StandaloneWindow::changeListenerCallback (juce::ChangeBroadcaster*)
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr) return;

    const int    bs = device->getCurrentBufferSizeSamples();
    const double sr = device->getCurrentSampleRate();
    const bool matches = bs == audioSettings.bufferSize
                      && std::abs (sr - audioSettings.sampleRate) < 1.0;
    if (matches) { settingsRestoreAttempts = 0; return; }

    const auto now = juce::Time::getMillisecondCounter();
    if (settingsRestoreAttempts >= 3 || now - lastSettingsRestoreMs < 1000) return;
    ++settingsRestoreAttempts;
    lastSettingsRestoreMs = now;

    juce::Logger::writeToLog ("Standalone: audio device now at " + juce::String ((int) sr) + " Hz / "
                              + juce::String (bs) + " without a settings change - restoring "
                              + juce::String ((int) audioSettings.sampleRate) + " / "
                              + juce::String (audioSettings.bufferSize)
                              + " (attempt " + juce::String (settingsRestoreAttempts) + ")");
    juce::Component::SafePointer<StandaloneWindow> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe != nullptr)
            safe->applyAudioSettings (safe->audioSettings);
    });
}

void StandaloneWindow::saveAudioSettings()
{
    if (auto* p = appProperties.getUserSettings())
    {
        p->setValue ("audioBufferSize", audioSettings.bufferSize);
        p->saveIfNeeded();
    }
}

void StandaloneWindow::restoreAudioSettings()
{
    if (auto* p = appProperties.getUserSettings())
    {
        int savedBufferSize = p->getIntValue ("audioBufferSize", audioSettings.bufferSize);
        if (savedBufferSize != audioSettings.bufferSize)
        {
            AudioSettings s = audioSettings;
            s.bufferSize = savedBufferSize;
            applyAudioSettings (s);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// StandaloneApp
// ─────────────────────────────────────────────────────────────────────────────
void StandaloneApp::initialise (const juce::String&)
{
    mainWindow = std::make_unique<StandaloneWindow>();
}

void StandaloneApp::shutdown()
{
    mainWindow.reset();
}

START_JUCE_APPLICATION (StandaloneApp)

#endif // JUCE_STANDALONE_APPLICATION
