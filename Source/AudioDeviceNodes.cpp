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
    // v0.0.915 (Side finding 1, 2026-09-27) — fresh opens get the host's
    // target config; transferred devices (previously skipped entirely, so
    // they never followed a host buffer/rate change) are synced to it.
    if (auto* n = graph.findAudioOutNode (nodeId))
    {
        if (deviceName.isEmpty())
            n->closeDevice();           // always close, even if transferred
        else if (! n->wasTransferred())
            // Fix, v0.0.922 — "DAW" never touches hardware: give it the
            // never-initialised placeholder instead of creating a per-node
            // manager, whose initialiseWithDefaultDevices() OPENED the default
            // devices for nothing (in the host's process under Rosetta: Logic's
            // own output — see getAvailableDevicesVar()).
            n->openDevice (deviceName, deviceName == "DAW" ? dawPlaceholderManager : getOrCreateOutputManager (nodeId),
                           targetSampleRate, targetBlockSize);
        else
            n->syncDeviceConfig (targetSampleRate, targetBlockSize);
        return true;
    }
    if (auto* n = graph.findAudioInNode (nodeId))
    {
        if (deviceName.isEmpty())
            n->closeDevice();           // always close, even if transferred
        else if (! n->wasTransferred())
            n->openDevice (deviceName, deviceName == "DAW" ? dawPlaceholderManager : getOrCreateInputManager (nodeId),
                           targetSampleRate, targetBlockSize);   // see the Out branch above
        else
            n->syncDeviceConfig (targetSampleRate, targetBlockSize);
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

    // Fix, v0.0.922 (2026-10-03) — this used to call
    // tempManager.initialiseWithDefaultDevices (2, 2), which OPENS and starts
    // the default input + output devices just to read a list. In a plug-in
    // hosted in the host's own process (Logic under Rosetta loads Intel
    // plug-ins in-process) that opened Logic's own output device with JUCE's
    // default buffer settings — "Sample Rate 10 986 recognized / check
    // conflict with external device", Logic's engine running 4× too slow
    // (callbacks every ~93 ms instead of ~23 ms). Listing needs no open
    // device: getAvailableDeviceTypes() creates and scans the types itself,
    // and createDevice() below only builds device objects to read channel
    // names, without opening them.
    juce::AudioDeviceManager tempManager;

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
//  v0.0.915 (Side finding 1, 2026-09-27) — shared device-config helpers
// ─────────────────────────────────────────────────────────────────────────────

/** Falls back to the old NodeProcessor-style defaults only if no valid
 *  target was passed (should not happen once the processor is set up). */
static double validRate  (double sr) { return sr > 0.0 ? sr : 44100.0; }
static int    validBlock (int bs)    { return bs > 0   ? bs : 512; }

/** Reconfigures an already-open device to sampleRate/blockSize, keeping its
 *  channel setup. Skips the restart when the device already runs at that
 *  config — e.g. the same physical device as the standalone host's own,
 *  which already follows the host (seen on the MacBook, 2026-09-23).
 *  Returns true only if the device was actually restarted. */
static bool reconfigureDevice (juce::AudioDeviceManager& manager,
                               double sampleRate, int blockSize,
                               const juce::String& who)
{
    auto* dev = manager.getCurrentAudioDevice();
    if (dev == nullptr)
        return false;
    if (std::abs (dev->getCurrentSampleRate() - sampleRate) < 0.01
        && dev->getCurrentBufferSizeSamples() == blockSize)
        return false;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    manager.getAudioDeviceSetup (setup);
    setup.sampleRate = sampleRate;
    setup.bufferSize = blockSize;
    auto err = manager.setAudioDeviceSetup (setup, true);
    if (err.isNotEmpty())
    {
        juce::Logger::writeToLog (who + ": reconfigure failed: " + err);
        return false;
    }
    if (auto* d = manager.getCurrentAudioDevice())
        juce::Logger::writeToLog (who + ": reconfigured " + d->getName() + " to "
                                  + juce::String ((int) d->getCurrentSampleRate()) + " Hz / "
                                  + juce::String (d->getCurrentBufferSizeSamples())
                                  + " (requested " + juce::String ((int) sampleRate) + " / "
                                  + juce::String (blockSize) + ")");
    return true;
}

static juce::String describeDeviceConfig (juce::AudioDeviceManager& manager)
{
    if (auto* d = manager.getCurrentAudioDevice())
        return " at " + juce::String ((int) d->getCurrentSampleRate()) + " Hz / "
               + juce::String (d->getCurrentBufferSizeSamples());
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioOutDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

void AudioOutDeviceNode::openDevice (const juce::String& deviceName,
                                      juce::AudioDeviceManager& manager,
                                      double targetSampleRate, int targetBlockSize)
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
            // v0.0.915 (Side finding 1) — host target, not this node's
            // not-yet-prepared defaults. See openDevice()'s declaration.
            setup.sampleRate        = validRate  (targetSampleRate);
            setup.bufferSize        = validBlock (targetBlockSize);
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
                requestedSampleRate = setup.sampleRate;   // v0.0.915
                requestedBlockSize  = setup.bufferSize;
                if (! holdsSleepGuard) { SleepGuard::acquire(); holdsSleepGuard = true; }   // v0.0.915
                manager.addAudioCallback (this);
                juce::Logger::writeToLog ("AudioOutDeviceNode: opened " + deviceName
                                          + " (" + juce::String (deviceChannelCount) + " ch)"
                                          + describeDeviceConfig (manager));
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
    if (holdsSleepGuard) { SleepGuard::release(); holdsSleepGuard = false; }   // v0.0.915
    requestedSampleRate = 0.0;   // v0.0.915 — next open starts clean
    requestedBlockSize  = 0;
    registeredDeviceName.clear();
    transferred = false;  // allow openDevice() to work after close
}

void AudioOutDeviceNode::syncDeviceConfig (double targetSampleRate, int targetBlockSize)
{
    // v0.0.915 (Side finding 1, 2026-09-27) — a transferred device was never
    // reconfigured, so after a host buffer/rate change it kept its old
    // config (studio: SYSTEM-8 stayed at 44100/512 with the graph at 32).
    // Only acts when the target differs from what was last REQUESTED, so an
    // ordinary graph edit (same config) never touches the device, and a
    // device that can't honour a block size exactly isn't restarted on
    // every rebuild.
    if (devManager == nullptr || isDawDevice) return;
    const double sr = validRate  (targetSampleRate);
    const int    bs = validBlock (targetBlockSize);
    if (std::abs (sr - requestedSampleRate) < 0.01 && bs == requestedBlockSize) return;
    requestedSampleRate = sr;
    requestedBlockSize  = bs;
    reconfigureDevice (*devManager, sr, bs, "AudioOutDeviceNode");
}

void AudioOutDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    audioFifo->prepareFor (deviceChannelCount, (int) sampleRate, maxBlockSize);   // Shared-fifo fix, 2026-09-24
}

