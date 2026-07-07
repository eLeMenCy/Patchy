#pragma once
#include "NodeProcessor.h"
#include "DmxMonitorNode.h"   // for DmxSnapshot (used by WebBridge telemetry)
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  ArtNetSnapshot — one complete 512-channel universe snapshot + universe number
// ─────────────────────────────────────────────────────────────────────────────
struct ArtNetSnapshot
{
    juce::String          nodeId;
    std::array<uint8_t, 512> channels {};
    int                   universe = 0;
    bool                  hasData  = false;
};

// ── Shared monitor buffer — identical mechanics to DmxMonitorBuffer ───────────
//    Stores universe number alongside channel data.
struct ArtNetMonitorBuffer
{
    struct Frame
    {
        std::array<uint8_t, 512> channels {};
        int                      universe = 0;
    };

    void push (const std::array<uint8_t, 512>& ch, int uni)
    {
        pending.channels = ch;
        pending.universe = uni;
        dirty.store (true, std::memory_order_release);
    }

    bool drain (std::array<uint8_t, 512>& outCh, int& outUni)
    {
        if (! dirty.exchange (false, std::memory_order_acq_rel)) return false;
        outCh  = pending.channels;
        outUni = pending.universe;
        return true;
    }

    Frame             pending {};
    std::atomic<bool> dirty   { false };
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetMonitorNode  (nodeType 18)
 *
 * ArtDMX In + ArtDMX Out (pass-through, no processing).
 * Captures each incoming universe snapshot into a shared ArtNetMonitorBuffer.
 * WebBridge 30fps timer drains and pushes to React for display.
 * Optionally filters by universe (universeFilter >= 0).
 */
class ArtNetMonitorNode : public NodeProcessor
{
public:
    ArtNetMonitorNode (const juce::String& nodeId, ArtNetMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer)
    {}

    /** Set universe filter — -1 means accept all universes */
    void setUniverseFilter (int uni) { universeFilter.store (uni, std::memory_order_release); }
    int  getUniverseFilter() const   { return universeFilter.load (std::memory_order_relaxed); }

    void process (int /*numSamples*/) override
    {
        // Pass values through unchanged
        outputValueCount = inputValueCount;
        for (int i = 0; i < inputValueCount; ++i)
            outputValues[static_cast<size_t> (i)] = inputValues[static_cast<size_t> (i)];

        if (inputValueCount > 0)
        {
            recordMidiActivity (inputValueCount);

            if (buffer != nullptr)
            {
                const auto& v = inputValues[0];
                int pktUniverse = (int) v.key;
                int filter      = universeFilter.load (std::memory_order_relaxed);

                // Apply universe filter (-1 = show all)
                if (filter >= 0 && pktUniverse != filter)
                    return;

                std::array<uint8_t, 512> ch {};
                if (v.dataType == PAX_DATA_BLOB && v.dataSize > 0)
                {
                    int copyLen = std::min ((int) v.dataSize, 512);
                    std::memcpy (ch.data(), v.data, (size_t) copyLen);
                }
                buffer->push (ch, pktUniverse);
            }
        }
    }

private:
    ArtNetMonitorBuffer* buffer          = nullptr;
    std::atomic<int>     universeFilter  { -1 };   // -1 = show all

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtNetMonitorNode)
};
