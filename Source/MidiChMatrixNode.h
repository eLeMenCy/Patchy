#pragma once
#include "NodeProcessor.h"
#include <array>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MidiChMatrixNode  (nodeType 27)
 *
 * MIDI CH. MATRIX — single MIDI in / single MIDI out, with an internal,
 * resizable NxN channel-remap matrix (N = kMinGrid..kMaxGrid, default
 * kMinGrid). Rows = input channel, columns = output channel; a lit cell
 * (r, c) means "messages arriving on input channel r+1 get duplicated
 * onto output channel c+1" — deliberately one-to-many (a single input
 * channel's own row may have several lit columns, fanning that channel
 * out to multiple outputs at once), not a strict 1:1 lookup. An input
 * channel with zero lit cells in its own row is either silently dropped
 * or passed through unchanged, per dropUnmapped below (a per-node global
 * toggle, not a per-cell setting).
 *
 * Built as a BUILT-IN rather than a Pax — a deliberate architectural
 * choice, 2026-09-19, matching AudioPlayerNode's own precedent: the Pax
 * ABI has no mechanism for bulk/custom state at all (only individual
 * scalar parameters, designed for things like a Sensitivity or Damping
 * slider), and this node's own resizable, up-to-16x16 grid needs exactly
 * that — the same category of problem AudioPlayerNode already solved by
 * being a built-in, rather than this node inventing a brand-new, unproven
 * Pax ABI extension for a single use case.
 *
 * Grid state lives behind matrixLock (a juce::SpinLock) rather than one
 * atomic per cell (up to 256 of them) — writes only ever come from the
 * message thread (a user's own mouse click, or a settings/undo restore),
 * are rare and brief, and process() takes one short, safe snapshot copy
 * per block rather than holding the lock during any per-event work.
 *
 * UI -> audio thread communication: setGridSize()/toggleCell()/
 * setDropUnmapped()/resetToDefault(), called live from WebBridge's own
 * dispatch (see WebBridge_Dispatch.cpp), matching DmxConsoleNode's own
 * established real-time-update pattern (bypassing the normal settings/
 * undo-snapshot path for a live, in-progress interaction).
 * Persistence (settingsJson, on rebuild/undo/redo/project-load) instead
 * goes through restoreState(), matching DmxConsoleNode's own
 * restoreChannels() role — see PatchyProcessor.h's own settingsJson
 * parsing for the call site.
 */
class MidiChMatrixNode : public NodeProcessor
{
public:
    static constexpr int kMinGrid  = 4;
    static constexpr int kMaxGrid  = 16;
    static constexpr int kCellCount = kMaxGrid * kMaxGrid;

    explicit MidiChMatrixNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi)
    {
        matrix.fill (false);
    }

    // ── Called from message thread (React -> C++ bridge) ─────────────────

    /** Live resize — preserves already-lit cells that remain in bounds;
        only cells that fall outside the new, smaller grid are dropped. */
    void setGridSize (int n)
    {
        n = juce::jlimit (kMinGrid, kMaxGrid, n);
        const juce::SpinLock::ScopedLockType sl (matrixLock);
        gridSize.store (n, std::memory_order_relaxed);
        for (int r = 0; r < kMaxGrid; ++r)
            for (int c = 0; c < kMaxGrid; ++c)
                if (r >= n || c >= n)
                    matrix[static_cast<size_t> (r * kMaxGrid + c)] = false;
    }
    int getGridSize() const { return gridSize.load (std::memory_order_relaxed); }

    void toggleCell (int r, int c)
    {
        if (r < 0 || c < 0 || r >= kMaxGrid || c >= kMaxGrid) return;
        const juce::SpinLock::ScopedLockType sl (matrixLock);
        auto& cell = matrix[static_cast<size_t> (r * kMaxGrid + c)];
        cell = ! cell;
    }

    void setDropUnmapped (bool shouldDrop) { dropUnmapped.store (shouldDrop, std::memory_order_relaxed); }
    bool getDropUnmapped() const { return dropUnmapped.load (std::memory_order_relaxed); }

    /** Reset button — a full, distinct wipe (grid back to kMinGrid AND
        every lit cell cleared), genuinely different from an ordinary
        resize, which preserves in-bounds cells. */
    void resetToDefault()
    {
        const juce::SpinLock::ScopedLockType sl (matrixLock);
        gridSize.store (kMinGrid, std::memory_order_relaxed);
        matrix.fill (false);
    }

    /** Restore from settingsJson — rebuild/undo/redo/project-load path. */
    void restoreState (int n, const std::array<bool, kCellCount>& cells, bool shouldDrop)
    {
        const juce::SpinLock::ScopedLockType sl (matrixLock);
        gridSize.store (juce::jlimit (kMinGrid, kMaxGrid, n), std::memory_order_relaxed);
        matrix = cells;
        dropUnmapped.store (shouldDrop, std::memory_order_relaxed);
    }
    std::array<bool, kCellCount> getMatrixSnapshot() const
    {
        const juce::SpinLock::ScopedLockType sl (matrixLock);
        return matrix;
    }

    bool passesThroughWhenDisabled() const override { return true; }

    // Drain-and-reset, matching NodeProcessor's own drainMidiActivity()
    // pattern — each poll reflects only activity since the last one.
    std::uint16_t drainInputChannelActivity()  { return inputChannelActivity.exchange  (0, std::memory_order_relaxed); }
    std::uint16_t drainOutputChannelActivity() { return outputChannelActivity.exchange (0, std::memory_order_relaxed); }

    // ── Called on the audio thread ────────────────────────────────────────

    void process (int /*numSamples*/) override
    {
        outputMidi.clear();

        if (disabled)
        {
            // Identity pass-through — input channel N always comes out as
            // channel N regardless of the configured matrix, matching this
            // project's own established precedent for same-type in-place
            // MIDI transforms (TransposePax already behaves this way).
            outputMidi = inputMidi;

            // Still genuinely live (not frozen) — track activity same as
            // the enabled branch below, just with input==output per channel.
            const int n = gridSize.load (std::memory_order_relaxed);
            std::uint16_t mask = 0;
            for (const auto& meta : inputMidi)
            {
                const int ch = meta.getMessage().getChannel();
                if (ch >= 1 && ch <= n)
                    mask |= static_cast<std::uint16_t> (1u << (ch - 1));
            }
            if (mask != 0)
            {
                inputChannelActivity.fetch_or  (mask, std::memory_order_relaxed);
                outputChannelActivity.fetch_or (mask, std::memory_order_relaxed);
            }
            if (! outputMidi.isEmpty())
                recordMidiActivity (outputMidi.getNumEvents());
            return;
        }

        const int  n    = gridSize.load (std::memory_order_relaxed);
        const bool drop = dropUnmapped.load (std::memory_order_relaxed);

        std::array<bool, kCellCount> snapshot;
        {
            const juce::SpinLock::ScopedLockType sl (matrixLock);
            snapshot = matrix;
        }

        std::uint16_t inputMask = 0, outputMask = 0;

        for (const auto& meta : inputMidi)
        {
            const auto msg = meta.getMessage();
            const int  ch  = msg.getChannel(); // 1-16, 0 for messages with no channel (e.g. sysex)

            if (ch < 1 || ch > n)
            {
                // Outside the configured grid entirely (or channel-less) —
                // always passes through untouched, regardless of dropUnmapped,
                // since it was never addressable by the matrix in the first place.
                outputMidi.addEvent (msg, meta.samplePosition);
                continue;
            }

            const int r = ch - 1;
            inputMask |= static_cast<std::uint16_t> (1u << r); // "something arrived here", even if it ends up dropped below

            bool anyMapped = false;
            for (int c = 0; c < n; ++c)
            {
                if (snapshot[static_cast<size_t> (r * kMaxGrid + c)])
                {
                    anyMapped = true;
                    outputMask |= static_cast<std::uint16_t> (1u << c); // genuinely emitted on this output channel
                    auto remapped = msg;
                    remapped.setChannel (c + 1);
                    outputMidi.addEvent (remapped, meta.samplePosition);
                }
            }
            if (! anyMapped && ! drop)
            {
                outputMask |= static_cast<std::uint16_t> (1u << r); // pass-through is a genuine emission too, same channel as the input
                outputMidi.addEvent (msg, meta.samplePosition); // pass through unchanged
            }
        }

        if (inputMask  != 0) inputChannelActivity.fetch_or  (inputMask,  std::memory_order_relaxed);
        if (outputMask != 0) outputChannelActivity.fetch_or (outputMask, std::memory_order_relaxed);
        if (! outputMidi.isEmpty())
            recordMidiActivity (outputMidi.getNumEvents());
    }

private:
    std::atomic<int>  gridSize     { kMinGrid };
    std::atomic<bool> dropUnmapped { true };
    juce::SpinLock    matrixLock;
    std::array<bool, kCellCount> matrix; // guarded by matrixLock; index = r * kMaxGrid + c

    // Channel-flash feature, 2026-09-20 — one bit per channel (1-16), set
    // during process() (both enabled and disabled branches — disabled is
    // identity pass-through, still genuinely live, not a frozen state),
    // drained by the same 30Hz poll that already drives the existing
    // per-node edge-activity flash elsewhere in this project (see
    // getPortActivity() in PatchyProcessor.h). Input bit set for every
    // channel that had ANY incoming message, regardless of whether it was
    // mapped/dropped ("something arrived here" is still worth showing);
    // output bit set only for channels that genuinely emitted a message.
    std::atomic<std::uint16_t> inputChannelActivity  { 0 };
    std::atomic<std::uint16_t> outputChannelActivity { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiChMatrixNode)
};
