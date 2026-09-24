#pragma once
#include <juce_audio_devices/juce_audio_devices.h>
#include "NodeProcessor.h"
#include "GraphModel.h"
#include "StartupFadeRegistry.h"
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
        // [Superseded 2026-09-24 by the shared-fifo fix further down —
        // the paragraphs below are kept for history.]
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
        // Shared-fifo fix, 2026-09-24 — the new node takes over THIS
        // node's own fifo (shared, not copied) before its callback is
        // registered. From here until processBlock()'s swap, the device
        // talks to the new node while the graph still runs this old one —
        // with separate fifos that window was a real gap (the device read
        // an empty fifo / wrote into one nobody read). Sharing one fifo
        // makes the handover seamless by construction: old graph and
        // device keep meeting in the same fifo, and after the swap the new
        // node simply carries on in it. Still single-producer/single-
        // consumer: the graph side is one audio thread processing either
        // the old node or (after the swap) the new one, never both.
        // Replaces the old swap-time transferFifoFrom() copy, which
        // allocated on the audio thread and was immediately wiped by the
        // swap's own prepare() anyway.
        dst.audioFifo = audioFifo;
        devManager->addAudioCallback (&dst);
        // Shared-fifo fix, 2026-09-24 — JUCE may call both callbacks for
        // a block while they overlap (add-then-remove, above/below). With
        // one shared fifo, the old callback must stop touching it the
        // moment the new one is live, or that block would be consumed
        // (Out) or written (In) twice.
        callbackHandedOver.store (true, std::memory_order_release);
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

        // Shared-fifo fix, 2026-09-24 — see transferCallbackTo()'s own
        // comment. A fifo handed over from an old node (same device, same
        // audio config) must keep its content and state: resetting it here
        // was exactly what wiped the audio on every graph edit. Only a real
        // config change (sample rate or block size — the same criterion as
        // ProcessingGraph::hasSameAudioConfigAs()) still resets it, keeping
        // the 2026-09-18 "no stale content across a buffer/rate change"
        // behaviour. A fresh node's fifo starts unconfigured, so it always
        // resets on its first prepare, exactly as before.
        void prepareFor (int numPhysical, int sampleRate, int blockSize)
        {
            if (sampleRate == configuredRate && blockSize == configuredBlock)
                return;
            reset (numPhysical, sampleRate);
            configuredRate  = sampleRate;
            configuredBlock = blockSize;
        }
        int configuredRate  = 0;
        int configuredBlock = 0;

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
            lastWriteFrames.store (numFrames, std::memory_order_relaxed);
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

            // Real fix, 2026-09-23 — buffer-size/sample-rate lag. read()
            // only ever consumes exactly numFrames, so any surplus that
            // builds up in this fifo (a skipped read, a burst around a
            // graph swap, two independent hardware clocks drifting apart)
            // was never discarded — it became permanent extra latency,
            // up to the full 8192-frame capacity (~170-186 ms per fifo),
            // clearable only by a restart. Trims the fifo back to a safe
            // target whenever its fill exceeds a limit. Wide hysteresis
            // (limit = target + one full block of the larger side) so it
            // only fires on genuine standing surplus, never on normal
            // reader/writer block-size jitter. Discarding is a read-side
            // operation, so this stays within AbstractFifo's own
            // single-reader contract.
            {
                const int writer = juce::jmax (1, lastWriteFrames.load (std::memory_order_relaxed));
                const int target = numFrames + 2 * writer;
                const int limit  = target + juce::jmax (numFrames, writer);
                const int ready  = fifo.getNumReady();
                if (ready > limit)
                {
                    int t1, tn1, t2, tn2;
                    fifo.prepareToRead (ready - target, t1, tn1, t2, tn2);
                    fifo.finishedRead (tn1 + tn2);
                }
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
        // Real fix, 2026-09-23 — see read()'s own trim comment.
        // lastWriteFrames: writer's most recent block size (writer thread
        // stores, reader thread loads).
        std::atomic<int> lastWriteFrames { 0 };
    };
    // Shared-fifo fix, 2026-09-24: held by shared_ptr so an old and a new
    // node for the same device can share ONE fifo across a graph rebuild
    // (see transferCallbackTo()). Only ever reassigned in
    // transferCallbackTo(), on the message thread, before the new node's
    // callback is registered and before the new node is ever processed.
    std::shared_ptr<AudioFifo> audioFifo { std::make_shared<AudioFifo>() };
    // Shared-fifo fix, 2026-09-24 — set in transferCallbackTo() once the
    // new node's callback is registered; this node's own device callback
    // then leaves the shared fifo alone (see that function's comment).
    std::atomic<bool> callbackHandedOver { false };

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

        // Real bug found and fixed 2026-09-03, revised twice more the
        // same day as deeper root causes kept surfacing — see this
        // project's own SessionLog.md for the full, multi-stage story.
        // This method now handles ONLY the callback registration itself
        // — register the new callback FIRST, then remove the old one,
        // per JUCE's own documented support for briefly overlapping
        // callbacks, avoiding any "nothing registered" window at all.
        //
        // [Superseded 2026-09-24 by the shared-fifo fix further down —
        // the paragraphs below are kept for history.]
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
        // Fix, 2026-09-24 — a fade-in armed by openDevice() but not yet
        // started (no real audio read yet) must follow the device to its
        // new node, or a rebuild landing in that window would skip it.
        dst.fadeInMuteMs.store (fadeInMuteMs.load (std::memory_order_relaxed), std::memory_order_relaxed);
        dst.fadeInArmed.store (fadeInArmed.load (std::memory_order_relaxed), std::memory_order_relaxed);
        // Shared-fifo fix, 2026-09-24 — same as AudioOutDeviceNode's own
        // transferCallbackTo(); see its comment.
        dst.audioFifo = audioFifo;
        devManager->addAudioCallback (&dst);
        // Shared-fifo fix, 2026-09-24 — JUCE may call both callbacks for
        // a block while they overlap (add-then-remove, above/below). With
        // one shared fifo, the old callback must stop touching it the
        // moment the new one is live, or that block would be consumed
        // (Out) or written (In) twice.
        callbackHandedOver.store (true, std::memory_order_release);
        devManager->removeAudioCallback (this);
        devManager = nullptr; registeredDeviceName = {};
    }

    // NodeProcessor
    void prepare (double sampleRate, int maxBlockSize) override;
    void process (int numSamples) override;

    // Real bug found 2026-09-15 (same root cause as MidiInDeviceNode's own
    // identical fix — see that class's own process() comment for the full
    // story): the hardware audio callback below keeps writing into
    // audioFifo regardless of `disabled`, entirely independent of whether
    // process() is being called. Skipping process() entirely while
    // disabled (this project's default "cut" behaviour) left audioFifo's
    // own 8192-frame ring buffer silently filling with real, unread audio
    // the whole time this node stayed disabled — capped at ~186ms of
    // backlog (juce::AbstractFifo never overwrites unread data), but
    // never self-draining either, so the very next process() call after
    // re-enabling would start reading audio from before this node was
    // disabled, at a de-synced ~186ms delay that never recovers on its
    // own. Fixed by keeping process() running unconditionally (still
    // draining audioFifo, so it never backlogs at all) while disabled,
    // clearing outputAudio instead of populating it from the fifo — see
    // process()'s own implementation for the actual logic.
    bool passesThroughWhenDisabled() const override { return true; }

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

        // Shared-fifo fix, 2026-09-24 — see transferCallbackTo()'s own
        // comment. A fifo handed over from an old node (same device, same
        // audio config) must keep its content and state: resetting it here
        // was exactly what wiped the audio on every graph edit. Only a real
        // config change (sample rate or block size — the same criterion as
        // ProcessingGraph::hasSameAudioConfigAs()) still resets it, keeping
        // the 2026-09-18 "no stale content across a buffer/rate change"
        // behaviour. A fresh node's fifo starts unconfigured, so it always
        // resets on its first prepare, exactly as before.
        void prepareFor (int numPhysical, int sampleRate, int blockSize)
        {
            if (sampleRate == configuredRate && blockSize == configuredBlock)
                return;
            reset (numPhysical, sampleRate);
            configuredRate  = sampleRate;
            configuredBlock = blockSize;
        }
        int configuredRate  = 0;
        int configuredBlock = 0;

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
            lastWriteFrames.store (numFrames, std::memory_order_relaxed);
        }

        // Read FIFO slots into contiguous dst channels 0, 1, …
        bool read (juce::AudioBuffer<float>& dst, int numFrames)   // true = real audio delivered (FCA1616 fade-in fix, 2026-09-24)
        {
            dst.clear();
            if (fifo.getNumReady() < numFrames)
                return false;

            // Real fix, 2026-09-23 — buffer-size/sample-rate lag. read()
            // only ever consumes exactly numFrames, so any surplus that
            // builds up in this fifo (a skipped read, a burst around a
            // graph swap, two independent hardware clocks drifting apart)
            // was never discarded — it became permanent extra latency,
            // up to the full 8192-frame capacity (~170-186 ms per fifo),
            // clearable only by a restart. Trims the fifo back to a safe
            // target whenever its fill exceeds a limit. Wide hysteresis
            // (limit = target + one full block of the larger side) so it
            // only fires on genuine standing surplus, never on normal
            // reader/writer block-size jitter. Discarding is a read-side
            // operation, so this stays within AbstractFifo's own
            // single-reader contract.
            {
                const int writer = juce::jmax (1, lastWriteFrames.load (std::memory_order_relaxed));
                const int target = numFrames + 2 * writer;
                const int limit  = target + juce::jmax (numFrames, writer);
                const int ready  = fifo.getNumReady();
                if (ready > limit)
                {
                    int t1, tn1, t2, tn2;
                    fifo.prepareToRead (ready - target, t1, tn1, t2, tn2);
                    fifo.finishedRead (tn1 + tn2);
                }
            }

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
            return true;
        }

        juce::AbstractFifo              fifo { kFifoFrames };
        std::vector<std::vector<float>> data;   // kMaxFifoChans × kFifoFrames, fixed after ctor
        int  numChannels = 2;
        bool primed      = false;
        // Real fix, 2026-09-23 — see read()'s own trim comment.
        // lastWriteFrames: writer's most recent block size (writer thread
        // stores, reader thread loads).
        std::atomic<int> lastWriteFrames { 0 };
    };
    // Shared-fifo fix, 2026-09-24: held by shared_ptr so an old and a new
    // node for the same device can share ONE fifo across a graph rebuild
    // (see transferCallbackTo()). Only ever reassigned in
    // transferCallbackTo(), on the message thread, before the new node's
    // callback is registered and before the new node is ever processed.
    std::shared_ptr<AudioFifo> audioFifo { std::make_shared<AudioFifo>() };
    // Shared-fifo fix, 2026-09-24 — set in transferCallbackTo() once the
    // new node's callback is registered; this node's own device callback
    // then leaves the shared fifo alone (see that function's comment).
    std::atomic<bool> callbackHandedOver { false };

    juce::AudioDeviceManager* devManager = nullptr;
    juce::String              registeredDeviceName;
    bool                      transferred = false;
    bool                      isDawDevice = false;

    // Fix, 2026-09-24 — open-time click. Some interfaces (confirmed: the
    // Behringer FCA1616) emit a sharp pop in their own input stream when it
    // starts — audible even on the interface's own headphone out, so it's
    // hardware-side. Patchy can't stop it at the source, but can keep it
    // out of its own audio path: after each FRESH open (never after a mere
    // transfer between graphs, which doesn't restart the stream), the first
    // real audio read is held silent for fadeInMuteMs, then ramped
    // up linearly over kFadeInRampSeconds. Counting starts at the first
    // successful read, not at open, so a slow-starting device can't
    // silently use up the window before its pop arrives. fadeInArmed is
    // set on the message thread (openDevice) and consumed on the audio
    // thread (process); fadeInPos is audio-thread-only (-1 = inactive).
    // Measured 2026-09-24 (pop-timing diagnostic): the FCA1616's pop lands
    // ~987 ms after the first audio (peak 0.914), a second spike at ~1045 ms
    // (0.878), then a decaying tail (0.053 -> 0.010 by ~1335 ms) — likely
    // the interface unmuting its inputs ~1 s after the stream starts. The
    // mute covers both spikes with margin; the ramp swallows the tail.
    //
    // Targeted, 2026-09-24: the fade is now only armed for devices listed
    // in StartupFadeRegistry (see that header), and the mute duration comes
    // from the registry per device (FCA1616's measured default: 1250 ms)
    // instead of a fixed constant. The ramp stays fixed — it only smooths
    // the return. fadeInMuteMs is set together with fadeInArmed in
    // openDevice() (message thread) and read when the fade starts in
    // process() (audio thread).
    static constexpr double kFadeInRampSeconds = 0.150;
    std::atomic<int>          fadeInMuteMs { 0 };
    std::atomic<bool>         fadeInArmed { false };
    int                       fadeInPos   = -1;

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
