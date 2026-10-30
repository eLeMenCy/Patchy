#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "NodeProcessor.h"
#include "VirtualMidiPorts.h"   // v0.0.960
#include <atomic>
#include <cstdint>
#include <array>
#include <bitset>

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

        // v0.0.919 (2026-10-02) — "DAW" (plugin builds): no hardware port;
        // ProcessingGraph::process() hands this node's MIDI back to the host
        // instead (same idea as the Audio Out "DAW" device).
        // v0.0.960 — "virtual:X": no hardware; PatchyProcessor hands the
        // node its persistent virtual port (setVirtualPort) after this
        if (deviceIdentifier.startsWith ("virtual"))
        {
            juce::Logger::writeToLog ("MidiOutDeviceNode: virtual port " + deviceIdentifier);
            return;
        }
        if (deviceIdentifier == "DAW")
        {
            dawOutput.store (true, std::memory_order_relaxed);
            juce::Logger::writeToLog ("MidiOutDeviceNode: sending to the DAW");
            return;
        }

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
        virtualOut.store (nullptr, std::memory_order_relaxed);   // v0.0.960
        dawOutput.store (false, std::memory_order_relaxed);   // v0.0.919
        std::unique_ptr<juce::MidiOutput> old;
        {
            juce::SpinLock::ScopedLockType sl (outputLock);
            old = std::move (output);
        }
        if (old)
        {
            // v0.0.954 — quitting (or switching device) while notes sound:
            // release them on the device first, or they stay stuck on the synth
            for (int c = 0; c < 16; ++c)
            {
                if (held[(size_t) c].none()) continue;
                for (int n = 0; n < 128; ++n)
                    if (held[(size_t) c].test ((size_t) n)) old->sendMessageNow (juce::MidiMessage::noteOff (c + 1, n));
                old->sendMessageNow (juce::MidiMessage::controllerEvent (c + 1, 64, 0));
            }
            old->stopBackgroundThread();
        }
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

        // v0.0.947 — Learn (channel filter): while armed, nothing goes to the
        // device (the learned note isn't played); downstream still gets it.
        if (captureLearn (inputMidi)) return;

        // v0.0.948 — notes sent and not yet released, for the panic's explicit
        // Note Offs (many synths ignore All Notes Off / All Sound Off)
        for (const auto meta : inputMidi)
        {
            const auto m = meta.getMessage();
            const int  c = m.getChannel() - 1;
            if (c < 0 || c > 15) continue;
            if (m.isNoteOn())                                   held[(size_t) c].set   ((size_t) m.getNoteNumber());
            else if (m.isNoteOff())                             held[(size_t) c].reset ((size_t) m.getNoteNumber());
            else if (m.isAllNotesOff() || m.isAllSoundOff())    held[(size_t) c].reset();
        }

        // Send to the physical device — gated by the channel filter.
        // 0 = omni (every channel sent, matching this node's own default/
        // original behaviour before this feature existed). A channel-less
        // message (sysex etc, getChannel()==0) always passes through,
        // matching MIDI CH. MATRIX's own established convention: it was
        // never addressable by a per-channel filter in the first place.
        const std::uint16_t mask = channelFilter.load (std::memory_order_relaxed);
        // v0.0.960 — a virtual port: the other apps get it
        if (auto* vp = virtualOut.load (std::memory_order_relaxed))
            for (const auto& meta : inputMidi)
            {
                const auto msg = meta.getMessage();
                const int  ch  = msg.getChannel();
                if (mask == 0 || ch == 0 || (mask & static_cast<std::uint16_t> (1u << (ch - 1))) != 0)
                    vp->send (msg);
            }
        juce::SpinLock::ScopedLockType sl (outputLock);
        if (output)
            for (const auto& meta : inputMidi)
            {
                const auto msg = meta.getMessage();
                const int  ch  = msg.getChannel();
                if (mask == 0 || ch == 0 || (mask & static_cast<std::uint16_t> (1u << (ch - 1))) != 0)
                    output->sendMessageNow (msg);
            }
    }

    // Live-dispatch accessor pair, same reasoning as every other node in
    // this project whose own settings genuinely drive real-time processing
    // (see this project's own established "commit alone doesn't reach a
    // running node" lesson) — bypasses the rebuild requirement entirely.
    void setChannelFilter (std::uint16_t mask) { channelFilter.store (mask, std::memory_order_relaxed); }

    /** v0.0.948 — Panic (audio thread, called by the graph before process):
        a Note Off for every note this node sent and hasn't released, then
        Sustain off, All Sound Off and All Notes Off on all 16 channels.
        Returns how many Note Offs were added. */
    int addPanic (juce::MidiBuffer& buf)
    {
        int offs = 0;
        for (int c = 0; c < 16; ++c)
        {
            for (int n = 0; n < 128; ++n)
                if (held[(size_t) c].test ((size_t) n)) { buf.addEvent (juce::MidiMessage::noteOff (c + 1, n), 0); ++offs; }
            buf.addEvent (juce::MidiMessage::controllerEvent (c + 1, 64, 0), 0);
            buf.addEvent (juce::MidiMessage::allSoundOff (c + 1), 0);
            buf.addEvent (juce::MidiMessage::allNotesOff (c + 1), 0);
        }
        return offs;
    }

    /** v0.0.919 — true when set to the "DAW" device: its MIDI goes back to
     *  the host (audio thread reads it; set on the message thread). */
    bool isDawOutput() const { return dawOutput.load (std::memory_order_relaxed); }
    std::uint16_t getChannelFilter() const     { return channelFilter.load (std::memory_order_relaxed); }

    juce::String selectedDeviceId;

    /** v0.0.960 — message thread (PatchyProcessor::syncVirtualMidiPorts). */
    void setVirtualPort (VirtualMidiOutPort* p) { virtualOut.store (p, std::memory_order_relaxed); }

