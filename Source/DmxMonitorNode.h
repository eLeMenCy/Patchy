#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  DmxSnapshot — one complete 512-channel universe snapshot
//  Shared between DmxMonitorNode/DmxConsoleNode and WebBridge telemetry.
// ─────────────────────────────────────────────────────────────────────────────
struct DmxSnapshot
{
    juce::String        nodeId;
    std::array<uint8_t, 512> channels {};
    bool                hasData = false;
};

// ── Shared monitor buffer — lock-free, lives in PatchyProcessor ───────────────
struct DmxMonitorBuffer
{
    void push (const std::array<uint8_t, 512>& ch)
    {
        pendingChannels = ch;
        dirty.store (true, std::memory_order_release);
    }

    bool drain (std::array<uint8_t, 512>& out)
    {
        if (! dirty.exchange (false, std::memory_order_acq_rel)) return false;
        out = pendingChannels;
        return true;
    }

    std::array<uint8_t, 512> pendingChannels {};
    std::atomic<bool>        dirty           { false };
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxMonitorNode  (nodeType 16)
 *
 * DMX In + DMX Out (pass-through, no processing).
 * Captures each incoming universe snapshot into a shared DmxMonitorBuffer.
 * WebBridge 30fps timer drains and pushes to React for display.
 */
class DmxMonitorNode : public NodeProcessor
{
public:
    DmxMonitorNode (const juce::String& nodeId, DmxMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer)
    {}

    void process (int /*numSamples*/) override
    {
        // Pass values through unchanged
        outputValueCount = inputValueCount;
        for (int i = 0; i < inputValueCount; ++i)
            outputValues[static_cast<size_t> (i)] = inputValues[static_cast<size_t> (i)];

        if (inputValueCount > 0)
        {
            recordMidiActivity (inputValueCount);

            // Push universe snapshot to monitor buffer
            if (buffer != nullptr)
            {
                std::array<uint8_t, 512> ch {};
                const auto& v = inputValues[0];
                if (v.dataType == PAX_DATA_BLOB && v.dataSize > 0)
                {
                    int copyLen = std::min ((int) v.dataSize, 512);
                    std::memcpy (ch.data(), v.data, (size_t) copyLen);
                }
                else if (v.dataType == PAX_DATA_FLOAT)
                {
                    ch[0] = (uint8_t) juce::jlimit (0, 255, (int) (v.value * 255.f));
                }
                buffer->push (ch);
            }
        }
    }

private:
    DmxMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxMonitorNode)
};
