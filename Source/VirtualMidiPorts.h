#pragma once
// VirtualMidiPorts.h — v0.0.960 (user's idea, "virtual cables as in MidiDash")
//
// A MIDI In / MIDI Out node set to "Virtual port" makes Patchy appear in
// every other MIDI app (macOS CoreMIDI, Linux ALSA; Windows has no virtual
// ports of its own):
//   MIDI In node  "MIDI VIN (X)"  → other apps see an OUTPUT "Patchy Vout-X"
//                                           they can send to (it arrives in Patchy)
//   MIDI Out node "MIDI VOUT (X)" → other apps see an INPUT "Patchy Vin-X"
//                                           that receives what Patchy sends
// In the plugin the DAW track's name goes in the middle ("Patchy Bass Vout-1"),
// so two Patchy instances never clash.
//
// The node's device id is "virtual:X". The ports are owned here, by the
// processor (keyed "in:X" / "out:X"), NOT by the nodes: the processing graph
// is rebuilt on every edit, and re-creating a port each time would make the
// other apps lose it. Nodes only get a raw pointer (atomic); a port that is
// no longer wanted is retired and destroyed at the next sync, by which time
// no graph still uses it.
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

/** Other apps send into this (a CoreMIDI / ALSA destination); a MIDI In node drains it. */
struct VirtualMidiInPort : private juce::MidiInputCallback
{
    explicit VirtualMidiInPort (const juce::String& portName) : name (portName)
    {
        input = juce::MidiInput::createNewDevice (portName, this);
        if (input) input->start();
    }
    ~VirtualMidiInPort() override { if (input) input->stop(); }

    bool isOpen() const { return input != nullptr; }

    /** Audio thread (one reader). */
    bool pop (juce::MidiMessage& m)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 == 0) return false;
        m = buffer[(size_t) s1];
        fifo.finishedRead (1);
        return true;
    }

    const juce::String name;

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 == 0) return;   // full: dropped (the graph isn't running)
        buffer[(size_t) s1] = m;
        fifo.finishedWrite (1);
    }

    std::unique_ptr<juce::MidiInput> input;
    static constexpr int kSize = 1024;
    juce::AbstractFifo fifo { kSize };
    std::array<juce::MidiMessage, kSize> buffer;
};

/** Other apps receive from this (a CoreMIDI / ALSA source); a MIDI Out node sends to it. */
struct VirtualMidiOutPort
{
    explicit VirtualMidiOutPort (const juce::String& portName) : name (portName)
    {
        output = juce::MidiOutput::createNewDevice (portName);
    }
    ~VirtualMidiOutPort()
    {
        // release what Patchy left sounding on it before it disappears
        if (output)
            for (int c = 1; c <= 16; ++c)
            {
                output->sendMessageNow (juce::MidiMessage::allNotesOff (c));
                output->sendMessageNow (juce::MidiMessage::controllerEvent (c, 64, 0));
            }
    }

    bool isOpen() const { return output != nullptr; }
    void send (const juce::MidiMessage& m) { if (output) output->sendMessageNow (m); }

    const juce::String name;

private:
    std::unique_ptr<juce::MidiOutput> output;
};

/** True where the OS can create virtual MIDI ports. */
inline bool virtualMidiPortsSupported()
{
   #if JUCE_MAC || JUCE_LINUX || JUCE_IOS
    return true;
   #else
    return false;
   #endif
}

/** "virtual:3" → 3; anything else → 0. */
inline int virtualPortNumber (const juce::String& deviceId)
{
    return deviceId.startsWith ("virtual:") ? deviceId.fromFirstOccurrenceOf (":", false, false).getIntValue() : 0;
}

/** The name other apps see. isInNode: a MIDI In node (→ "Vout" for the other apps). */
inline juce::String virtualPortName (bool isInNode, int x, const juce::String& trackName)
{
    auto track = trackName.trim().replaceCharacters (":/\\", "   ").substring (0, 40).trim();
    return "Patchy " + (track.isNotEmpty() ? track + " " : juce::String())
         + (isInNode ? "Vout-" : "Vin-") + juce::String (x);
}
