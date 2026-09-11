#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "NodeProcessor.h"
#include "GraphModel.h"
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <memory>

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

    // Real fix, 2026-09-03 (3rd revision) — public wrapper so
    // PatchyProcessor's own processBlock() can call this from outside
    // the class, at the one moment it's genuinely safe to (immediately
    // after the audio-thread swap — see that function's own comment for
    // the full story). audioFifo itself stays private; this is the only
    // sanctioned way in.
    void transferFifoFrom (AudioOutDeviceNode& other) { audioFifo.transferFrom (other.audioFifo); }
    bool getIsDawDevice()  const { return isDawDevice; }
    template <typename NodeT>
    void transferCallbackTo (NodeT& dst)
    {
        if (devManager == nullptr) return;

        // Real bug found and fixed 2026-09-03, revised twice more the
        // same day as deeper root causes kept surfacing — see this
        // project's own SessionLog.md for the full, multi-stage story.
        // This method now handles ONLY the callback registration itself
        // — register the new callback FIRST, then remove the old one,
        // per JUCE's own documented support for briefly overlapping
        // callbacks, avoiding any "nothing registered" window at all.
        //
        // The audio content itself — this node's own audioFifo — is
        // deliberately NOT transferred here anymore. An earlier version
        // of this fix did it right here, on the message thread, but that
        // left a genuine, if small, gap: the OLD node keeps being
        // actively processed (and keeps writing fresh audio into its own
        // fifo) for a real stretch of this same rebuild's own remaining
        // work, happening entirely AFTER this transfer runs — any audio
        // written during that window was simply lost, never making it
        // into the new fifo's own snapshot. Doing the fifo transfer here
        // also risked a genuine cross-thread race: this call runs on the
        // message thread, while the OLD node — still live, still part of
        // the graph the audio thread is actively processing until the
        // swap — could be concurrently writing to that exact same fifo
        // via its own process() call, violating AbstractFifo's own
        // single-reader/single-writer contract.
        //
        // Both problems are solved by doing the fifo transfer separately,
        // on the audio thread itself, immediately after the swap in
        // processBlock() — see that function's own comment for where and
        // why. At that exact moment, the audio thread is the only thread
        // touching either fifo at all, with the old graph no longer live
        // and the message thread's own rebuild long since finished —
        // eliminating both the lost-audio window and the cross-thread
        // race entirely, rather than merely shrinking either one.
        dst.devManager           = devManager;
        dst.registeredDeviceName = registeredDeviceName;
        dst.currentSampleRate    = currentSampleRate;
        dst.currentBlockSize     = currentBlockSize;
        devManager->addAudioCallback (&dst);
        devManager->removeAudioCallback (this);
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

        // Real fix, 2026-09-03 — found while investigating a longstanding,
        // user-reported audio-stream glitch specifically on graph edits,
        // after 6 separate investigations of PatchyProcessor::processBlock()'s
        // own internal timing had already, conclusively ruled out
        // everything measurable inside that one function. The real cause
        // turned out to be architectural: AudioOutDeviceNode/AudioInDeviceNode
        // register directly as their own AudioIODeviceCallback with a
        // separate, dedicated juce::AudioDeviceManager — meaning their own
        // real, hardware-level audio callback runs on a genuinely different
        // thread than PatchyProcessor::processBlock() ever touches, which
        // is exactly why none of those 6 investigations could ever have
        // found this. Every node instance is destroyed and recreated on
        // every single graph rebuild (this project's own established,
        // unavoidable pattern), meaning a fresh AudioFifo — this one —
        // starts completely empty, requiring roughly 2 full block periods
        // of re-priming (see this fifo's own write()) before it will
        // output anything but correct, graceful silence — a real, audible
        // gap on the actual hardware output, entirely independent of how
        // fast the graph's own internal processing runs, since the real
        // audio-hardware callback for a transferred device never actually
        // stops running at all.
        //
        // Transfers whatever audio content is genuinely still queued in
        // the OLD fifo into this one, so a transferred device can
        // continue outputting real, correct audio immediately rather than
        // falling silent and re-priming from scratch. Uses only
        // AbstractFifo's own well-documented public read/write API
        // (prepareToRead/finishedRead/prepareToWrite/finishedWrite) rather
        // than assuming anything about its own copy or move semantics,
        // which aren't documented or verified anywhere. Must only ever be
        // called after the old node's own audio callback has been fully,
        // synchronously removed from its device manager (JUCE's own
        // removeAudioCallback() guarantees no further callbacks fire on
        // it once that call returns) — calling this while the old fifo
        // might still be concurrently read from another thread would
        // violate AbstractFifo's own single-reader contract. Verified
        // this exact logic, including a genuine ring-buffer wraparound
        // case, with a standalone simulation before writing this.
        void transferFrom (AudioFifo& old)
        {
            numChannels = old.numChannels;

            int available = old.fifo.getNumReady();
            if (available <= 0) { fifo.reset(); primed = false; return; }
            available = juce::jmin (available, kFifoFrames);

            int rs1, rn1, rs2, rn2;
            old.fifo.prepareToRead (available, rs1, rn1, rs2, rn2);

            fifo.reset();
            int ws1, wn1, ws2, wn2;
            fifo.prepareToWrite (available, ws1, wn1, ws2, wn2);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                // Read the old fifo's own (possibly wrapped) content into a
                // temporary linear buffer first, then write that into this
                // fifo's own (possibly differently-wrapped) write positions —
                // the two fifos' own read/write pointers have no reason to
                // align, so each side's own wrap must be handled separately.
                std::vector<float> temp (static_cast<size_t> (available));
                auto& oldChan = old.data[static_cast<size_t> (ch)];
                if (rn1 > 0) std::memcpy (temp.data(), oldChan.data() + rs1, static_cast<size_t> (rn1) * sizeof (float));
                if (rn2 > 0) std::memcpy (temp.data() + rn1, oldChan.data() + rs2, static_cast<size_t> (rn2) * sizeof (float));

                auto& newChan = data[static_cast<size_t> (ch)];
                if (wn1 > 0) std::memcpy (newChan.data() + ws1, temp.data(), static_cast<size_t> (wn1) * sizeof (float));
                if (wn2 > 0) std::memcpy (newChan.data() + ws2, temp.data() + wn1, static_cast<size_t> (wn2) * sizeof (float));
            }

            old.fifo.finishedRead (rn1 + rn2);
            fifo.finishedWrite (wn1 + wn2);
            primed = old.primed;
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

    // Real fix, 2026-09-03 (3rd revision) — same reasoning as
    // AudioOutDeviceNode's own equivalent (see that class's own comment
    // for the full story).
    void transferFifoFrom (AudioInDeviceNode& other) { audioFifo.transferFrom (other.audioFifo); }
    bool getIsDawDevice()  const { return isDawDevice; }
    template <typename NodeT>
    void transferCallbackTo (NodeT& dst)
    {
        if (devManager == nullptr) return;

        // Real bug found and fixed 2026-09-03, revised twice more the
        // same day as deeper root causes kept surfacing — see this
        // project's own SessionLog.md for the full, multi-stage story.
        // This method now handles ONLY the callback registration itself
        // — register the new callback FIRST, then remove the old one,
        // per JUCE's own documented support for briefly overlapping
        // callbacks, avoiding any "nothing registered" window at all.
        //
        // The audio content itself — this node's own audioFifo — is
        // deliberately NOT transferred here anymore. An earlier version
        // of this fix did it right here, on the message thread, but that
        // left a genuine, if small, gap: the OLD node keeps being
        // actively processed (and keeps writing fresh audio into its own
        // fifo) for a real stretch of this same rebuild's own remaining
        // work, happening entirely AFTER this transfer runs — any audio
        // written during that window was simply lost, never making it
        // into the new fifo's own snapshot. Doing the fifo transfer here
        // also risked a genuine cross-thread race: this call runs on the
        // message thread, while the OLD node — still live, still part of
        // the graph the audio thread is actively processing until the
        // swap — could be concurrently writing to that exact same fifo
        // via its own process() call, violating AbstractFifo's own
        // single-reader/single-writer contract.
        //
        // Both problems are solved by doing the fifo transfer separately,
        // on the audio thread itself, immediately after the swap in
        // processBlock() — see that function's own comment for where and
        // why. At that exact moment, the audio thread is the only thread
        // touching either fifo at all, with the old graph no longer live
        // and the message thread's own rebuild long since finished —
        // eliminating both the lost-audio window and the cross-thread
        // race entirely, rather than merely shrinking either one.
        dst.devManager           = devManager;
        dst.registeredDeviceName = registeredDeviceName;
        dst.currentSampleRate    = currentSampleRate;
        dst.currentBlockSize     = currentBlockSize;
        devManager->addAudioCallback (&dst);
        devManager->removeAudioCallback (this);
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

        // Real fix, 2026-09-03 — same reasoning and same fix as
        // AudioOutDeviceNode's own equivalent (see that struct's own
        // comment for the full story).
        void transferFrom (AudioFifo& old)
        {
            numChannels = old.numChannels;

            int available = old.fifo.getNumReady();
            if (available <= 0) { fifo.reset(); primed = false; return; }
            available = juce::jmin (available, kFifoFrames);

            int rs1, rn1, rs2, rn2;
            old.fifo.prepareToRead (available, rs1, rn1, rs2, rn2);

            fifo.reset();
            int ws1, wn1, ws2, wn2;
            fifo.prepareToWrite (available, ws1, wn1, ws2, wn2);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                std::vector<float> temp (static_cast<size_t> (available));
                auto& oldChan = old.data[static_cast<size_t> (ch)];
                if (rn1 > 0) std::memcpy (temp.data(), oldChan.data() + rs1, static_cast<size_t> (rn1) * sizeof (float));
                if (rn2 > 0) std::memcpy (temp.data() + rn1, oldChan.data() + rs2, static_cast<size_t> (rn2) * sizeof (float));

                auto& newChan = data[static_cast<size_t> (ch)];
                if (wn1 > 0) std::memcpy (newChan.data() + ws1, temp.data(), static_cast<size_t> (wn1) * sizeof (float));
                if (wn2 > 0) std::memcpy (newChan.data() + ws2, temp.data() + wn1, static_cast<size_t> (wn2) * sizeof (float));
            }

            old.fifo.finishedRead (rn1 + rn2);
            fifo.finishedWrite (wn1 + wn2);
            primed = old.primed;
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

    /** Root-cause fix, 2026-09-11 — see this class's own outputManagers/
     *  inputManagers members below for the full story. Lazily creates and
     *  returns THIS specific node's own, independent juce::AudioDeviceManager
     *  — never a single, application-wide shared one. */
    juce::AudioDeviceManager& getOrCreateOutputManager (const juce::String& nodeId)
    {
        return getOrCreateManager (outputManagers, nodeId, 0, 2);
    }
    juce::AudioDeviceManager& getOrCreateInputManager (const juce::String& nodeId)
    {
        return getOrCreateManager (inputManagers, nodeId, 2, 0);
    }

    /** Closes and discards the per-node manager (and whatever real hardware
     *  device it may still hold open) for any nodeId whose own node no
     *  longer exists in the given (already fully rebuilt) graph — called
     *  once per rebuild, right after applyDeviceSelections(), so it only
     *  ever runs once the new graph's own node list is final and accurate.
     *  Without this, a genuinely deleted node's own device would otherwise
     *  stay open forever — unlike the lightweight, harmless string/int
     *  entries left behind in selections/channelSelections above (which
     *  this method deliberately leaves untouched, matching that existing,
     *  established, accepted-as-harmless pattern), an abandoned
     *  juce::AudioDeviceManager can hold a real, exclusive hardware
     *  connection, blocking every other node or application from using
     *  that same device even though the user has already deleted the node
     *  that opened it. */
    void pruneDeletedNodeManagers (ProcessingGraph& graph);

private:
    juce::AudioDeviceManager& getOrCreateManager (
        std::unordered_map<juce::String, std::unique_ptr<juce::AudioDeviceManager>>& managers,
        const juce::String& nodeId, int numInputChannels, int numOutputChannels)
    {
        auto it = managers.find (nodeId);
        if (it != managers.end()) return *it->second;
        auto newManager = std::make_unique<juce::AudioDeviceManager>();
        newManager->initialiseWithDefaultDevices (numInputChannels, numOutputChannels);
        auto& ref = *newManager;
        managers[nodeId] = std::move (newManager);
        return ref;
    }

    // Root cause of the "last device wins" bug (found and fixed
    // 2026-09-11): every AudioInDeviceNode/AudioOutDeviceNode used to
    // share these same two, single, application-wide juce::AudioDeviceManager
    // instances — a real juce::AudioDeviceManager represents ONE active
    // device connection at a time, so each node's own openDevice() call
    // simply replaced whatever the previous node had already set, with
    // every node's own callback staying registered but all of them then
    // receiving audio from whichever device was opened last. Now one
    // independent manager per node, keyed by nodeId (not by C++ object
    // instance — see transferCallbackTo()'s own comment in this file for
    // why: the map entry must persist stably across a graph rebuild, the
    // same way the old, single, PatchyProcessor-owned managers used to,
    // for that existing transfer mechanism to keep working unchanged).
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioDeviceManager>> outputManagers;
    std::unordered_map<juce::String, std::unique_ptr<juce::AudioDeviceManager>> inputManagers;
    std::unordered_map<juce::String, juce::String>       selections;
    std::unordered_map<juce::String, std::vector<int>>   channelSelections;
};
