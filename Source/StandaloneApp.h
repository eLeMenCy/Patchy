// Patchy — Custom Standalone Application
#pragma once

#if JUCE_STANDALONE_APPLICATION

#include <juce_audio_utils/juce_audio_utils.h>
#include "PatchyProcessor.h"
#include "PatchyEditor.h"

class StandaloneWindow;

// ─────────────────────────────────────────────────────────────────────────────
class StandaloneApp final : public juce::JUCEApplication
{
public:
    StandaloneApp() = default;

    const juce::String getApplicationName()    override { return "Patchy"; }
    const juce::String getApplicationVersion() override { return "0.1"; }
    bool moreThanOneInstanceAllowed()          override { return true; }

    void initialise (const juce::String&) override;
    void shutdown()                        override;
    void systemRequestedQuit()             override { quit(); }

private:
    std::unique_ptr<StandaloneWindow> mainWindow;
};

// ─────────────────────────────────────────────────────────────────────────────
// AudioSettings — standalone audio engine configuration
// ─────────────────────────────────────────────────────────────────────────────
struct AudioSettings
{
    double sampleRate  = 44100.0;
    int    bufferSize  = 512;
    bool   muteFeedback = true;
};

// ─────────────────────────────────────────────────────────────────────────────
class StandaloneWindow final : public juce::DocumentWindow
{
public:
    StandaloneWindow();
    ~StandaloneWindow() override;

    void closeButtonPressed() override;
    void saveWindowBounds();
    void restoreWindowBounds();

private:
    juce::ApplicationProperties appProperties;

    // Audio settings
    AudioSettings     getAudioSettings() const { return audioSettings; }
    void              applyAudioSettings (const AudioSettings& s);
    juce::StringArray getAvailableSampleRates() const;
    juce::StringArray getAvailableBufferSizes() const;

    PatchyProcessor& getProcessor() { return *processor; }

private:
    void setupAudioDevice();
    void pushAudioSettingsToUI();

    std::unique_ptr<PatchyProcessor>     processor;
    PatchyEditor*                        editor = nullptr;
    juce::AudioDeviceManager             deviceManager;
    juce::AudioProcessorPlayer           player;
    AudioSettings                        audioSettings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StandaloneWindow)
};

#endif // JUCE_STANDALONE_APPLICATION
