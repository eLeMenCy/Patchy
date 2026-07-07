#pragma once
#include "ArtNetMonitorNode.h"

// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetConsoleNode  (nodeType 19)
 *
 * ArtDMX Out only — Console is a source node (output-only).
 * - Outputs current fader state as ArtNet universe (PAX_Value::key = universe)
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

        // Build output PAX_Value — ArtNet uses PAX_TYPE_DMX with key = universe
        PAX_Value v {};
        v.type     = PAX_TYPE_DMX;
        v.dataType = PAX_DATA_BLOB;
        v.key      = (uint32_t) universe.load (std::memory_order_relaxed);
        v.value    = current[0] / 255.f;
        v.dataSize = 512;
        std::memcpy (v.data, current.data(), std::min ((size_t) 512, sizeof (v.data)));
        v.dataSize = static_cast<uint16_t> (sizeof (v.data));

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
