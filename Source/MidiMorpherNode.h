#pragma once
#include "NodeProcessor.h"
#include <cmath>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MidiMorpherNode  (nodeType 28) — v0.0.925, 2026-10-06
 *
 * MIDI MORPHER — single MIDI in / single MIDI out, ONE rule per node
 * (agreed with the user: the node body shows the whole route at a glance,
 * several Morphers are chained when more is needed). Inspired by MidiDash's
 * Map node.
 *
 * The rule:
 *   IN  : channel (Any / 1-16), message (Any / type), Data1 range, Data2 range
 *   OUT : channel (Copy / 1-16), message (Copy / type), Data1 range, Data2 range,
 *         "Pull" per OUT data slot (OUT Data1 takes the incoming Data2, OUT
 *         Data2 takes the incoming Data1 — MidiDash's "Pull 2" / "Pull 1").
 *
 * Data values (raw MIDI bytes, except pitch bend):
 *   Note Off / Note On / Poly AT / CC : Data1 = byte 1, Data2 = byte 2 (0-127)
 *   Program / Channel AT              : Data1 = byte 1, no Data2
 *   Pitch bend                        : Data1 = the 14-bit value (0-16383), no Data2
 * A range's high end of -1 means "Max" — resolved per message type (127, or
 * 16383 for pitch bend), so a full-range rule keeps working when the message
 * type changes (e.g. Pitch → CC scales 0-16383 onto 0-127 by itself).
 *
 * Matching: channel, message type and every data value the message HAS must
 * be inside the IN ranges. An OUT range left at Min–Max keeps the value
 * unchanged (rescaled only across type ranges, e.g. Pitch → CC); any other
 * OUT range scales linearly from the source slot's IN range onto it (low >
 * high inverts; IN low == high gives OUT low, i.e. a fixed value). A source
 * slot the message doesn't have (Data2 of a Program change) gives OUT low.
 *
 * Unmatched channel messages (anything outside the rule, out-of-range values
 * included — the user's choice: ignore, never clamp) are passed through or
 * blocked per node (default: pass through). Non-channel messages (sysex,
 * clock, transport…) are never addressable by the rule and always pass,
 * same precedent as MidiChMatrixNode.
 *
 * Disabled: identity pass-through.
 *
 * State: the rule lives in the node's settingsJson (see parseRule() for the
 * keys). UI changes go through the generic setNodeSettings / commit path
 * (undo for free) and reach the running node via WebBridge::pushSettingsToUI
 * → onNodeSettingsChanged → PatchyProcessor::applyMidiMorpherSettings();
 * rebuilds (undo/redo, project load) restore it in PatchyProcessor's
 * channelRestores pass, like nodeType 27.
 */
class MidiMorpherNode : public NodeProcessor
{
public:
    // Message codes — 0 = Any (IN) / Copy (OUT). Same order as MidiDash's menu.
    enum Msg { kAny = 0, kNoteOff, kNoteOn, kPolyAT, kCC, kProgram, kChannelAT, kPitch, kNumMsg };

    struct Range { int lo = 0; int hi = -1; };   // hi == -1 → "Max"

    struct Rule
    {
        int   inCh  = 0;          // 0 = Any, 1-16
        int   inMsg = kAny;
        Range inD1, inD2;
        int   outCh  = 0;         // 0 = Copy, 1-16
        int   outMsg = kAny;      // kAny = Copy
        Range outD1, outD2;
        bool  outD1FromD2 = false;   // "Pull 2"
        bool  outD2FromD1 = false;   // "Pull 1"
        bool  blockUnmatched = false;
    };

