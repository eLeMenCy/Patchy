#include "AudioDeviceNodes.h"

#include "ProcessingGraph.h"

// ─────────────────────────────────────────────────────────────────────────────
//  AudioDeviceManager
// ─────────────────────────────────────────────────────────────────────────────

AudioDeviceManager::AudioDeviceManager()
{
    // Deliberately empty now — see this class's own outputManagers/
    // inputManagers member comments in AudioDeviceNodes.h for the full
    // story. Each node's own manager is created lazily, on first use, by
    // getOrCreateOutputManager()/getOrCreateInputManager() below — there
    // is no longer a single, shared manager to eagerly initialise here.
}

bool AudioDeviceManager::applyToGraph (const juce::String& nodeId,
                                        const juce::String& deviceName,
                                        ProcessingGraph& graph)
{
    if (auto* n = graph.findAudioOutNode (nodeId))
    {
        if (deviceName.isEmpty())
            n->closeDevice();           // always close, even if transferred
        else if (! n->wasTransferred())
            n->openDevice (deviceName, getOrCreateOutputManager (nodeId));
        return true;
    }
    if (auto* n = graph.findAudioInNode (nodeId))
    {
        if (deviceName.isEmpty())
            n->closeDevice();           // always close, even if transferred
        else if (! n->wasTransferred())
            n->openDevice (deviceName, getOrCreateInputManager (nodeId));
        return true;
    }
    return false;
}

void AudioDeviceManager::pruneDeletedNodeManagers (ProcessingGraph& graph)
{
    // See this method's own declaration comment in AudioDeviceNodes.h for
    // the full reasoning — called once per rebuild, after
    // applyDeviceSelections(), once the new graph's own node list is
    // final. erase() on a std::unique_ptr-valued map entry destroys the
    // juce::AudioDeviceManager, which closes whatever real device it may
    // still hold open.
    for (auto it = outputManagers.begin(); it != outputManagers.end(); )
        it = (graph.findAudioOutNode (it->first) == nullptr) ? outputManagers.erase (it) : std::next (it);
    for (auto it = inputManagers.begin(); it != inputManagers.end(); )
        it = (graph.findAudioInNode (it->first) == nullptr) ? inputManagers.erase (it) : std::next (it);
}

void AudioDeviceManager::applyDeviceSelections (ProcessingGraph& graph)
{
    for (const auto& [nodeId, deviceName] : selections)
        applyToGraph (nodeId, deviceName, graph);
}

void AudioDeviceManager::applyChannelsToGraph (const juce::String& nodeId,
                                                const std::vector<int>& channels,
                                                ProcessingGraph& graph)
{
    if (auto* n = graph.findAudioOutNode (nodeId))
        n->setSelectedChannels (channels);
    else if (auto* nIn = graph.findAudioInNode (nodeId))
        nIn->setSelectedChannels (channels);
}

void AudioDeviceManager::applyAllChannelSelections (ProcessingGraph& graph)
{
    for (const auto& [nodeId, channels] : channelSelections)
        applyChannelsToGraph (nodeId, channels, graph);
}

