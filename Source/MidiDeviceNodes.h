#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "NodeProcessor.h"

// Forward declare to avoid circular include (ProcessingGraph includes NodeProcessor
// which would then include ProcessingGraph again)
class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MidiOutDeviceNode  (nodeType 4)
 *
 * 1 MIDI input port — receives MIDI from upstream graph nodes.
 * Sends whatever arrives on inputMidi to the selected physical MIDI OUT port.
 */
class MidiOutDeviceNode : public NodeProcessor
{
public:
    explicit MidiOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi) {}

    ~MidiOutDeviceNode() override { closeDevice(); }

    // Real, genuine data race found and fixed 2026-09-02 — process() below
    // runs on the audio thread and directly dereferences output every
    // single block; openDevice()/closeDevice() run on the message thread
    // and reassign/reset that same output, completely unsynchronized —
    // with no lock at all, this is undefined behaviour, not merely
    // inefficient, unlike this project's own comparable DMX/ArtNet fixes
    // this same week, which addressed a background-thread race rather
    // than a direct audio-thread one. Found while investigating a user
    // report of a longstanding, general audio-stream glitch on any graph
    // edit — this node's own openDevice() was also confirmed, via the
    // user's own real hardware log, to unconditionally reopen on every
    // single graph rebuild, anywhere, just like DMX's own equivalent bug
    // — meaning this exact race fires on every single graph edit whenever
    // a MIDI Out Device is configured, not just when its own selection
    // genuinely changes. Uses this project's own already-established
    // juce::SpinLock pattern (see AudioOutDeviceNode's own channelLock)
    // rather than introducing a new synchronization approach — a spin
    // lock's own critical section here is extremely short (a pointer
    // read/reassignment), keeping this real-time safe for the audio
    // thread's own side.
    void openDevice (const juce::String& deviceIdentifier)
    {
        closeDevice();
        selectedDeviceId = deviceIdentifier;
        if (deviceIdentifier.isEmpty()) return;

        auto newOutput = juce::MidiOutput::openDevice (deviceIdentifier);
        if (newOutput)
            juce::Logger::writeToLog ("MidiOutDeviceNode: opened " + deviceIdentifier);
        else
            juce::Logger::writeToLog ("MidiOutDeviceNode: failed to open " + deviceIdentifier);

        juce::SpinLock::ScopedLockType sl (outputLock);
        output = std::move (newOutput);
    }

    void closeDevice()
    {
        std::unique_ptr<juce::MidiOutput> old;
        {
            juce::SpinLock::ScopedLockType sl (outputLock);
            old = std::move (output);
        }
        if (old) old->stopBackgroundThread();
    }

    // Real fix, 2026-09-02 — same reasoning as DmxIn/OutDeviceNode's own
    // transferOrConfigure() (see DmxDeviceNodes.h's own comment for the
    // full story): ProcessingGraph::rebuild() destroys and recreates
    // every node instance on every graph edit, anywhere, and this node's
    // own openDevice() was unconditionally closing and reopening its own
    // MIDI connection on every single one — confirmed directly via the
    // user's own real hardware log ("MidiOutDeviceNode: opened..."
    // repeating on every graph action). Reuses an already-open connection
    // when the device selection hasn't genuinely changed, transferring
    // ownership of the juce::MidiOutput itself (a plain std::unique_ptr,
    // simpler here than DMX's own SerialPort — no custom transfer method
    // needed, and no separate background thread of this node's own to
    // stop first, since juce::MidiOutput manages its own internal
    // background thread independently of this node).
    bool transferOrConfigure (const juce::String& deviceIdentifier, MidiOutDeviceNode* oldNode)
    {
        if (oldNode != nullptr && oldNode->selectedDeviceId == deviceIdentifier)
        {
            std::unique_ptr<juce::MidiOutput> transferred;
            {
                juce::SpinLock::ScopedLockType oldLock (oldNode->outputLock);
                transferred = std::move (oldNode->output);
            }
            if (transferred != nullptr)
            {
                juce::SpinLock::ScopedLockType sl (outputLock);
                output = std::move (transferred);
                selectedDeviceId = deviceIdentifier;
                return true;
            }
        }

        openDevice (deviceIdentifier);
        return false;
    }

    void process (int /*numSamples*/) override
    {
        // Pass MIDI through so downstream nodes can still use it
        outputMidi = inputMidi;
        if (inputMidi.getNumEvents() > 0)
            recordMidiActivity (inputMidi.getNumEvents());

        // Send to the physical device
        juce::SpinLock::ScopedLockType sl (outputLock);
        if (output)
            for (const auto& meta : inputMidi)
                output->sendMessageNow (meta.getMessage());
    }

    juce::String selectedDeviceId;