void AudioOutDeviceNode::process (int numSamples)
{
    for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
        outputAudio.copyFrom (ch, 0, inputAudio, ch, 0, numSamples);

    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }
    audioFifo->write (inputAudio, numSamples, chans);
}

void AudioOutDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const*, int,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    // Shared-fifo fix, 2026-09-24 — handed over: output silence, never
    // touch the shared fifo (JUCE sums overlapping callbacks' outputs).
    if (callbackHandedOver.load (std::memory_order_acquire))
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);
        return;
    }

    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }
    audioFifo->read (outputChannelData, numOutputChannels, numSamples, chans);

    // TEMPORARY diagnostic (2026-09-25) — log each new starved/trim event.
    {
        const int st = audioFifo->starvedCount.load (std::memory_order_relaxed);
        if (st != diagLastStarved)
        {
            diagLastStarved = st;
            diagLog ("AudioOut[" + registeredDeviceName + "] STARVED — underrun, silent block + re-prime (total " + juce::String (st) + ")");
        }
        const int tr = audioFifo->trimCount.load (std::memory_order_relaxed);
        if (tr != diagLastTrims)
        {
            diagLastTrims = tr;
            diagLog ("AudioOut[" + registeredDeviceName + "] TRIM — surplus discarded (total " + juce::String (tr) + ")");
        }
    }

    // TEMPORARY diagnostic (2026-09-25) — driver-level dropouts, polled
    // about every 250 ms on the device thread; logged only when the
    // device's own xrun count goes up.
    diagXrunSamples += numSamples;
    if (diagXrunSamples >= (int) (currentSampleRate * 0.25))
    {
        diagXrunSamples = 0;
        if (auto* mgr = devManager)
            if (auto* dev = mgr->getCurrentAudioDevice())
            {
                const int x = dev->getXRunCount();
                if (x >= 0)
                {
                    if (diagLastXruns >= 0 && x > diagLastXruns)
                        diagLog ("AudioOut[" + registeredDeviceName + "] XRUN — driver dropout (+"
                                 + juce::String (x - diagLastXruns) + ", total " + juce::String (x) + ")");
                    diagLastXruns = x;
                }
            }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioInDeviceNode
// ─────────────────────────────────────────────────────────────────────────────

void AudioInDeviceNode::openDevice (const juce::String& deviceName,
                                     juce::AudioDeviceManager& manager,
                                     double targetSampleRate, int targetBlockSize)
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
            // v0.0.915 (Side finding 1) — host target, not this node's
            // not-yet-prepared defaults. See openDevice()'s declaration.
            setup.sampleRate        = validRate  (targetSampleRate);
            setup.bufferSize        = validBlock (targetBlockSize);
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
                // Fix, 2026-09-24 — see fadeInArmed's own comment; armed BEFORE
                // the callback starts writing. Targeted: only for devices listed
                // in StartupFadeRegistry, with that device's own mute duration.
                armStartupFade (deviceName);
                requestedSampleRate = setup.sampleRate;   // v0.0.915
                requestedBlockSize  = setup.bufferSize;
                if (! holdsSleepGuard) { SleepGuard::acquire(); holdsSleepGuard = true; }   // v0.0.915
                manager.addAudioCallback (this);
                juce::Logger::writeToLog ("AudioInDeviceNode: opened " + deviceName
                                          + " (" + juce::String (deviceChannelCount) + " ch)"
                                          + describeDeviceConfig (manager));
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
    if (holdsSleepGuard) { SleepGuard::release(); holdsSleepGuard = false; }   // v0.0.915
    requestedSampleRate = 0.0;   // v0.0.915 — next open starts clean
    requestedBlockSize  = 0;
    registeredDeviceName.clear();
    transferred = false;  // allow openDevice() to work after close
}

