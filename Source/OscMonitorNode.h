#pragma once
#include "NodeProcessor.h"
#include "OscDeviceNodes.h"   // RawOscMessage, OscInDeviceNode
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>

/**
 * OscMonitorNode  (nodeType 20)
 *
 * 1 OSC In port + 1 OSC Out port (pass-through, no processing).
 * Shows the COMPLETE OSC message — address + every typed argument — not the
 * single collapsed value that PAX_Value carries downstream. This matches how
 * every standard OSC tool (Protokol, TouchOSC, QLC+) displays messages, and
 * matters for Patchy specifically since the Monitor is how you reverse-
 * engineer what a controller/app is sending before building an adapter Pax.
 *
 * Full detail is only available when the upstream node is an OscInDeviceNode
 * (pushFromSource, fed by ProcessingGraph with that node's per-block raw
 * decoded messages). Any other upstream (e.g. a Pax outputting OSC values)
 * falls back to a single-arg display synthesised from the collapsed
 * PAX_Value (pushFallbackValue) — still useful, just less detailed.
 */

struct OscMonitorEvent
{
    int64_t      timestampMs = 0;
    juce::String sourceNode;         // upstream node label
    juce::String address;
    juce::String typeTags;           // e.g. "ffi" (raw OSC type tag chars)
    juce::String argsDisplay;        // e.g. "0.7500  42  hello"
    int          byteCount = 0;
};

// Batch of monitor events from one node — used by WebBridge telemetry
struct OscMonitorBatch
{
    juce::String                  nodeId;
    std::vector<OscMonitorEvent>  events;
};

// ── Shared ring buffer — lives in PatchyProcessor, outlives graph rebuilds ───
struct OscMonitorBuffer
{
    static constexpr int kRingSize = 512;

    void push (const OscMonitorEvent& ev)
    {
        int writePos = (ringWritePos.load() + 1) % kRingSize;
        ring[static_cast<size_t> (writePos)] = ev;
        ringWritePos.store (writePos);
    }

    std::vector<OscMonitorEvent> drain()
    {
        std::vector<OscMonitorEvent> result;
        int readPos  = ringReadPos.load();
        int writePos = ringWritePos.load();
        while (readPos != writePos)
        {
            readPos = (readPos + 1) % kRingSize;
            result.push_back (ring[static_cast<size_t> (readPos)]);
            ringReadPos.store (readPos);
        }
        return result;
    }

    std::array<OscMonitorEvent, kRingSize> ring;
    std::atomic<int> ringWritePos { 0 };
    std::atomic<int> ringReadPos  { 0 };
};

class OscMonitorNode : public NodeProcessor
{
public:
    // sharedBuffer is owned by PatchyProcessor — survives graph rebuilds
    OscMonitorNode (const juce::String& nodeId, OscMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer) {}

    /** Full-detail path — called per-edge from ProcessingGraph when the
     *  upstream node is an OscInDeviceNode. */
    void pushFromSource (const std::vector<RawOscMessage>& msgs,
                         const juce::String& nodeName)
    {
        if (buffer == nullptr) return;
        const int64_t now = juce::Time::currentTimeMillis();
        for (const auto& m : msgs)
        {
            OscMonitorEvent ev;
            ev.timestampMs = now;
            ev.sourceNode  = nodeName;
            ev.address     = m.address;
            ev.typeTags    = m.typeTags;
            ev.argsDisplay = m.argsDisplay;
            ev.byteCount   = m.byteCount;
            buffer->push (ev);
        }
    }

    /** Fallback path — upstream isn't an OscInDeviceNode, so only the
     *  collapsed PAX_Value is available (single arg, address may be
     *  overwritten by a string arg — same known limitation as elsewhere). */
    void pushFallbackValue (const PAX_Value& v, const juce::String& nodeName)
    {
        if (buffer == nullptr) return;
        OscMonitorEvent ev;
        ev.timestampMs = juce::Time::currentTimeMillis();
        ev.sourceNode  = nodeName;

        if (v.dataType == PAX_DATA_BLOB && v.dataSize > 0)
        {
            ev.address = juce::String (juce::CharPointer_UTF8 ((const char*) v.data));
        }
        else
        {
            ev.address    = "(value)";
            ev.typeTags   = "f";
            ev.argsDisplay = juce::String (v.value, 4);
        }
        buffer->push (ev);
    }

    void process (int /*numSamples*/) override
    {
        // Pass values through unchanged
        outputValueCount = inputValueCount;
        for (int i = 0; i < inputValueCount; ++i)
            outputValues[static_cast<size_t> (i)] = inputValues[static_cast<size_t> (i)];

        if (inputValueCount > 0)
            recordMidiActivity (inputValueCount);
        // Buffer pushing is handled per-edge via pushFromSource()/
        // pushFallbackValue() so each event already has the correct source.
    }

private:
    OscMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscMonitorNode)
};