private:
    std::atomic<VirtualMidiOutPort*> virtualOut { nullptr };   // v0.0.960 — owned by PatchyProcessor
    std::unique_ptr<juce::MidiOutput> output;
    std::atomic<bool>                 dawOutput { false };   // v0.0.919 — see isDawOutput()
    juce::SpinLock outputLock;   // protects output from the message-thread/audio-thread race above
    std::atomic<std::uint16_t> channelFilter { 0 }; // one bit per channel (1-16); 0 = omni
    std::array<std::bitset<128>, 16> held {};        // v0.0.948 — audio thread only (see process / addPanic)
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

        // v0.0.960 — "virtual:X": no hardware; the persistent virtual port
        // comes from PatchyProcessor (setVirtualPort) after this
        if (deviceIdentifier.startsWith ("virtual"))
        {
            juce::Logger::writeToLog ("MidiInDeviceNode: virtual port " + deviceIdentifier);
            return;
        }

        // v0.0.919 (2026-10-02) — "DAW" (plugin builds): no hardware port;
        // process() forwards the host's MIDI (track clips/regions, the
        // controller through the track), which ProcessingGraph hands this
        // node as inputMidi. Before, that host MIDI was silently ignored.
        if (deviceIdentifier == "DAW")
        {
            selectedDeviceName = "DAW";
            dawInput.store (true, std::memory_order_relaxed);
            juce::Logger::writeToLog ("MidiInDeviceNode: receiving from the DAW");
            return;
        }

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
        virtualIn.store (nullptr, std::memory_order_relaxed);   // v0.0.960
        dawInput.store (false, std::memory_order_relaxed);   // v0.0.919
        if (input) { input->stop(); input.reset(); }
    }

    void process (int /*numSamples*/) override
    {
        // Real bug found 2026-09-15, reported from the user's own studio:
        // the OS-driven MIDI input callback below (handleIncomingMidiMessage)
        // keeps running and keeps pushing into `fifo` regardless of this
        // node's own `disabled` state — it's driven by the OS's own MIDI
        // subsystem on its own thread, entirely independent of whether
        // process() is being called. The central Disable/Enable "cut"
        // behaviour (skip process() entirely while disabled) is correct
        // for nodes with no such background accumulation, but here it left
        // `fifo` silently accumulating every incoming note the whole time
        // this node stayed disabled, then dumping the entire backlog into
        // a single block the moment process() ran again on re-enable.
        // Fixed by overriding passesThroughWhenDisabled() (below) so the
        // host keeps calling process() unconditionally — draining `fifo`
        // continuously either way — but only actually forwarding drained
        // messages into outputMidi when enabled; while disabled they're
        // drained and discarded, keeping this node correctly "cut"
        // (silent), not accidentally pass-through.
        outputMidi.clear();

        // v0.0.919 — "DAW" device: the host's MIDI for this block, sample
        // positions kept (inputMidi is the host's buffer, see ProcessingGraph).
        if (dawInput.load (std::memory_order_relaxed) && ! disabled)
            for (const auto meta : inputMidi)
                outputMidi.addEvent (meta.getMessage(), meta.samplePosition);

        juce::MidiMessage msg;
        int samplePos = 0;
        while (fifo.pop (msg))
            if (! disabled)
                outputMidi.addEvent (msg, samplePos++);

        // v0.0.960 — what other apps sent to the virtual port
        if (auto* vp = virtualIn.load (std::memory_order_relaxed))
            while (vp->pop (msg))
                if (! disabled)
                {
                    outputMidi.addEvent (msg, samplePos++);
                    recordMidiActivity (1);
                }
    }

    /** v0.0.960 — message thread (PatchyProcessor::syncVirtualMidiPorts). */
    void setVirtualPort (VirtualMidiInPort* p) { virtualIn.store (p, std::memory_order_relaxed); }
    std::atomic<VirtualMidiInPort*> virtualIn { nullptr };   // owned by PatchyProcessor

    bool passesThroughWhenDisabled() const override { return true; }

    juce::String selectedDeviceId;
    juce::String selectedDeviceName;   // human-readable, for MidiMonitor source info
    std::atomic<bool> dawInput { false };   // v0.0.919 — "DAW" device, see openDevice()

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
    static juce::var getAvailableDevicesVar (bool isStandalone, const juce::StringArray& ownVirtualPorts = {});   // v0.0.960 — own ports left out

private:
    std::unordered_map<juce::String, juce::String> selections;
};