void AudioInDeviceNode::armStartupFade (const juce::String& deviceName)
{
    // Moved here unchanged from openDevice() (v0.0.915) so a reconfigure
    // restart can arm it too — see syncDeviceConfig().
    const int muteMs = StartupFadeRegistry::getMuteMs (deviceName);
    fadeListed.store (muteMs >= 0, std::memory_order_relaxed);   // v0.0.923 — dropout fade
    if (muteMs >= 0)
    {
        fadeInMuteMs.store (muteMs, std::memory_order_relaxed);
        fadeInArmed.store (true, std::memory_order_relaxed);
    }
    else
    {
        // Not listed: clear any arm left over from a previous,
        // listed device on this node that never got to start.
        fadeInArmed.store (false, std::memory_order_relaxed);
    }
}

void AudioInDeviceNode::refreshStartupFadeListing()
{
    const int muteMs = registeredDeviceName.isEmpty() ? -1 : StartupFadeRegistry::getMuteMs (registeredDeviceName);
    fadeListed.store (muteMs >= 0, std::memory_order_relaxed);
    if (muteMs >= 0)
        fadeInMuteMs.store (muteMs, std::memory_order_relaxed);
}

void AudioInDeviceNode::syncDeviceConfig (double targetSampleRate, int targetBlockSize)
{
    // v0.0.915 (Side finding 1, 2026-09-27) — same as AudioOutDeviceNode's
    // own syncDeviceConfig(); see its comment. Plus: reconfiguring restarts
    // the device's stream, and a listed device (FCA1616) pops on every
    // stream start, so the startup fade is re-armed when a restart actually
    // happened. Armed after the restart; the pop lands ~1 s after the first
    // audio, and the fade only starts counting at the first real read.
    if (devManager == nullptr || isDawDevice) return;
    const double sr = validRate  (targetSampleRate);
    const int    bs = validBlock (targetBlockSize);
    if (std::abs (sr - requestedSampleRate) < 0.01 && bs == requestedBlockSize) return;
    requestedSampleRate = sr;
    requestedBlockSize  = bs;
    if (reconfigureDevice (*devManager, sr, bs, "AudioInDeviceNode"))
        armStartupFade (registeredDeviceName);
}

