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
                // Reconstruct 512-byte universe from the incoming PAX_Value blob
                std::array<uint8_t, 512> ch {};
                const auto& v = inputValues[0];
                if (v.dataType == PAX_DATA_BLOB && v.dataSize > 0)
                {
                    int copyLen = std::min ((int) v.dataSize, 512);
                    std::memcpy (ch.data(), v.data, (size_t) copyLen);
                }
                else if (v.dataType == PAX_DATA_FLOAT)
                {
                    // Single float value — map to channel 1
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


// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxConsoleNode  (nodeType 17)
 *
 * DMX In + DMX Out.
 * - No IN connection: outputs current fader state from UI
 * - IN connected: upstream DMX updates internal fader state (pass-through)
 *   UI faders reflect incoming values and can override them
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
        for (auto& ch : faderChannels) ch.store (0,   std::memory_order_relaxed);
        lastSent.fill (255);   // force first send
    }

    // ── Called from message thread (React → C++ bridge) ──────────────────────

    /** Set a single channel value from a fader move (0-based channel index). */
    void setChannel (int channel, uint8_t value)
    {
        if (channel < 0 || channel >= 512) return;
        faderChannels[static_cast<size_t> (channel)].store (value, std::memory_order_relaxed);
        pendingOutput.store (true, std::memory_order_release);
    }

    /** Set all channels at once (e.g. blackout or full restore). */
    void setAllChannels (const std::array<uint8_t, 512>& values)
    {
        for (int i = 0; i < 512; ++i)
            faderChannels[static_cast<size_t> (i)].store (values[i], std::memory_order_relaxed);
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
            // If upstream DMX is connected, let it override fader state
            if (inputValueCount > 0)
            {
                const auto& v = inputValues[0];
                if (v.dataType == PAX_DATA_BLOB && v.dataSize > 0)
                {
                    int copyLen = std::min ((int) v.dataSize, 512);
                    std::memcpy (current.data(), v.data, (size_t) copyLen);

                    // Sync faders to incoming values
                    for (int i = 0; i < copyLen; ++i)
                        faderChannels[static_cast<size_t> (i)].store (current[i],
                                                                       std::memory_order_relaxed);
                }
            }
            else
            {
                // No upstream — use fader state
                for (int i = 0; i < 512; ++i)
                    current[i] = faderChannels[static_cast<size_t> (i)].load (
                                     std::memory_order_relaxed);
            }
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
        // We can only store 56 bytes in data[] — store first 56 channels
        // Downstream DmxOutDeviceNode will receive this and zero-pad to 512
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
    std::atomic<bool>                     pendingOutput { true };
    std::atomic<bool>                     blackout      { false };
    DmxMonitorBuffer*                     buffer        = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxConsoleNode)
};
