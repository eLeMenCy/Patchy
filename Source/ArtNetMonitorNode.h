#pragma once
#include "NodeProcessor.h"
#include "DmxMonitorNode.h"   // for DmxSnapshot (used by WebBridge telemetry)
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <vector>
#include <algorithm>

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
 * Captures each incoming universe via the dedicated ArtNet frame path
 * (NodeProcessor.h) into a shared ArtNetMonitorBuffer. WebBridge 30fps
 * timer drains and pushes to React for display.
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
        // Pass the ArtNet frame through unchanged, and mirror it into the
        // shared monitor buffer for the UI. No longer reads PAX_Value at
        // all for the channel payload — the old blob-based path silently
        // truncated at 56 of 512 channels (see Architecture.md's DMX
        // entry — ArtNet had the identical bug).
        if (inputArtNetFrameValid)
        {
            outputArtNetFrame      = inputArtNetFrame;
            outputArtNetFrameValid = true;
            outputArtNetUniverse   = inputArtNetUniverse;
            recordMidiActivity (1);

            int filter = universeFilter.load (std::memory_order_relaxed);
            if (filter >= 0 && inputArtNetUniverse != filter)
                return;

            if (buffer != nullptr)
                buffer->push (inputArtNetFrame, inputArtNetUniverse);

            // Lightweight Value mirror, added 2026-08-30 as part of
            // migrating ArtNet from "flash" to DMX's own "continuous
            // intensity" treatment (see App.tsx and Architecture.md) —
            // this node never populated outputValues[0] at all before,
            // the exact same gap DmxMonitorNode had before its own fix
            // earlier the same day (see that file's own comment). Placed
            // after the universe filter check above, deliberately — so
            // this mirror (and the glow it drives) reflects exactly what
            // the canvas bargraph display itself is showing, not some
            // other, filtered-out universe's own frame. Max across all
            // 512 received channels, same reasoning as DmxMonitorNode's
            // own fix — a single channel at full should still read as
            // active.
            PAX_Value v {};
            v.type     = PAX_TYPE_DMX;
            v.dataType = PAX_DATA_FLOAT;
            v.key      = (uint32_t) inputArtNetUniverse;
            v.value    = *std::max_element (inputArtNetFrame.begin(), inputArtNetFrame.end()) / 255.f;

            outputValues[0] = v;
            outputValueCount = 1;
        }
        // Deliberately no else branch — reverted 2026-08-30 (4th pass),
        // same reasoning and same fix as DmxMonitorNode.h's own revert
        // (see that file's own comment for the full story) — the user
        // reconsidered directly after seeing the reset-to-dark behaviour
        // running: with the console's own output port and the real
        // hardware output nodes both correctly holding their last state
        // on disconnect, having the Monitor go dark instead created a
        // jarring inconsistency across the graph. Now consistently holds
        // last known state everywhere, matching DmxMonitorNode's own.
    }

private:
    ArtNetMonitorBuffer* buffer          = nullptr;
    std::atomic<int>     universeFilter  { -1 };   // -1 = show all

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtNetMonitorNode)
};