void AudioInDeviceNode::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    audioFifo->prepareFor (deviceChannelCount, (int) sampleRate, maxBlockSize);   // Shared-fifo fix, 2026-09-24
}

void AudioInDeviceNode::process (int numSamples)
{
    // See passesThroughWhenDisabled()'s own comment in AudioDeviceNodes.h
    // for the full story. Always drains audioFifo (never backlogs), but
    // only actually populates outputAudio when enabled — while disabled,
    // drained audio is simply discarded, keeping this node correctly
    // "cut" (silent) rather than accidentally pass-through. Reuses the
    // already-allocated outputAudio member as the drain destination even
    // while disabled (rather than a new scratch buffer) — this project's
    // own established rule against heap allocation on the audio thread.
    const bool gotAudio = audioFifo->read (outputAudio, numSamples);
    if (disabled)
        outputAudio.clear();

    // Fix, 2026-09-24 — open-time click: mute-then-ramp after a fresh open.
    // See fadeInArmed's own comment in AudioDeviceNodes.h.
    if (gotAudio && fadeInArmed.exchange (false, std::memory_order_relaxed))
        fadeInPos = 0;

    // v0.0.923 — dropout fade: audio back after a gap of at least
    // kDropoutSeconds → the same mute-then-ramp, for listed devices. See
    // fadeListed's own comment in AudioDeviceNodes.h.
    if (! gotAudio)
        starvedSamples += numSamples;
    else
    {
        if (fadeInPos < 0
            && fadeListed.load (std::memory_order_relaxed)
            && starvedSamples >= (juce::int64) (currentSampleRate * kDropoutSeconds))
        {
            fadeInPos = 0;
            diagLog ("AudioIn[" + registeredDeviceName + "] dropout of "
                     + juce::String ((int) (1000.0 * (double) starvedSamples / currentSampleRate))
                     + " ms - fade re-armed");
        }
        starvedSamples = 0;
    }

    if (fadeInPos >= 0)
    {
        const int muteLen = (int) (currentSampleRate * 0.001 * fadeInMuteMs.load (std::memory_order_relaxed));
        const int rampLen = juce::jmax (1, (int) (currentSampleRate * kFadeInRampSeconds));
        for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
        {
            float* d = outputAudio.getWritePointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const int pos = fadeInPos + i;
                if (pos < muteLen)                d[i] = 0.0f;
                else if (pos < muteLen + rampLen) d[i] *= (float) (pos - muteLen) / (float) rampLen;
            }
        }
        fadeInPos += numSamples;
        if (fadeInPos >= muteLen + rampLen)
            fadeInPos = -1;
    }

    // TEMPORARY diagnostic (2026-09-25) — log each new starved/trim event.
    {
        const int st = audioFifo->starvedCount.load (std::memory_order_relaxed);
        if (st != diagLastStarved)
        {
            diagLastStarved = st;
            diagLog ("AudioIn[" + registeredDeviceName + "] STARVED — short read, silent block (total " + juce::String (st) + ")");
        }
        const int tr = audioFifo->trimCount.load (std::memory_order_relaxed);
        if (tr != diagLastTrims)
        {
            diagLastTrims = tr;
            diagLog ("AudioIn[" + registeredDeviceName + "] TRIM — surplus discarded (total " + juce::String (tr) + ")");
        }
    }
}

