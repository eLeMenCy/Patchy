#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "NodeProcessor.h"
#include "GraphModel.h"
#include <atomic>
#include <vector>
#include <mutex>

class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
/**
 * AudioOutDeviceNode  (nodeType 6)
 *
 * 1 stereo Audio input port — receives audio from upstream graph nodes.
 * Sends that audio to the selected physical audio output device.
 */
class AudioOutDeviceNode : public NodeProcessor,
                            private juce::AudioIODeviceCallback
{
public:
    explicit AudioOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Audio) {}

    ~AudioOutDeviceNode() override { closeDevice(); }  // closeDevice() is idempotent

    void openDevice (const juce::String& deviceName,
                     juce::AudioDeviceManager& deviceManager);
    void closeDevice();

    juce::String getSelectedDeviceName() const { return registeredDeviceName; }
    void markTransferred() { transferred = true; }
    bool wasTransferred()  const { return transferred; }
    bool getIsDawDevice()  const { return isDawDevice; }
    template <typename NodeT>
    void transferCallbackTo (NodeT& dst)
    {
        if (devManager == nullptr) return;
        devManager->removeAudioCallback (this);
        dst.devManager           = devManager;
        dst.registeredDeviceName = registeredDeviceName;
        dst.currentSampleRate    = currentSampleRate;
        dst.currentBlockSize     = currentBlockSize;
        devManager->addAudioCallback (&dst);
        devManager = nullptr; registeredDeviceName = {};
    }

    // NodeProcessor
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (int numSamples) override;

    juce::String selectedDeviceName;

private:
    // AudioIODeviceCallback — called by the output device thread
    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped()                            override {}

    // Lock-free multi-channel audio FIFO (graph thread writes, device thread reads)
    static constexpr int kFifoFrames    = 8192;
    static constexpr int kMaxFifoChans  = 256;

    struct AudioFifo
    {
        // Pre-allocate all channels at construction — never reallocates at runtime.
        AudioFifo()
        {
            data.resize (kMaxFifoChans, std::vector<float> (kFifoFrames, 0.0f));
        }

        // Safe to call from prepare() — only updates numChannels and zeroes slots.
        // Does NOT resize data, so device-callback thread is never racing a realloc.
        void reset (int numPhysical, int /*sampleRate*/)
        {
            numChannels = juce::jlimit (1, kMaxFifoChans, numPhysical);
            fifo.reset();
            primed = false;
            for (int i = 0; i < numChannels; ++i)
                std::fill (data[static_cast<size_t>(i)].begin(),
                           data[static_cast<size_t>(i)].end(), 0.0f);
        }

        // Write graph channels 0,1,2… into contiguous FIFO slots.
        // selected[slot] = physical output channel — used by read() for routing.
        void write (const juce::AudioBuffer<float>& src,
                    int numFrames,
                    const std::vector<int>& selected)
        {
            const int slots = juce::jlimit (1, kMaxFifoChans, (int) selected.size());
            numChannels = slots;

            int s1, n1, s2, n2;
            fifo.prepareToWrite (numFrames, s1, n1, s2, n2);
            for (int slot = 0; slot < slots; ++slot)
            {
                float* wr = data[static_cast<size_t>(slot)].data();
                // Always read from contiguous graph channel 'slot', not from physical index
                if (slot < src.getNumChannels())
                {
                    const float* rd = src.getReadPointer (slot);
                    if (n1 > 0) std::memcpy (wr + s1, rd,      static_cast<size_t>(n1) * sizeof(float));
                    if (n2 > 0) std::memcpy (wr + s2, rd + n1, static_cast<size_t>(n2) * sizeof(float));
                }
                else
                {
                    if (n1 > 0) std::memset (wr + s1, 0, static_cast<size_t>(n1) * sizeof(float));
                    if (n2 > 0) std::memset (wr + s2, 0, static_cast<size_t>(n2) * sizeof(float));
                }
            }
            fifo.finishedWrite (n1 + n2);
            if (! primed && fifo.getNumReady() >= numFrames * 2)
                primed = true;
        }

        // Read FIFO slots into the selected physical output channels.
        void read (float* const* dst,
                   int numOutputChannels,
                   int numFrames,
                   const std::vector<int>& selected)
        {
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (dst[ch]) std::memset (dst[ch], 0, static_cast<size_t>(numFrames) * sizeof(float));

            if (! primed) return;

            if (fifo.getNumReady() < numFrames)
            {
                fifo.reset();
                primed = false;
                return;
            }

            int s1, n1, s2, n2;
            fifo.prepareToRead (numFrames, s1, n1, s2, n2);
            const int slots = juce::jlimit (0, numChannels, (int) selected.size());
            for (int slot = 0; slot < slots; ++slot)
            {
                int phys = selected[static_cast<size_t>(slot)];
                if (phys < 0 || phys >= numOutputChannels) continue;
                if (dst[phys] == nullptr) continue;
                const float* rd = data[static_cast<size_t>(slot)].data();
                if (n1 > 0) std::memcpy (dst[phys],      rd + s1, static_cast<size_t>(n1) * sizeof(float));
                if (n2 > 0) std::memcpy (dst[phys] + n1, rd + s2, static_cast<size_t>(n2) * sizeof(float));
            }
            fifo.finishedRead (n1 + n2);
        }

        juce::AbstractFifo                          fifo { kFifoFrames };
        std::vector<std::vector<float>>             data;   // kMaxFifoChans × kFifoFrames, fixed after ctor
        int  numChannels = 2;
        bool primed      = false;
    } audioFifo;

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    bool                      isDawDevice = false;

    // selectedChannels is written from the message thread and read from the
    // device callback thread — protect with a SpinLock.
    std::vector<int>    selectedChannels { 0, 1 };
    juce::SpinLock      channelLock;
    int                 deviceChannelCount = 2;