juce::var AudioDeviceManager::getAvailableDevicesVar (bool isStandalone)
{
    // Enumerate all available audio device types and their devices
    juce::Array<juce::var> outArr, inArr;

    // In DAW mode, add virtual DAW device at the top of both lists
    if (! isStandalone)
    {
        auto makeDaw = [](const char* label) -> juce::var {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id",   "DAW");
            obj->setProperty ("name", label);
            return obj;
        };
        outArr.add (makeDaw ("DAW"));
        inArr.add  (makeDaw ("DAW"));
    }

    juce::AudioDeviceManager tempManager;
    tempManager.initialiseWithDefaultDevices (2, 2);

    // Known DAW virtual device patterns — confusing and potentially dangerous
    auto isDawVirtualDevice = [](const juce::String& name) -> bool {
        return name.containsIgnoreCase ("Bitwig")
            || name.containsIgnoreCase ("Ableton")
            || name.containsIgnoreCase ("Logic Pro")
            || name.containsIgnoreCase ("Pro Tools")
            || name.containsIgnoreCase ("Reaper")
            || name.containsIgnoreCase ("Cubase")
            || name.containsIgnoreCase ("Studio One")
            || name.containsIgnoreCase ("FL Studio")
            || name.containsIgnoreCase ("Ardour");
    };

    for (auto* type : tempManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        // Output devices
        for (const auto& name : type->getDeviceNames (false))
        {
            int chCount = 2;
            if (auto* dev = type->createDevice ({}, name))
            {
                chCount = dev->getOutputChannelNames().size();
                delete dev;
            }
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id",           name);
            obj->setProperty ("name",         name);
            obj->setProperty ("dawHost",      isDawVirtualDevice (name));
            obj->setProperty ("channelCount", chCount > 0 ? chCount : 2);
            outArr.add (obj);
        }
        // Input devices
        for (const auto& name : type->getDeviceNames (true))
        {
            int chCount = 2;
            if (auto* dev = type->createDevice (name, {}))
            {
                chCount = dev->getInputChannelNames().size();
                delete dev;
            }
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("id",           name);
            obj->setProperty ("name",         name);
            obj->setProperty ("dawHost",      isDawVirtualDevice (name));
            obj->setProperty ("channelCount", chCount > 0 ? chCount : 2);
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
    isDawDevice = (deviceName == "DAW");
    if (deviceName.isEmpty() || isDawDevice) { return; }  // DAW handled by ProcessingGraph

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
            setup.useDefaultOutputChannels = false;
            // Activate all possible channels upfront — real count read back after open
            setup.outputChannels.setRange (0, kMaxFifoChans, true);

            auto err = manager.setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty())
                juce::Logger::writeToLog ("AudioOutDeviceNode: " + err);
            else
            {
                if (auto* dev = manager.getCurrentAudioDevice())
                    deviceChannelCount = dev->getOutputChannelNames().size();
                manager.addAudioCallback (this);
                juce::Logger::writeToLog ("AudioOutDeviceNode: opened " + deviceName
                                          + " (" + juce::String (deviceChannelCount) + " ch)");
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
    transferred = false;  // allow openDevice() to work after close
}

void AudioOutDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    audioFifo.reset (deviceChannelCount, (int) sampleRate);
}

void AudioOutDeviceNode::process (int numSamples)
{
    for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
        outputAudio.copyFrom (ch, 0, inputAudio, ch, 0, numSamples);

    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }
    audioFifo.write (inputAudio, numSamples, chans);
}

void AudioOutDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const*, int,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }
    audioFifo.read (outputChannelData, numOutputChannels, numSamples, chans);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioInDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

void AudioInDeviceNode::openDevice (const juce::String& deviceName,
                                     juce::AudioDeviceManager& manager)
{
    closeDevice();
    selectedDeviceName = deviceName;
    isDawDevice = (deviceName == "DAW");
    if (deviceName.isEmpty() || isDawDevice) { return; }  // DAW handled by ProcessingGraph

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
            setup.useDefaultInputChannels  = false;
            setup.useDefaultOutputChannels = false;
            // Activate all possible channels upfront — real count read back after open
            setup.inputChannels.setRange (0, kMaxFifoChans, true);

            auto err = manager.setAudioDeviceSetup (setup, true);
            if (err.isNotEmpty())
                juce::Logger::writeToLog ("AudioInDeviceNode: " + err);
            else
            {
                if (auto* dev = manager.getCurrentAudioDevice())
                    deviceChannelCount = dev->getInputChannelNames().size();
                manager.addAudioCallback (this);
                juce::Logger::writeToLog ("AudioInDeviceNode: opened " + deviceName
                                          + " (" + juce::String (deviceChannelCount) + " ch)");
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
    transferred = false;  // allow openDevice() to work after close
}

void AudioInDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    audioFifo.reset (deviceChannelCount, (int) sampleRate);
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
    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }
    audioFifo.write (inputChannelData, numInputChannels, numSamples, chans);
}
