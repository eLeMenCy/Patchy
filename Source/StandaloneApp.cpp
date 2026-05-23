// SPDX-License-Identifier: GPL-3.0-or-later
// Patchy — Custom Standalone Application

#if JUCE_STANDALONE_APPLICATION

#include "StandaloneApp.h"

// ─────────────────────────────────────────────────────────────────────────────
// StandaloneWindow
// ─────────────────────────────────────────────────────────────────────────────
StandaloneWindow::StandaloneWindow()
    : DocumentWindow ("Patchy",
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
    centreWithSize (1200, 750);
    setVisible (true);
}

StandaloneWindow::~StandaloneWindow()
{
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);
    if (editor) processor->editorBeingDeleted (editor);
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
