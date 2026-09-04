#pragma once
#include "ArtNetMonitorNode.h"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetConsoleNode  (nodeType 19)
 *
 * ArtDMX Out only — Console is a source node (output-only).
 * - Outputs current fader state as a full 512-channel universe via the
 *   dedicated ArtNet frame path (NodeProcessor.h), plus a lightweight
 *   PAX_Value mirror (type=DMX, dataType=FLOAT, value=channel[0]/255,
 *   key=universe, no blob) for anything that only wants a plain scalar.
 *   The frame is the real payload — cramming the universe into
 *   PAX_Value.data[] used to silently truncate at 56 of 512 channels.
 * - Blackout: when active, outputs all zeros
 * - Outputs only on change (memcmp vs last sent frame)
 * - Universe set from UI settings panel via setUniverse()
 *
 * Undo/redo: follows DMX Console pattern exactly —
 *   512 atomic channels in C++, base64-encoded in settingsJson.
 *   restoreChannels/transferLastSent/resetChannels for flash-correct restore.
 */
class ArtNetConsoleNode : public NodeProcessor
{
public:
    ArtNetConsoleNode (const juce::String& nodeId, ArtNetMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer)
    {
        for (auto& ch : faderChannels) ch.store (0, std::memory_order_relaxed);
        lastSent.fill (0);
    }

    // ── Universe ──────────────────────────────────────────────────────────────
    void setUniverse (int uni)    { universe.store (uni, std::memory_order_release); }
    int  getUniverse() const      { return universe.load (std::memory_order_relaxed); }

    // ── Called from message thread (React → C++ bridge) ──────────────────────

    void transferLastSent (const std::array<uint8_t, 512>& prev)
    {
        std::memcpy (lastSent.data(), prev.data(), 512);
        isFirstRestore = false;
    }
    const std::array<uint8_t, 512>& getLastSent() const { return lastSent; }
    bool getBlackout() const { return blackout.load (std::memory_order_relaxed); }
    void transferBlackout (bool prev)
    {
        blackout.store (prev, std::memory_order_release);
        if (prev) lastSent.fill (0);
    }

    // Force process() to re-emit on its very next call — same reasoning
    // and same fix as DmxConsoleNode's own (see that file's own comment
    // for the full story).
    void forceReEmit() { pendingOutput.store (true, std::memory_order_release); }

    std::array<uint8_t, 512> getAllChannels() const
    {
        std::array<uint8_t, 512> out {};
        for (int i = 0; i < 512; ++i)
            out[static_cast<size_t>(i)] = faderChannels[static_cast<size_t>(i)].load (std::memory_order_relaxed);
        return out;
    }

    void setChannel (int channel, uint8_t value)
    {
        if (channel < 0 || channel >= 512) return;
        faderChannels[static_cast<size_t> (channel)].store (value, std::memory_order_relaxed);
        pendingOutput.store (true, std::memory_order_release);
    }

    void setAllChannels (const std::array<uint8_t, 512>& values)
    {
        for (int i = 0; i < 512; ++i)
            faderChannels[static_cast<size_t> (i)].store (values[static_cast<size_t>(i)], std::memory_order_relaxed);
        pendingOutput.store (true, std::memory_order_release);
    }

    void restoreChannels (const std::array<uint8_t, 512>& values)
    {
        bool isBO = blackout.load (std::memory_order_relaxed);
        bool changed;
        if (isBO)
        {
            std::array<uint8_t, 512> zeros {};
            changed = isFirstRestore || (std::memcmp (zeros.data(), lastSent.data(), 512) != 0);
        }
        else
        {
            changed = isFirstRestore || (std::memcmp (values.data(), lastSent.data(), 512) != 0);
        }
        isFirstRestore = false;
        for (int i = 0; i < 512; ++i)
            faderChannels[static_cast<size_t> (i)].store (values[static_cast<size_t>(i)], std::memory_order_relaxed);
        if (! isBO)
            std::memcpy (lastSent.data(), values.data(), 512);
        if (changed)
            pendingOutput.store (true, std::memory_order_release);
    }

