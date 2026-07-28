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
 * Captures each incoming universe via the dedicated DMX frame path
 * (PaxAPI.h v4) into a shared DmxMonitorBuffer. WebBridge 30fps timer
 * drains and pushes to React for display.
 */
class DmxMonitorNode : public NodeProcessor
{
public:
    DmxMonitorNode (const juce::String& nodeId, DmxMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer)
    {}

    void process (int /*numSamples*/) override
    {
        // Pass the DMX frame through unchanged, and mirror it into the
        // shared monitor buffer for the UI. No longer reads PAX_Value at
        // all for the channel payload — the old blob-based path silently
        // truncated at 56 of 512 channels (see Architecture.md).
        if (inputDmxFrameValid)
        {
            outputDmxFrame      = inputDmxFrame;
            outputDmxFrameValid = true;
            recordMidiActivity (1);

            if (buffer != nullptr)
                buffer->push (inputDmxFrame);
        }
    }

private:
    DmxMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxMonitorNode)
};
