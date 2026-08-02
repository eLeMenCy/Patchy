#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <unordered_map>
#include "../Pax/PaxAPI.h"

/**
 * NodeProcessor — base class for all processing nodes.
 *
 * Each node owns its own input/output audio and MIDI buffers.
 * The ProcessingGraph copies data between connected nodes before
 * calling process() on each node in topological order.
 *
 * All audio/MIDI processing is currently STUB (pass-through / silence).
 * Replace the process() bodies with real DSP when ready.
 */
class NodeProcessor
{
public:
    // Lightweight activity counter — incremented on audio thread, read+reset on message thread
    std::atomic<int> midiEventsSinceLastPoll { 0 };
    void recordMidiActivity (int count) { midiEventsSinceLastPoll.fetch_add (count, std::memory_order_relaxed); }
    int  drainMidiActivity()            { return midiEventsSinceLastPoll.exchange (0, std::memory_order_relaxed); }

    enum class Type { Midi = 1, Audio = 2, AV = 3 };

    explicit NodeProcessor (const juce::String& nodeId, Type type)
        : id (nodeId), nodeType (type) {}

    virtual ~NodeProcessor() = default;

    //─────────────────────────────────────────────────────────────────────────
    virtual void prepare (double sampleRate, int maxBlockSize)
    {
        currentSampleRate  = sampleRate;
        currentBlockSize   = maxBlockSize;

        // Resize primary buffers (used by built-in nodes)
        inputAudio.setSize  (2, maxBlockSize, false, true, true);
        outputAudio.setSize (2, maxBlockSize, false, true, true);
        inputMidi.clear();
        outputMidi.clear();

        // Resize per-port buffers for addon nodes with variable port counts
        for (auto& buf : inputAudioBuffers)
            buf.setSize (2, maxBlockSize, false, true, true);
        for (auto& buf : outputAudioBuffers)
            buf.setSize (2, maxBlockSize, false, true, true);
    }

    void resetBuffers (int numSamples)
    {
        inputAudio.clear();
        outputAudio.clear();
        inputMidi.clear();
        outputMidi.clear();
        lastNumSamples = numSamples;

        for (auto& buf : inputAudioBuffers)  buf.clear();
        for (auto& buf : outputAudioBuffers) buf.clear();

        // Reset value buffers — clear events from previous block
        inputValueCount  = 0;
        outputValueCount = 0;

        // Reset DMX frame valid flags — a stale frame byte array left over
        // from a previous block is harmless as long as the valid flag says
        // not to trust it; the flag is the actual reset, not the bytes.
        inputDmxFrameValid  = false;
        outputDmxFrameValid = false;
    }

    /** Allocate per-port audio buffers (called when port count is known). */
    void allocatePortBuffers (int numAudioIn, int numAudioOut, int maxBlockSize)
    {
        inputAudioBuffers.resize  (static_cast<size_t>(numAudioIn));
        outputAudioBuffers.resize (static_cast<size_t>(numAudioOut));
        for (auto& buf : inputAudioBuffers)
            buf.setSize (2, maxBlockSize, false, true, true);
        for (auto& buf : outputAudioBuffers)
            buf.setSize (2, maxBlockSize, false, true, true);
    }

    virtual void process (int numSamples) = 0;

    juce::AudioBuffer<float> inputAudio,  outputAudio;   // single-port (built-ins)
    juce::MidiBuffer         inputMidi,   outputMidi;

    // Multi-port audio buffers (Pax nodes with variable port counts)
    std::vector<juce::AudioBuffer<float>> inputAudioBuffers;
    std::vector<juce::AudioBuffer<float>> outputAudioBuffers;

    // ── Value buffers (PAX_Value — DMX, OSC, MQTT, UDP etc.) ─────────────────
    static constexpr int kMaxValueEvents = 256;
    std::array<PAX_Value, kMaxValueEvents> inputValues  {};
    std::array<PAX_Value, kMaxValueEvents> outputValues {};
    int inputValueCount  = 0;
    int outputValueCount = 0;

    // ── DMX universe buffer — separate wide-payload path (PaxAPI.h v4) ───────
    // PAX_Value.data[] (56 bytes) can't carry a full 512-channel universe;
    // this is a parallel, purpose-built path so DMX nodes (built-in and Pax)
    // aren't squeezed through that cap. Single slot per direction — matches
    // today's one-DMX-port-per-node reality (see DmxDeviceNodes.h's own
    // "single universe per device" convention); can grow to an array keyed
    // by port index later if a node ever needs more than one DMX port.
    std::array<uint8_t, 512> inputDmxFrame  {};
    std::array<uint8_t, 512> outputDmxFrame {};
    bool inputDmxFrameValid  = false;
    bool outputDmxFrameValid = false;

    // Per-source DMX frame cache, keyed by the upstream node's own id.
    // Needed for correct HTP merging across multiple sources feeding one
    // destination (see ProcessingGraph.cpp's routing) — some sources emit
    // a fresh frame every single block (e.g. AudioToDmxPax, audio-rate),
    // others only on an actual change (e.g. DmxConsoleNode, discrete
    // fader moves). Without this cache, merging only "whatever's valid
    // this exact block" meant a discrete source's contribution vanished
    // the instant a continuously-emitting source re-initialised the merge
    // on the very next block — a real bug found via DAW testing: moving a
    // console fader flashed the new value for one block, then it was
    // immediately overridden back to 0. Deliberately NOT cleared by
    // resetBuffers() — the whole point is to persist between a source's
    // own emission events; ProcessingGraph.cpp prunes entries whose
    // source is no longer actually connected, each block, so a
    // disconnected source's last value doesn't linger forever.
    std::unordered_map<juce::String, std::array<uint8_t, 512>> dmxSourceFrames;

    const juce::String id;
    const Type         nodeType;

protected:
    double currentSampleRate = 44100.0;
    int    currentBlockSize  = 512;
    int    lastNumSamples    = 0;
};

// Built-in stub node types removed — all built-in nodes are now device nodes.
// Dynamic addon nodes use DynamicPaxProcessor (see PaxRegistry.h).
