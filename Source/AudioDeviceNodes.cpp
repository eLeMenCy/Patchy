#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"

// ─────────────────────────────────────────────────────────────────────────────
//  AudioDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

AudioDeviceManager::AudioDeviceManager()
{
    // Initialise with no device — nodes open their own devices as needed.
    // Using 0 input and 0 output channels at the manager level; each node
    // registers its own callback for its specific device.
    outputManager.initialiseWithDefaultDevices (0, 2);  // output only
    inputManager.initialiseWithDefaultDevices  (2, 0);  // input only
}

bool AudioDeviceManager::applyToGraph (const juce::String& nodeId,
                                        const juce::String& deviceName,
                                        ProcessingGraph& graph)
{
    if (auto* n = graph.findAudioOutNode (nodeId))
    {
        if (! n->wasTransferred())
            n->openDevice (deviceName, outputManager);
        return true;
    }
    if (auto* n = graph.findAudioInNode (nodeId))
    {
        if (! n->wasTransferred())
            n->openDevice (deviceName, inputManager);
        return true;
    }
    return false;
}

void AudioDeviceManager::applyDeviceSelections (ProcessingGraph& graph)
{
    for (const auto& [nodeId, deviceName] : selections)
        applyToGraph (nodeId, deviceName, graph);
}

juce::var AudioDeviceManager::getAvailableDevicesVar()
{
    // Enumerate all available audio device types and their devices
    juce::Array<juce::var> outArr, inArr;

    juce::AudioDeviceManager tempManager;
    tempManager.initialiseWithDefaultDevices (2, 2);

    for (auto* type : tempManager.getAvailableDeviceTypes())
    {
        // Output devices
        for (const auto& name : type->getDeviceNames (false))
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id",   name);
            obj->setProperty ("name", name);
            outArr.add (obj);
        }
        // Input devices
        for (const auto& name : type->getDeviceNames (true))
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id",   name);
            obj->setProperty ("name", name);
            inArr.add (obj);
        }
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("audioOutDevices", outArr);
    root->setProperty ("audioInDevices",  inArr);
    return root;
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioOutDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

void AudioOutDeviceNode::openDevice (const juce::String& deviceName,
                                      juce::AudioDeviceManager& manager)
{
    closeDevice();
    selectedDeviceName = deviceName;
    if (deviceName.isEmpty()) return;

    devManager           = &manager;
    registeredDeviceName = deviceName;

    // Find the device type that has this device name
    for (auto* type : manager.getAvailableDeviceTypes())
    {
        auto names = type->getDeviceNames (false); // output devices
        if (names.contains (deviceName))
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.outputDeviceName  = deviceName;
            setup.inputDeviceName   = {};
            setup.sampleRate        = currentSampleRate > 0 ? currentSampleRate : 44100.0;
            setup.bufferSize        = currentBlockSize  > 0 ? currentBlockSize  : 512;
            setup.useDefaultInputChannels  = false;
            setup.useDefaultOutputChannels = true;

            auto err = manager.setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty())
                juce::Logger::writeToLog ("AudioOutDeviceNode: " + err);
            else
            {
                manager.addAudioCallback (this);
                juce::Logger::writeToLog ("AudioOutDeviceNode: opened " + deviceName);
            }
            return;
        }
    }
    juce::Logger::writeToLog ("AudioOutDeviceNode: device not found: " + deviceName);
}

void AudioOutDeviceNode::closeDevice()
{
    if (devManager != nullptr)
    {
        devManager->removeAudioCallback (this);
        devManager = nullptr;
    }
    registeredDeviceName.clear();
}

void AudioOutDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    audioFifo.reset (2, (int) sampleRate);
}

void AudioOutDeviceNode::process (int numSamples)
{
    // Pass audio through (so downstream nodes can still use it)
    for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
        outputAudio.copyFrom (ch, 0, inputAudio, ch, 0, numSamples);

    // Push to FIFO for the device callback to consume
    audioFifo.write (inputAudio, numSamples);
}

void AudioOutDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const*, int,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    audioFifo.read (outputChannelData, numOutputChannels, numSamples);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioInDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

void AudioInDeviceNode::openDevice (const juce::String& deviceName,
                                     juce::AudioDeviceManager& manager)
{
    closeDevice();
    selectedDeviceName = deviceName;
    if (deviceName.isEmpty()) return;

    devManager           = &manager;
    registeredDeviceName = deviceName;

    for (auto* type : manager.getAvailableDeviceTypes())
    {
        auto names = type->getDeviceNames (true); // input devices
        if (names.contains (deviceName))
        {
            juce::AudioDeviceManager::AudioDeviceSetup setup;
            setup.inputDeviceName   = deviceName;
            setup.outputDeviceName  = {};
            setup.sampleRate        = currentSampleRate > 0 ? currentSampleRate : 44100.0;
            setup.bufferSize        = currentBlockSize  > 0 ? currentBlockSize  : 512;
            setup.useDefaultInputChannels  = true;
            setup.useDefaultOutputChannels = false;

            auto err = manager.setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty())
                juce::Logger::writeToLog ("AudioInDeviceNode: " + err);
            else
            {
                manager.addAudioCallback (this);
                juce::Logger::writeToLog ("AudioInDeviceNode: opened " + deviceName);
            }
            return;
        }
    }
    juce::Logger::writeToLog ("AudioInDeviceNode: device not found: " + deviceName);
}

void AudioInDeviceNode::closeDevice()
{
    if (devManager != nullptr)
    {
        devManager->removeAudioCallback (this);
        devManager = nullptr;
    }
    registeredDeviceName.clear();
}

void AudioInDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    audioFifo.reset (2, (int) sampleRate);
}

void AudioInDeviceNode::process (int numSamples)
{
    // Drain FIFO into outputAudio for downstream nodes
    audioFifo.read (outputAudio, numSamples);
}

void AudioInDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData, int numInputChannels,
    float* const*, int, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    audioFifo.write (inputChannelData, numInputChannels, numSamples);
}
