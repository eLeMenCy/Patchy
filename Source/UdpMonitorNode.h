#pragma once
#include "NodeProcessor.h"
#include "UdpDeviceNodes.h"   // RawUdpPacket, UdpInDeviceNode
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>

/**
 * UdpMonitorNode  (nodeType 21)
 *
 * 1 Value In port + 1 Value Out port (pass-through, no processing).
 * Shows raw UDP packets — sender IP:port, byte count, and a hex/ASCII
 * preview of the payload — independent of the collapsed PAX_Value that
 * flows downstream for routing (same rationale as OSC Monitor: a plain
 * pass-through would only ever show a truncated/interpreted value, not
 * what's actually on the wire).
 *
 * Full detail is only available when the upstream node is a
 * UdpInDeviceNode (pushFromSource, fed by ProcessingGraph with that
 * node's per-block raw packets, sender info included). Any other
 * upstream falls back to a byte-dump synthesised from the collapsed
 * PAX_Value (pushFallbackValue) — no sender info available in that case.
 */

struct UdpMonitorEvent
{
    int64_t      timestampMs = 0;
    juce::String sourceNode;         // upstream node label
    juce::String senderIp;           // empty if unknown (fallback path)
    int          senderPort = 0;
    int          byteCount  = 0;
    juce::String hexPreview;         // uppercase hex, no spaces, e.g. "48656C6C6F"
};

// Batch of monitor events from one node — used by WebBridge telemetry
struct UdpMonitorBatch
{
    juce::String                  nodeId;
    std::vector<UdpMonitorEvent>  events;
};

// ── Shared ring buffer — lives in PatchyProcessor, outlives graph rebuilds ───
struct UdpMonitorBuffer
{
    static constexpr int kRingSize = 512;

    void push (const UdpMonitorEvent& ev)
    {
        int writePos = (ringWritePos.load() + 1) % kRingSize;
        ring[static_cast<size_t> (writePos)] = ev;
        ringWritePos.store (writePos);
    }

    std::vector<UdpMonitorEvent> drain()
    {
        std::vector<UdpMonitorEvent> result;
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

    std::array<UdpMonitorEvent, kRingSize> ring;
    std::atomic<int> ringWritePos { 0 };
    std::atomic<int> ringReadPos  { 0 };
};

class UdpMonitorNode : public NodeProcessor
{
public:
    // sharedBuffer is owned by PatchyProcessor — survives graph rebuilds
    UdpMonitorNode (const juce::String& nodeId, UdpMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer) {}

    static juce::String toHex (const std::array<uint8_t, RawUdpPacket::kPreviewLen>& bytes, int len)
    {
        static const char* kHexChars = "0123456789ABCDEF";
        juce::String out;
        out.preallocateBytes ((size_t) len * 2 + 4);
        for (int i = 0; i < len; ++i)
        {
            out += kHexChars[(bytes[static_cast<size_t> (i)] >> 4) & 0xF];
            out += kHexChars[bytes[static_cast<size_t> (i)] & 0xF];
        }
        return out;
    }

    /** Full-detail path — called per-edge from ProcessingGraph when the
     *  upstream node is a UdpInDeviceNode. */
    void pushFromSource (const std::vector<RawUdpPacket>& pkts,
                         const juce::String& nodeName)
    {
        if (buffer == nullptr) return;
        const int64_t now = juce::Time::currentTimeMillis();
        for (const auto& p : pkts)
        {
            UdpMonitorEvent ev;
            ev.timestampMs = now;
            ev.sourceNode  = nodeName;
            ev.senderIp    = p.senderIp;
            ev.senderPort  = p.senderPort;
            ev.byteCount   = p.byteCount;
            ev.hexPreview  = toHex (p.preview, p.previewLen);
            buffer->push (ev);
        }
    }

    /** Fallback path — upstream isn't a UdpInDeviceNode, so no sender info
     *  is available; dumps whatever the collapsed PAX_Value carries. */
    void pushFallbackValue (const PAX_Value& v, const juce::String& nodeName)
    {
        if (buffer == nullptr) return;
        UdpMonitorEvent ev;
        ev.timestampMs = juce::Time::currentTimeMillis();
        ev.sourceNode  = nodeName;

        if (v.dataType == PAX_DATA_BLOB && v.dataSize > 0)
        {
            std::array<uint8_t, RawUdpPacket::kPreviewLen> tmp {};
            int len = std::min ((int) v.dataSize, (int) tmp.size());
            std::memcpy (tmp.data(), v.data, (size_t) len);
            ev.byteCount  = v.dataSize;
            ev.hexPreview = toHex (tmp, len);
        }
        else
        {
            std::array<uint8_t, RawUdpPacket::kPreviewLen> tmp {};
            std::memcpy (tmp.data(), &v.value, 4);
            ev.byteCount  = 4;
            ev.hexPreview = toHex (tmp, 4);
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
    UdpMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UdpMonitorNode)
};
