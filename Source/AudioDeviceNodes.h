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

    // Lock-free stereo audio FIFO (graph thread writes, device thread reads)
    static constexpr int kFifoFrames = 8192;

    struct AudioFifo
    {
        void reset (int channels, int /*sampleRate*/)
        {
            numChannels = std::min (channels, 2);
            fifo.reset();
            primed = false;
            for (auto& ch : data) ch.assign (kFifoFrames, 0.0f);
        }

        void write (const juce::AudioBuffer<float>& src, int numFrames)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (numFrames, s1, n1, s2, n2);
            for (int ch = 0; ch < numChannels && ch < src.getNumChannels(); ++ch)
            {
                if (n1 > 0) std::memcpy (data[static_cast<size_t>(ch)].data() + s1, src.getReadPointer (ch), static_cast<size_t>(n1) * sizeof(float));
                if (n2 > 0) std::memcpy (data[static_cast<size_t>(ch)].data() + s2, src.getReadPointer (ch) + n1, static_cast<size_t>(n2) * sizeof(float));
            }
            fifo.finishedWrite (n1 + n2);
            // Prime output once we have at least 2 blocks of data buffered
            if (! primed && fifo.getNumReady() >= numFrames * 2)
                primed = true;
        }

        void read (float* const* dst, int numOutputChannels, int numFrames)
        {
            // Always zero the output first — protects against underrun garbage
            for (int ch = 0; ch < numOutputChannels; ++ch)
                if (dst[ch]) std::memset (dst[ch], 0, static_cast<size_t>(numFrames) * sizeof(float));

            // Only output audio once FIFO has been primed with real data
            if (! primed) return;

            int available = fifo.getNumReady();
            if (available < numFrames)
            {
                // Underrun — output silence and reset to avoid phase drift
                fifo.reset();
                primed = false;
                return;
            }

            int s1, n1, s2, n2;
            fifo.prepareToRead (numFrames, s1, n1, s2, n2);
            for (int ch = 0; ch < numOutputChannels; ++ch)
            {
                if (dst[ch] == nullptr) continue;
                int srcCh = std::min (ch, numChannels - 1);
                if (srcCh < 0) continue;  // already zeroed above
                if (n1 > 0) std::memcpy (dst[ch], data[static_cast<size_t>(srcCh)].data() + s1, static_cast<size_t>(n1) * sizeof(float));
                if (n2 > 0) std::memcpy (dst[ch] + n1, data[static_cast<size_t>(srcCh)].data() + s2, static_cast<size_t>(n2) * sizeof(float));
            }
            fifo.finishedRead (n1 + n2);
        }

        juce::AbstractFifo         fifo { kFifoFrames };
        std::array<std::vector<float>, 2> data { std::vector<float>(kFifoFrames, 0.0f),
                                                  std::vector<float>(kFifoFrames, 0.0f) };
        int  numChannels = 2;
        bool primed      = false;
    } audioFifo;

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    bool                      isDawDevice = false;

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

    static constexpr int kFifoFrames = 8192;

    struct AudioFifo
    {
        void reset (int channels, int /*sampleRate*/)
        {
            numChannels = std::min (channels, 2);
            fifo.reset();
            primed = false;
            for (auto& ch : data) ch.assign (kFifoFrames, 0.0f);
        }

        void write (const float* const* src, int numInputChannels, int numFrames)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (numFrames, s1, n1, s2, n2);
            for (int ch = 0; ch < numChannels && ch < numInputChannels; ++ch)
            {
                if (src[ch] == nullptr) continue;
                if (n1 > 0) std::memcpy (data[static_cast<size_t>(ch)].data() + s1, src[ch], static_cast<size_t>(n1) * sizeof(float));
                if (n2 > 0) std::memcpy (data[static_cast<size_t>(ch)].data() + s2, src[ch] + n1, static_cast<size_t>(n2) * sizeof(float));
            }
            fifo.finishedWrite (n1 + n2);
        }

        void read (juce::AudioBuffer<float>& dst, int numFrames)
        {
            // Always clear destination first — prevents garbage on underrun
            dst.clear();
            if (fifo.getNumReady() < numFrames) return;  // underrun — output silence

            int s1, n1, s2, n2;
            fifo.prepareToRead (numFrames, s1, n1, s2, n2);
            for (int ch = 0; ch < dst.getNumChannels(); ++ch)
            {
                int srcCh = std::min (ch, numChannels - 1);
                if (srcCh < 0) continue;  // already cleared
                if (n1 > 0) std::memcpy (dst.getWritePointer (ch), data[static_cast<size_t>(srcCh)].data() + s1, static_cast<size_t>(n1) * sizeof(float));
                if (n2 > 0) std::memcpy (dst.getWritePointer (ch) + n1, data[static_cast<size_t>(srcCh)].data() + s2, static_cast<size_t>(n2) * sizeof(float));
            }
            fifo.finishedRead (n1 + n2);
        }

        juce::AbstractFifo         fifo { kFifoFrames };
        std::array<std::vector<float>, 2> data { std::vector<float>(kFifoFrames, 0.0f),
                                                  std::vector<float>(kFifoFrames, 0.0f) };
        int  numChannels = 2;
        bool primed      = false;  // unused for AudioIn but keeps struct consistent
    } audioFifo;

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    bool                      isDawDevice = false;

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

    static juce::var getAvailableDevicesVar (bool isStandalone = true);

    juce::AudioDeviceManager& getOutputManager() { return outputManager; }
    juce::AudioDeviceManager& getInputManager()  { return inputManager;  }

private:
    juce::AudioDeviceManager             outputManager;
    juce::AudioDeviceManager             inputManager;
    std::unordered_map<juce::String, juce::String> selections;
};