public:
    void setSelectedChannels (std::vector<int> chans)
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        selectedChannels = std::move (chans);
    }
    std::vector<int> getSelectedChannels() const
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        return selectedChannels;
    }
    int getDeviceChannelCount() const { return deviceChannelCount; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioOutDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * AudioInDeviceNode  (nodeType 7)
 *
 * 1 stereo Audio output port — feeds audio to downstream graph nodes.
 * Receives audio asynchronously from a physical audio input device via a
 * lock-free FIFO; drains it into outputAudio each process() call.
 */
class AudioInDeviceNode : public NodeProcessor,
                           private juce::AudioIODeviceCallback
{
public:
    explicit AudioInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Audio) {}

    ~AudioInDeviceNode() override { closeDevice(); }   // closeDevice() is idempotent

    void openDevice (const juce::String& deviceName,
                     juce::AudioDeviceManager& deviceManager);
    void closeDevice();

    juce::String getSelectedDeviceName() const { return registeredDeviceName; }
    void markTransferred() { transferred = true; }
    bool wasTransferred()  const { return transferred; }
    bool getIsDawDevice()  const { return isDawDevice; }
    template <typename NodeT>
    void transferCallbackTo (NodeT& dst)
    {
        if (devManager == nullptr) return;
        devManager->removeAudioCallback (this);
        dst.devManager           = devManager;
        dst.registeredDeviceName = registeredDeviceName;
        dst.currentSampleRate    = currentSampleRate;
        dst.currentBlockSize     = currentBlockSize;
        devManager->addAudioCallback (&dst);
        devManager = nullptr; registeredDeviceName = {};
    }

    // NodeProcessor
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (int numSamples) override;

    juce::String selectedDeviceName;

private:
    void audioDeviceIOCallbackWithContext (const float* const*,
                                           int,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped()                           override {}

    static constexpr int kFifoFrames    = 8192;
    static constexpr int kMaxFifoChans  = 256;

    struct AudioFifo
    {
        AudioFifo()
        {
            data.resize (kMaxFifoChans, std::vector<float> (kFifoFrames, 0.0f));
        }

        void reset (int numPhysical, int /*sampleRate*/)
        {
            numChannels = juce::jlimit (1, kMaxFifoChans, numPhysical);
            fifo.reset();
            primed = false;
            for (int i = 0; i < numChannels; ++i)
                std::fill (data[static_cast<size_t>(i)].begin(),
                           data[static_cast<size_t>(i)].end(), 0.0f);
        }

        // Write selected physical input channels into contiguous FIFO slots.
        // Unselected channels are simply not written — numChannels == selected.size().
        void write (const float* const* src,
                    int numInputChannels,
                    int numFrames,
                    const std::vector<int>& selected)
        {
            // Resize slot count to match selection — no realloc, just update numChannels
            numChannels = juce::jlimit (1, kMaxFifoChans, (int) selected.size());

            int s1, n1, s2, n2;
            fifo.prepareToWrite (numFrames, s1, n1, s2, n2);
            for (int slot = 0; slot < numChannels; ++slot)
            {
                int phys = selected[static_cast<size_t>(slot)];
                float* wr = data[static_cast<size_t>(slot)].data();
                if (phys >= 0 && phys < numInputChannels && src[phys] != nullptr)
                {
                    if (n1 > 0) std::memcpy (wr + s1, src[phys],      static_cast<size_t>(n1) * sizeof(float));
                    if (n2 > 0) std::memcpy (wr + s2, src[phys] + n1, static_cast<size_t>(n2) * sizeof(float));
                }
                else
                {
                    if (n1 > 0) std::memset (wr + s1, 0, static_cast<size_t>(n1) * sizeof(float));
                    if (n2 > 0) std::memset (wr + s2, 0, static_cast<size_t>(n2) * sizeof(float));
                }
            }
            fifo.finishedWrite (n1 + n2);
        }

        // Read FIFO slots into contiguous dst channels 0, 1, …
        void read (juce::AudioBuffer<float>& dst, int numFrames)
        {
            dst.clear();
            if (fifo.getNumReady() < numFrames) return;

            int s1, n1, s2, n2;
            fifo.prepareToRead (numFrames, s1, n1, s2, n2);
            const int slots = std::min (numChannels, dst.getNumChannels());
            for (int slot = 0; slot < slots; ++slot)
            {
                const float* rd = data[static_cast<size_t>(slot)].data();
                float* wr = dst.getWritePointer (slot);
                if (n1 > 0) std::memcpy (wr,      rd + s1, static_cast<size_t>(n1) * sizeof(float));
                if (n2 > 0) std::memcpy (wr + n1, rd + s2, static_cast<size_t>(n2) * sizeof(float));
            }
            fifo.finishedRead (n1 + n2);
        }

        juce::AbstractFifo              fifo { kFifoFrames };
        std::vector<std::vector<float>> data;   // kMaxFifoChans × kFifoFrames, fixed after ctor
        int  numChannels = 2;
        bool primed      = false;
    } audioFifo;

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    bool                      isDawDevice = false;

    // selectedChannels is written from the message thread and read from the
    // device callback thread — protect with a SpinLock.
    std::vector<int>  selectedChannels { 0, 1 };
    juce::SpinLock    channelLock;
    int               deviceChannelCount = 2;

public:
    void setSelectedChannels (std::vector<int> chans)
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        selectedChannels = std::move (chans);
    }
    std::vector<int> getSelectedChannels() const
    {
        juce::SpinLock::ScopedLockType sl (channelLock);
        return selectedChannels;
    }
    int getDeviceChannelCount() const { return deviceChannelCount; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioInDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * AudioDeviceManager
 *
 * Owns a juce::AudioDeviceManager shared between all audio device nodes.
 * Handles device open/close and persists selections.
 */
class AudioDeviceManager
{
public:
    AudioDeviceManager();

    void storeSelection (const juce::String& nodeId,
                         const juce::String& deviceName)
    {
        selections[nodeId] = deviceName;
    }

    juce::String getSelection (const juce::String& nodeId) const
    {
        auto it = selections.find (nodeId);
        return it != selections.end() ? it->second : juce::String{};
    }

    bool applyToGraph (const juce::String& nodeId,
                       const juce::String& deviceName,
                       ProcessingGraph& graph);

    void applyDeviceSelections (ProcessingGraph& graph);

    void setDevice (const juce::String& nodeId,
                    const juce::String& deviceName,
                    ProcessingGraph& graph)
    {
        storeSelection (nodeId, deviceName);
        applyToGraph   (nodeId, deviceName, graph);
    }

    /** Store and apply a channel selection for an audio device node.
     *  Returns the new deviceChannelCount so the caller can validate. */
    void storeChannelSelection (const juce::String& nodeId, const std::vector<int>& channels)
    {
        channelSelections[nodeId] = channels;
    }

    void setAudioDeviceChannels (const juce::String& nodeId,
                                  const std::vector<int>& channels,
                                  ProcessingGraph& graph)
    {
        channelSelections[nodeId] = channels;
        applyChannelsToGraph (nodeId, channels, graph);
    }

    void applyChannelsToGraph (const juce::String& nodeId,
                                const std::vector<int>& channels,
                                ProcessingGraph& graph);

    void applyAllChannelSelections (ProcessingGraph& graph);

    const std::vector<int>* getChannelSelection (const juce::String& nodeId) const
    {
        auto it = channelSelections.find (nodeId);
        return it != channelSelections.end() ? &it->second : nullptr;
    }

    static juce::var getAvailableDevicesVar (bool isStandalone = true);

    juce::AudioDeviceManager& getOutputManager() { return outputManager; }
    juce::AudioDeviceManager& getInputManager()  { return inputManager;  }

private:
    juce::AudioDeviceManager             outputManager;
    juce::AudioDeviceManager             inputManager;
    std::unordered_map<juce::String, juce::String>       selections;
    std::unordered_map<juce::String, std::vector<int>>   channelSelections;
};