    explicit MidiMorpherNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi) {}

    // ── Rule (message thread) ────────────────────────────────────────────

    /** settingsJson → Rule. Missing keys keep their defaults, so an empty
        or old settingsJson gives MidiDash's default (copy everything). */
    static Rule parseRule (const juce::var& v)
    {
        Rule r;
        if (! v.isObject()) return r;

        auto i = [&v] (const char* key, int def, int lo, int hi)
        {
            return v.hasProperty (key) ? juce::jlimit (lo, hi, (int) v[key]) : def;
        };
        auto range = [&v] (const char* key, Range def)
        {
            if (auto* arr = v[key].getArray())
                if (arr->size() == 2)
                {
                    Range rg;
                    rg.lo = juce::jlimit (0, 16383, (int) (*arr)[0]);
                    const int hi = (int) (*arr)[1];
                    rg.hi = hi < 0 ? -1 : juce::jmin (16383, hi);
                    return rg;
                }
            return def;
        };

        r.inCh   = i ("inCh",   0, 0, 16);
        r.inMsg  = i ("inMsg",  kAny, 0, kNumMsg - 1);
        r.inD1   = range ("inD1",  {});
        r.inD2   = range ("inD2",  {});
        r.outCh  = i ("outCh",  0, 0, 16);
        r.outMsg = i ("outMsg", kAny, 0, kNumMsg - 1);
        r.outD1  = range ("outD1", {});
        r.outD2  = range ("outD2", {});
        r.outD1FromD2    = (bool) v.getProperty ("outD1Pull", false);
        r.outD2FromD1    = (bool) v.getProperty ("outD2Pull", false);
        r.blockUnmatched = v.getProperty ("unmatched", "pass").toString() == "block";
        return r;
    }

    void setRule (const Rule& r)
    {
        const juce::SpinLock::ScopedLockType sl (ruleLock);
        rule = r;
    }

    bool passesThroughWhenDisabled() const override { return true; }

    // ── Audio thread ─────────────────────────────────────────────────────

    void process (int /*numSamples*/) override
    {
        outputMidi.clear();

        // v0.0.929 — Learn: while armed, channel events are held back (the
        // user's knob turn / key press shouldn't reach the synth); system
        // messages (clock, transport…) still pass.
        if (captureLearn (inputMidi))
        {
            for (const auto meta : inputMidi)
                if (meta.numBytes > 0 && meta.data[0] >= 0xF0)
                    outputMidi.addEvent (meta.data, meta.numBytes, meta.samplePosition);
            return;
        }

        if (disabled)
        {
            outputMidi = inputMidi;
            if (! outputMidi.isEmpty())
                recordMidiActivity (outputMidi.getNumEvents());
            return;
        }

        Rule r;
        {
            const juce::SpinLock::ScopedLockType sl (ruleLock);
            r = rule;
        }

        int nMatched = 0, nPassed = 0, nBlocked = 0;
        std::uint64_t last = 0;

        for (const auto meta : inputMidi)
        {
            const auto msg = meta.getMessage();
            const auto* raw = msg.getRawData();
            const int  size = msg.getRawDataSize();

            const int type = size > 0 ? msgTypeOf (raw[0]) : kAny;
            if (type == kAny)
            {
                outputMidi.addEvent (msg, meta.samplePosition);   // sysex, clock, … — never addressable
                continue;
            }

            const int ch = (raw[0] & 0x0F) + 1;
            int d1 = 0, d2 = 0;
            readData (type, raw, size, d1, d2);

            if (! matches (r, type, ch, d1, d2))
            {
                if (! r.blockUnmatched) { outputMidi.addEvent (msg, meta.samplePosition); ++nPassed; }
                else                    ++nBlocked;
                continue;
            }

            const int outType = r.outMsg == kAny ? type : r.outMsg;
            const int outCh   = r.outCh  == 0    ? ch   : r.outCh;

            // Source slot for each OUT data value (Pull swaps), with its IN range.
            const int  src1Val   = r.outD1FromD2 ? d2 : d1;
            const int  src1Type  = type;
            const bool src1Has   = r.outD1FromD2 ? hasData2 (type) : true;
            const Range src1Rng  = r.outD1FromD2 ? r.inD2 : r.inD1;
            const int  src1Max   = r.outD1FromD2 ? 127 : maxData1 (src1Type);

            const int  src2Val   = r.outD2FromD1 ? d1 : d2;
            const bool src2Has   = r.outD2FromD1 ? true : hasData2 (type);
            const Range src2Rng  = r.outD2FromD1 ? r.inD1 : r.inD2;
            const int  src2Max   = r.outD2FromD1 ? maxData1 (type) : 127;

            const int o1 = scale (src1Has, src1Val, src1Rng, src1Max, r.outD1, maxData1 (outType));
            const int o2 = scale (src2Has, src2Val, src2Rng, src2Max, r.outD2, 127);

            outputMidi.addEvent (build (outType, outCh, o1, o2), meta.samplePosition);
            ++nMatched;
            last = packMorph (type, ch, d1, d2, outType, outCh, o1, hasData2 (outType) ? o2 : 0);
        }

        if (nMatched > 0)
        {
            matchedCount.fetch_add (nMatched, std::memory_order_relaxed);
            lastMorph.store (last, std::memory_order_relaxed);
        }
        if (nPassed  > 0) passedCount.fetch_add  (nPassed,  std::memory_order_relaxed);
        if (nBlocked > 0) blockedCount.fetch_add (nBlocked, std::memory_order_relaxed);

        if (! outputMidi.isEmpty())
            recordMidiActivity (outputMidi.getNumEvents());
    }

    // ── Live feedback (v0.0.926) — drained by the 30 Hz activity poll ────
    // Counts since the last poll: events that matched (and were morphed),
    // unmatched events passed through, unmatched events blocked; plus the
    // most recent morph, packed in one 64-bit word so the audio thread
    // never takes a lock for it (see packMorph()).
    struct Feedback { int matched = 0, passed = 0, blocked = 0; std::uint64_t last = 0; };
    Feedback drainFeedback()
    {
        Feedback f;
        f.matched = matchedCount.exchange (0, std::memory_order_relaxed);
        f.passed  = passedCount.exchange  (0, std::memory_order_relaxed);
        f.blocked = blockedCount.exchange (0, std::memory_order_relaxed);
        f.last    = lastMorph.load (std::memory_order_relaxed);
        return f;
    }

    /** One side = type (3 bits) | channel-1 (4) | data1 (14) | data2 (7) = 28 bits.
        Word = in side (bits 0-27) | out side (bits 28-55) | valid flag (bit 63). */
    static std::uint64_t packMorph (int t, int ch, int d1, int d2, int ot, int och, int o1, int o2)
    {
        const auto side = [] (int ty, int c, int a, int b) -> std::uint64_t
        {
            return  (std::uint64_t) (ty & 0x7)
                 | ((std::uint64_t) ((c - 1) & 0xF)   << 3)
                 | ((std::uint64_t) (a & 0x3FFF)      << 7)
                 | ((std::uint64_t) (b & 0x7F)        << 21);
        };
        return side (t, ch, d1, d2) | (side (ot, och, o1, o2) << 28) | (std::uint64_t (1) << 63);
    }

    // ── Pure helpers (public so they can be unit-tested) ─────────────────

    static int msgTypeOf (std::uint8_t status)
    {
        switch (status & 0xF0)
        {
            case 0x80: return kNoteOff;
            case 0x90: return kNoteOn;
            case 0xA0: return kPolyAT;
            case 0xB0: return kCC;
            case 0xC0: return kProgram;
            case 0xD0: return kChannelAT;
            case 0xE0: return kPitch;
            default:   return kAny;   // not a channel voice message
        }
    }

    static bool hasData2 (int type)  { return type == kNoteOff || type == kNoteOn || type == kPolyAT || type == kCC; }
    static int  maxData1 (int type)  { return type == kPitch ? 16383 : 127; }

    static int resolveHi (const Range& rg, int typeMax) { return rg.hi < 0 ? typeMax : juce::jmin (rg.hi, typeMax); }
    static int resolveLo (const Range& rg, int typeMax) { return juce::jmin (rg.lo, typeMax); }

    static bool inRange (int v, const Range& rg, int typeMax)
    {
        const int a = resolveLo (rg, typeMax), b = resolveHi (rg, typeMax);
        return v >= juce::jmin (a, b) && v <= juce::jmax (a, b);
    }

    static bool matches (const Rule& r, int type, int ch, int d1, int d2)
    {
        if (r.inCh  != 0    && r.inCh  != ch)   return false;
        if (r.inMsg != kAny && r.inMsg != type) return false;
        if (! inRange (d1, r.inD1, maxData1 (type))) return false;
        if (hasData2 (type) && ! inRange (d2, r.inD2, 127)) return false;
        return true;
    }

    static bool isFullRange (const Range& rg) { return rg.lo == 0 && rg.hi < 0; }

    /** Linear map from the source IN range onto the OUT range, rounded.
        An OUT range left at Min–Max means "value unchanged" — only rescaled
        when the type's range changes (Pitch 0-16383 ↔ 0-127), so a rule that
        only FILTERS (IN 64–Max) never stretches the values it lets through. */
    static int scale (bool has, int v, const Range& inRg, int inMax, const Range& outRg, int outMax)
    {
        if (isFullRange (outRg))
        {
            if (! has) return 0;
            if (inMax == outMax) return juce::jlimit (0, outMax, v);
            return juce::jlimit (0, outMax, (int) std::lround ((double) v * outMax / inMax));
        }

        const int inLo  = resolveLo (inRg, inMax),  inHi  = resolveHi (inRg, inMax);
        const int outLo = resolveLo (outRg, outMax), outHi = resolveHi (outRg, outMax);
        if (! has || inLo == inHi) return outLo;

        const double t = (double) (v - inLo) / (double) (inHi - inLo);
        const int out  = (int) std::lround (outLo + t * (outHi - outLo));
        return juce::jlimit (0, outMax, out);
    }

    static void readData (int type, const std::uint8_t* raw, int size, int& d1, int& d2)
    {
        const int b1 = size > 1 ? (raw[1] & 0x7F) : 0;
        const int b2 = size > 2 ? (raw[2] & 0x7F) : 0;
        if (type == kPitch) { d1 = b1 | (b2 << 7); d2 = 0; }
        else                { d1 = b1; d2 = hasData2 (type) ? b2 : 0; }
    }

    static juce::MidiMessage build (int type, int ch, int d1, int d2)
    {
        const auto st = [ch] (int hi) { return (std::uint8_t) (hi | ((ch - 1) & 0x0F)); };
        const auto b  = [] (int v) { return (std::uint8_t) juce::jlimit (0, 127, v); };
        switch (type)
        {
            case kNoteOff:   return juce::MidiMessage (st (0x80), b (d1), b (d2));
            case kNoteOn:    return juce::MidiMessage (st (0x90), b (d1), b (d2));
            case kPolyAT:    return juce::MidiMessage (st (0xA0), b (d1), b (d2));
            case kCC:        return juce::MidiMessage (st (0xB0), b (d1), b (d2));
            case kProgram:   return juce::MidiMessage (st (0xC0), b (d1));
            case kChannelAT: return juce::MidiMessage (st (0xD0), b (d1));
            case kPitch:
            default:
            {
                const int v = juce::jlimit (0, 16383, d1);
                return juce::MidiMessage (st (0xE0), (std::uint8_t) (v & 0x7F), (std::uint8_t) (v >> 7));
            }
        }
    }

private:
    juce::SpinLock ruleLock;
    Rule           rule;   // guarded by ruleLock — copied once per block in process()

    std::atomic<int>           matchedCount { 0 }, passedCount { 0 }, blockedCount { 0 };
    std::atomic<std::uint64_t> lastMorph    { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiMorpherNode)
};
