#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>
#include <algorithm>

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

            // Lightweight Value mirror, added 2026-08-30 — this node never
            // populated outputValues[0] at all before, meaning the
            // frontend's own per-node DMX intensity glow (which reads this
            // mirror via PatchyProcessor.h) never had anything real to show
            // for a Monitor node specifically, regardless of what it was
            // actually receiving. Went unnoticed for the same reason as
            // DmxConsoleNode's own channel-1-only gap (see that file's own
            // comment, and Architecture.md 2026-08-30): a since-fixed
            // frontend bug always showed some dim glow regardless of the
            // real value, masking that this mirror was never populated at
            // all. Max across all 512 received channels, same reasoning as
            // DmxConsoleNode's own fix — a single channel at full should
            // still read as active.
            PAX_Value v {};
            v.type     = PAX_TYPE_DMX;
            v.dataType = PAX_DATA_FLOAT;
            v.key      = 0;
            v.value    = *std::max_element (inputDmxFrame.begin(), inputDmxFrame.end()) / 255.f;

            outputValues[0] = v;
            outputValueCount = 1;
        }
        // Deliberately no else branch — reverted 2026-08-30 (4th pass),
        // after first adding one that reset the bargraph/mirror to dark
        // on disconnection. The user reconsidered directly after seeing
        // it running: with the console's own output port and the real
        // hardware output nodes both correctly holding their last state
        // on disconnect, having the Monitor be the one element that goes
        // dark instead created a jarring inconsistency across the graph
        // — better for the whole system to consistently hold last known
        // state together, matching what a real DMX monitor/tester device
        // would practically do (freeze at its last reading rather than
        // reset to a specific value once its input line goes quiet), than
        // for the Monitor alone to actively signal "nothing here" while
        // everything else still shows the signal chain's own last real
        // state. ProcessingGraph.cpp's own merge fix (see that file's own
        // comment) is still correct and needed regardless — ensuring
        // inputDmxFrameValid genuinely reflects current connection state
        // — this class's own process() simply no longer acts on that
        // change by resetting anything; it just doesn't run this block
        // again until a source reconnects, leaving output/mirror/buffer
        // to naturally hold whatever they were last set to.
    }

private:
    DmxMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxMonitorNode)
};