void AudioInDeviceNode::audioDeviceIOCallbackWithContext (
    const float* const* inputChannelData, int numInputChannels,
    float* const*, int, int numSamples,
    const juce::AudioIODeviceCallbackContext&)
{
    // Shared-fifo fix, 2026-09-24 — handed over: the new node's callback
    // is the only writer now (see transferCallbackTo()).
    if (callbackHandedOver.load (std::memory_order_acquire))
        return;

    std::vector<int> chans;
    { juce::SpinLock::ScopedLockType sl (channelLock); chans = selectedChannels; }

    // TEMPORARY diagnostic (2026-09-25) — click detector on the RAW input,
    // before anything in Patchy touches it. See the diag members' comment.
    {
        float worstJump = 0.0f;
        int   worstCh   = -1;
        for (int ch : chans)
        {
            if (ch < 0 || ch >= numInputChannels || ch >= kMaxFifoChans || inputChannelData[ch] == nullptr)
                continue;
            const float* x = inputChannelData[ch];
            float prev = diagDetectorPrimed ? diagPrevSample[(size_t) ch] : x[0];
            float peak = diagPeakJump[(size_t) ch];
            for (int i = 0; i < numSamples; ++i)
            {
                const float jump = std::abs (x[i] - prev);
                if (jump > kClickMinJump && jump > kClickPeakRatio * peak && jump > worstJump)
                {
                    worstJump = jump;
                    worstCh   = ch;
                }
                peak = juce::jmax (peak * kClickPeakDecay, jump);
                prev = x[i];
            }
            diagPrevSample[(size_t) ch] = prev;
            diagPeakJump[(size_t) ch]   = peak;
        }
        diagDetectorPrimed = true;
        diagSamplesSinceClick = juce::jmin (diagSamplesSinceClick + numSamples, 1 << 30);
        if (worstCh >= 0 && diagSamplesSinceClick >= (int) (currentSampleRate * 0.25))
        {
            diagSamplesSinceClick = 0;
            diagLog ("AudioIn[" + registeredDeviceName + "] CLICK in raw input — ch "
                     + juce::String (worstCh + 1) + ", jump " + juce::String (worstJump, 3));
        }
    }

    // TEMPORARY diagnostic (2026-09-25) — driver-level dropouts, polled
    // about every 250 ms on the device thread; logged only when the
    // device's own xrun count goes up.
    diagXrunSamples += numSamples;
    if (diagXrunSamples >= (int) (currentSampleRate * 0.25))
    {
        diagXrunSamples = 0;
        if (auto* mgr = devManager)
            if (auto* dev = mgr->getCurrentAudioDevice())
            {
                const int x = dev->getXRunCount();
                if (x >= 0)
                {
                    if (diagLastXruns >= 0 && x > diagLastXruns)
                        diagLog ("AudioIn[" + registeredDeviceName + "] XRUN — driver dropout (+"
                                 + juce::String (x - diagLastXruns) + ", total " + juce::String (x) + ")");
                    diagLastXruns = x;
                }
            }
    }

    audioFifo->write (inputChannelData, numInputChannels, numSamples, chans);
}