    void resetChannels()
    {
        std::array<uint8_t, 512> zeros {};
        restoreChannels (zeros);
    }

    void restoreBlackout (bool active)
    {
        bool changed = (blackout.load (std::memory_order_relaxed) != active);
        blackout.store (active, std::memory_order_release);
        if (active)
            lastSent.fill (0);  // BO outputs zeros — sync lastSent so next process() sees no change
        if (changed)
            pendingOutput.store (true, std::memory_order_release);
    }

    void setBlackout (bool active)
    {
        blackout.store (active, std::memory_order_release);
        pendingOutput.store (true, std::memory_order_release);
    }

    bool isBlackout() const { return blackout.load (std::memory_order_relaxed); }

    void process (int /*numSamples*/) override
    {
        std::array<uint8_t, 512> current {};
        bool isBlackoutActive = blackout.load (std::memory_order_acquire);

        if (isBlackoutActive)
            current.fill (0);
        else
            for (int i = 0; i < 512; ++i)
                current[static_cast<size_t>(i)] = faderChannels[static_cast<size_t> (i)].load (std::memory_order_relaxed);

        // Push to monitor buffer so connected ArtNetMonitorNode can display faders
        if (buffer != nullptr)
            buffer->push (current, universe.load (std::memory_order_relaxed));

        // Change detection — only emit when values differ
        if (std::memcmp (current.data(), lastSent.data(), 512) == 0
            && ! pendingOutput.exchange (false, std::memory_order_acq_rel))
            return;

        std::memcpy (lastSent.data(), current.data(), 512);
        pendingOutput.store (false, std::memory_order_relaxed);

        // Full universe via the dedicated ArtNet frame path (see
        // NodeProcessor.h) — this used to be squeezed into
        // PAX_Value.data[] (56 bytes), which silently truncated the
        // console to its first 56 of 512 channels, the exact same bug
        // DMX had before its own fix (see Architecture.md).
        const int uni = universe.load (std::memory_order_relaxed);
        outputArtNetFrame      = current;
        outputArtNetFrameValid = true;
        outputArtNetUniverse   = uni;

        // Lightweight Value mirror alongside it, no blob — same reasoning
        // as DmxConsoleNode's own mirror: purely so the existing port/
        // edge-matching UI machinery keeps working.
        //
        // Reflects the MAX across all 512 channels (fixed 2026-08-30, as
        // part of migrating ArtNet from "flash" to DMX's own "continuous
        // intensity" treatment — see App.tsx and Architecture.md), not
        // just current[0] (channel 1) as it did before — the exact same
        // fix already applied to DmxConsoleNode.h earlier the same day,
        // for the identical reason: current[0] meant this mirror never
        // updated at all unless channel 1 specifically was the one being
        // moved. Unlike DMX's own case, this was never actually visible
        // as a UI bug before now, since ArtNet has never yet driven any
        // continuous-intensity display from this value — it's only being
        // fixed here because that's the whole point of the migration
        // itself, not because it was silently broken in a shipped feature.
        PAX_Value v {};
        v.type     = PAX_TYPE_DMX;
        v.dataType = PAX_DATA_FLOAT;
        v.key      = (uint32_t) uni;
        v.value    = *std::max_element (current.begin(), current.end()) / 255.f;

        outputValues[0] = v;
        outputValueCount = 1;
        recordMidiActivity (1);
    }

private:
    std::array<std::atomic<uint8_t>, 512> faderChannels;
    std::array<uint8_t, 512>              lastSent;
    std::atomic<bool>                     pendingOutput  { false };
    std::atomic<bool>                     blackout       { false };
    std::atomic<int>                      universe       { 0 };
    bool                                  isFirstRestore { true };
    ArtNetMonitorBuffer*                  buffer         = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtNetConsoleNode)
};