private:
    std::unique_ptr<juce::MidiOutput> output;
    juce::SpinLock outputLock;   // protects output from the message-thread/audio-thread race above
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiOutDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MidiInDeviceNode  (nodeType 5)
 *
 * 1 MIDI output port — feeds MIDI to downstream graph nodes.
 * Receives MIDI asynchronously from a physical MIDI IN port via a lock-free FIFO.
 */
class MidiInDeviceNode : public NodeProcessor,
                          private juce::MidiInputCallback
{
public:
    explicit MidiInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi) {}

    ~MidiInDeviceNode() override { closeDevice(); }

    void openDevice (const juce::String& deviceIdentifier)
    {
        // Deliberately NOT given the same transfer-based fix as
        // MidiOutDeviceNode's own equivalent (2026-09-02) — juce::MidiInput
        // registers its own callback target (the `this` pointer passed to
        // openDevice() below) at open time; transferring an already-open
        // connection to a new node instance would leave it still calling
        // back into the OLD instance's own handleIncomingMidiMessage(),
        // not the new one — a genuine use-after-free once the old instance
        // is later destroyed via graphTrash, not something to introduce
        // without first confirming juce::MidiInput supports re-targeting
        // its own callback, which nothing here establishes. This node's
        // own process() never touches `input` directly (it only drains a
        // thread-safe FIFO populated by that callback), so the reopen
        // itself lacks the direct audio-thread data race
        // MidiOutDeviceNode's own equivalent had — a real, known
        // remaining inefficiency (unconditional reopen on every graph
        // edit, anywhere), not a correctness bug.
        closeDevice();
        selectedDeviceId = deviceIdentifier;
        // Look up human-readable name from the identifier
        selectedDeviceName = "";
        for (auto& d : juce::MidiInput::getAvailableDevices())
            if (d.identifier == deviceIdentifier) { selectedDeviceName = d.name; break; }

        if (deviceIdentifier.isEmpty()) return;

        input = juce::MidiInput::openDevice (deviceIdentifier, this);
        if (input)
        {
            input->start();
            juce::Logger::writeToLog ("MidiInDeviceNode: opened " + selectedDeviceName);
        }
        else
        {
            juce::Logger::writeToLog ("MidiInDeviceNode: failed to open " + deviceIdentifier);
        }
    }

    void closeDevice()
    {
        if (input) { input->stop(); input.reset(); }
    }

    void process (int /*numSamples*/) override
    {
        outputMidi.clear();
        juce::MidiMessage msg;
        int samplePos = 0;
        while (fifo.pop (msg))
            outputMidi.addEvent (msg, samplePos++);
    }

    juce::String selectedDeviceId;
    juce::String selectedDeviceName;   // human-readable, for MidiMonitor source info

private:
    void handleIncomingMidiMessage (juce::MidiInput*,
                                    const juce::MidiMessage& msg) override
    {
        fifo.push (msg);
        recordMidiActivity (1);
    }

    std::unique_ptr<juce::MidiInput> input;

    static constexpr int kFifoSize = 512;

    struct MidiFifo
    {
        void push (const juce::MidiMessage& msg)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) messages[static_cast<size_t>(s1)] = msg;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (juce::MidiMessage& msg)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            msg = messages[static_cast<size_t>(s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        juce::AbstractFifo                       fifo { kFifoSize };
        std::array<juce::MidiMessage, kFifoSize> messages;
    } fifo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiInDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MidiDeviceManager
 *
 * Owned by PatchyProcessor. Tracks device selections and (re)opens devices
 * after ProcessingGraph rebuilds. Does NOT own the nodes — ProcessingGraph does.
 */
class MidiDeviceManager
{
public:
    /** Store a selection without opening the device yet. */
    void storeSelection (const juce::String& nodeId,
                         const juce::String& deviceIdentifier)
    {
        selections[nodeId] = deviceIdentifier;
    }

    /** Try to open the device on the given node in the given graph.
     *  Returns true if the node was found. */
    bool applyToGraph (const juce::String& nodeId,
                       const juce::String& deviceIdentifier,
                       ProcessingGraph&    graph,
                       ProcessingGraph*    oldGraph = nullptr);

    /** Re-open all saved selections on a freshly rebuilt graph. */
    void applyDeviceSelections (ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);

    /** Convenience: store + apply in one call. */
    void setDevice (const juce::String& nodeId,
                    const juce::String& deviceIdentifier,
                    ProcessingGraph&    graph)
    {
        storeSelection (nodeId, deviceIdentifier);
        applyToGraph   (nodeId, deviceIdentifier, graph);
    }

    /** Query system MIDI devices for the UI comboboxes. */
    static juce::var getAvailableDevicesVar();

private:
    std::unordered_map<juce::String, juce::String> selections;
};
