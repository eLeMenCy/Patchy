#pragma once
#include "DmxMonitorNode.h"

// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxConsoleNode  (nodeType 17)
 *
 * DMX Out only — Console is a source node (output-only, like a hardware console).
 * - Outputs current fader state set from UI via atomic channel array
 * - Blackout: when active, outputs all zeros regardless of fader state
 * - Outputs only on change (memcmp vs last sent frame)
 *
 * UI → audio thread communication via atomic snapshot (512 bytes).
 * Audio → UI communication via DmxMonitorBuffer (reused for fader sync).
 */
class DmxConsoleNode : public NodeProcessor
{
public:
    DmxConsoleNode (const juce::String& nodeId, DmxMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer)
    {
        for (auto& ch : faderChannels) ch.store (0, std::memory_order_relaxed);
        lastSent.fill (0);
    }

    // ── Called from message thread (React → C++ bridge) ──────────────────────

    /** Transfer lastSent from previous node instance so restoreChannels can detect real changes. */
    void transferLastSent (const std::array<uint8_t, 512>& prev)
    {
        std::memcpy (lastSent.data(), prev.data(), 512);
        isFirstRestore = false;
    }
    const std::array<uint8_t, 512>& getLastSent() const { return lastSent; }

    /** Get all 512 channel values (for saving to settingsJson). */
    std::array<uint8_t, 512> getAllChannels() const
    {
        std::array<uint8_t, 512> out {};
        for (int i = 0; i < 512; ++i)
            out[i] = faderChannels[i].load (std::memory_order_relaxed);
        return out;
    }

    /** Set a single channel value from a fader move (0-based channel index). */
    void setChannel (int channel, uint8_t value)
    {
        if (channel < 0 || channel >= 512) return;
        faderChannels[static_cast<size_t> (channel)].store (value, std::memory_order_relaxed);
        pendingOutput.store (true, std::memory_order_release);
    }

    /** Set all channels at once (e.g. full restore). */
    void setAllChannels (const std::array<uint8_t, 512>& values)
    {
        for (int i = 0; i < 512; ++i)
            faderChannels[static_cast<size_t> (i)].store (values[i], std::memory_order_relaxed);
        pendingOutput.store (true, std::memory_order_release);
    }

    /** Restore channels — flashes only if values actually changed vs last sent. */
    void restoreChannels (const std::array<uint8_t, 512>& values)
    {
        bool changed = isFirstRestore
                     || (std::memcmp (values.data(), lastSent.data(), 512) != 0);
        isFirstRestore = false;
        for (int i = 0; i < 512; ++i)
            faderChannels[static_cast<size_t> (i)].store (values[i], std::memory_order_relaxed);
        std::memcpy (lastSent.data(), values.data(), 512);
        if (changed)
            pendingOutput.store (true, std::memory_order_release);
    }

    /** Reset all channels to zero — used when undo restores to pre-fader state. */
    void resetChannels()
    {
        std::array<uint8_t, 512> zeros {};
        restoreChannels (zeros);
    }

    /** Restore blackout silently — flashes only if state actually changed. */
    void restoreBlackout (bool active)
    {
        bool changed = (blackout.load (std::memory_order_relaxed) != active);
        blackout.store (active, std::memory_order_release);
        if (changed)
            pendingOutput.store (true, std::memory_order_release);
    }

    /** Blackout toggle — when true, outputs all zeros. */
    void setBlackout (bool active)
    {
        blackout.store (active, std::memory_order_release);
        pendingOutput.store (true, std::memory_order_release);
    }

    bool isBlackout() const { return blackout.load (std::memory_order_relaxed); }

    void process (int /*numSamples*/) override
    {
        // Build current 512-byte frame from fader atomics
        std::array<uint8_t, 512> current {};
        bool isBlackoutActive = blackout.load (std::memory_order_acquire);

        if (isBlackoutActive)
        {
            current.fill (0);
        }
        else
        {
            // Console is output-only — always use fader state
            for (int i = 0; i < 512; ++i)
                current[i] = faderChannels[static_cast<size_t> (i)].load (
                                 std::memory_order_relaxed);
        }

        // Push to monitor buffer so UI faders stay in sync
        if (buffer != nullptr)
            buffer->push (current);

        // Change detection — only emit when values differ
        if (std::memcmp (current.data(), lastSent.data(), 512) == 0
            && ! pendingOutput.exchange (false, std::memory_order_acq_rel))
            return;

        std::memcpy (lastSent.data(), current.data(), 512);
        pendingOutput.store (false, std::memory_order_relaxed);

        // Build output PAX_Value
        PAX_Value v {};
        v.type     = PAX_TYPE_DMX;
        v.dataType = PAX_DATA_BLOB;
        v.key      = 0;
        v.value    = current[0] / 255.f;
        v.dataSize = 512;
        std::memcpy (v.data, current.data(),
                     std::min ((size_t) 512, sizeof (v.data)));
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
    bool                                  isFirstRestore { true };
    DmxMonitorBuffer*                     buffer         = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxConsoleNode)
};
