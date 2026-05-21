#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>

/**
 * MidiMonitorNode  (nodeType 5)
 *
 * 1 MIDI In port + 1 MIDI Out port (passes MIDI through unchanged).
 * Captures every incoming MidiMessage into a lock-free ring buffer.
 * The WebBridge 30fps timer drains the buffer and pushes events to React.
 *
 * Each captured event carries:
 *   - absolute timestamp (ms since epoch)
 *   - source node name  (set by ProcessingGraph before calling process())
 *   - source device name (set by ProcessingGraph, e.g. "IAC Driver Bus 1")
 *   - raw MIDI bytes
 */

struct MidiMonitorEvent
{
    int64_t      timestampMs   = 0;
    juce::String sourceNode;         // upstream node label
    juce::String sourceDevice;       // upstream MIDI In device name (if any)
    uint8_t      statusByte    = 0;
    uint8_t      data1         = 0;
    uint8_t      data2         = 0;
    int          sampleOffset  = 0;
};

// Batch of monitor events from one node — used by WebBridge telemetry
struct MidiMonitorBatch
{
    juce::String                    nodeId;
    std::vector<MidiMonitorEvent>   events;
};

// ── Shared ring buffer — lives in PatchyProcessor, outlives graph rebuilds ───
struct MidiMonitorBuffer
{
    static constexpr int kRingSize = 1024;

    void push (const MidiMonitorEvent& ev)
    {
        int writePos = (ringWritePos.load() + 1) % kRingSize;
        ring[static_cast<size_t>(writePos)] = ev;
        ringWritePos.store (writePos);
    }

    std::vector<MidiMonitorEvent> drain()
    {
        std::vector<MidiMonitorEvent> result;
        int readPos  = ringReadPos.load();
        int writePos = ringWritePos.load();
        while (readPos != writePos)
        {
            readPos = (readPos + 1) % kRingSize;
            result.push_back (ring[static_cast<size_t>(readPos)]);
            ringReadPos.store (readPos);
        }
        return result;
    }

    std::array<MidiMonitorEvent, kRingSize> ring;
    std::atomic<int> ringWritePos { 0 };
    std::atomic<int> ringReadPos  { 0 };
};

class MidiMonitorNode : public NodeProcessor
{
public:
    // sharedBuffer is owned by PatchyProcessor — survives graph rebuilds
    MidiMonitorNode (const juce::String& nodeId, MidiMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer) {}

    /** Push a batch of MIDI events from a specific upstream source directly into
     *  the monitor buffer. Called per-edge during graph routing so each event
     *  correctly carries its own source label and device name. */
    void pushFromSource (const juce::MidiBuffer& events,
                         const juce::String& nodeName,
                         const juce::String& deviceName)
    {
        if (buffer == nullptr) return;
        const int64_t now = juce::Time::currentTimeMillis();
        for (const auto& meta : events)
        {
            const auto& msg = meta.getMessage();
            MidiMonitorEvent ev;
            ev.timestampMs  = now;
            ev.sourceNode   = nodeName;
            ev.sourceDevice = deviceName;
            ev.sampleOffset = meta.samplePosition;
            const uint8_t* raw = msg.getRawData();
            ev.statusByte = raw[0];
            ev.data1      = msg.getRawDataSize() > 1 ? raw[1] : 0;
            ev.data2      = msg.getRawDataSize() > 2 ? raw[2] : 0;
            buffer->push (ev);
        }
    }

    void process (int /*numSamples*/) override
    {
        outputMidi = inputMidi;
        // Record activity for port LED flashing
        if (! outputMidi.isEmpty())
            recordMidiActivity (outputMidi.getNumEvents());
        // Buffer pushing is handled per-edge via pushFromSource()
        // so each event already has the correct source info
    }

private:
    MidiMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMonitorNode)
};
