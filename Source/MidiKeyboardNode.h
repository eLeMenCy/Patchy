#pragma once
#include "NodeProcessor.h"
#include "MidiMonitorNode.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>

/**
 * MidiKeyboardNode  (nodeType 7)
 *
 * MIDI In + MIDI Out.
 * - No IN connection: generates NoteOn/NoteOff from UI clicks
 * - IN connected:     passes through upstream MIDI + lights up keys
 *
 * UI → audio communication via lock-free atomic event queue.
 */
class MidiKeyboardNode : public NodeProcessor
{
public:
    static constexpr int kQueueSize = 64;

    struct UIEvent
    {
        uint8_t status  = 0;
        uint8_t data1   = 0;
        uint8_t data2   = 0;
        bool    valid   = false;
    };

    MidiKeyboardNode (const juce::String& nodeId, MidiMonitorBuffer* incomingBuf = nullptr)
        : NodeProcessor (nodeId, Type::Midi), inBuffer (incomingBuf)
    {
        readPos.store  (0);
        writePos.store (0);
    }

    juce::String customName;   // user-defined display name (for MidiMonitor NAME column)

    // Called from message thread (React → C++ bridge)
    void pushUIEvent (uint8_t status, uint8_t data1, uint8_t data2)
    {
        int wp   = writePos.load (std::memory_order_relaxed);
        int next = (wp + 1) % kQueueSize;
        if (next == readPos.load (std::memory_order_acquire)) return;
        queue[static_cast<size_t>(wp)] = { status, data1, data2, true };
        writePos.store (next, std::memory_order_release);
    }

    void process (int /*numSamples*/) override
    {
        // Pass through upstream MIDI + push to shared monitor buffer for UI
        outputMidi = inputMidi;
        if (inputMidi.getNumEvents() > 0)
        {
            recordMidiActivity (inputMidi.getNumEvents());
            if (inBuffer != nullptr)
            {
                for (const auto& meta : inputMidi)
                {
                    const auto& msg = meta.getMessage();
                    const uint8_t* raw = msg.getRawData();
                    MidiMonitorEvent ev;
                    ev.timestampMs = juce::Time::currentTimeMillis();
                    ev.sourceNode  = id;
                    ev.statusByte  = raw[0];
                    ev.data1       = msg.getRawDataSize() > 1 ? raw[1] : 0;
                    ev.data2       = msg.getRawDataSize() > 2 ? raw[2] : 0;
                    inBuffer->push (ev);
                }
            }
        }

        // Drain UI-generated events and merge into output
        int rp = readPos.load (std::memory_order_relaxed);
        int wp = writePos.load (std::memory_order_acquire);
        while (rp != wp)
        {
            auto& ev = queue[static_cast<size_t>(rp)];
            if (ev.valid)
            {
                outputMidi.addEvent (
                    juce::MidiMessage (ev.status, ev.data1, ev.data2), 0);
                recordMidiActivity (1);
                ev.valid = false;
            }
            rp = (rp + 1) % kQueueSize;
        }
        readPos.store (rp, std::memory_order_release);
    }

private:
    MidiMonitorBuffer*              inBuffer = nullptr;
    std::array<UIEvent, kQueueSize> queue {};
    std::atomic<int>                readPos  { 0 };
    std::atomic<int>                writePos { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiKeyboardNode)
};
