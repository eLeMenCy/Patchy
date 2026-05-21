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

    void openDevice (const juce::String& deviceIdentifier)
    {
        closeDevice();
        selectedDeviceId = deviceIdentifier;
        if (deviceIdentifier.isEmpty()) return;

        output = juce::MidiOutput::openDevice (deviceIdentifier);
        if (output)
            juce::Logger::writeToLog ("MidiOutDeviceNode: opened " + deviceIdentifier);
        else
            juce::Logger::writeToLog ("MidiOutDeviceNode: failed to open " + deviceIdentifier);
    }

    void closeDevice()
    {
        if (output) { output->stopBackgroundThread(); output.reset(); }
    }

    void process (int /*numSamples*/) override
    {
        // Pass MIDI through so downstream nodes can still use it
        outputMidi = inputMidi;
        if (inputMidi.getNumEvents() > 0)
            recordMidiActivity (inputMidi.getNumEvents());

        // Send to the physical device
        if (output)
            for (const auto& meta : inputMidi)
                output->sendMessageNow (meta.getMessage());
    }

    juce::String selectedDeviceId;

private:
    std::unique_ptr<juce::MidiOutput> output;
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
                       ProcessingGraph&    graph);

    /** Re-open all saved selections on a freshly rebuilt graph. */
    void applyDeviceSelections (ProcessingGraph& graph);

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
