/**
 * MidiToDmxPax — MIDI CC → DMX channel adapter (Phase 4)
 *
 * The last of the 7 originally-planned Phase 4 adapters. Listens for one
 * specific MIDI CC number and drives one DMX channel from its value —
 * e.g. CC74 → DMX channel 1, the exact example named when this Pax was
 * first planned (see Architecture.md's Phase 4 table).
 *
 * MIDI in (nodeType 1), DMX out via the same dedicated frame path
 * ValueToDMXPax/AudioToDmxPax already use (ctx->dmxFrameOut/
 * dmxFrameOutValid, PaxAPI.h v4) — not the general Value mechanism, since
 * that's what DmxOutDeviceNode/DmxMonitorNode actually read. A minimal
 * DMX-typed Value is still mirrored alongside (portIndex 0), same
 * reasoning as both of those: purely so the port/edge-matching machinery
 * shows a proper DMX-coloured connection.
 *
 * MIDI passthrough workaround: PaxAPI.h's own PAX_Descriptor doc confirms
 * an explicit midiOutputs=0 doesn't actually mean zero — 0 is ambiguous
 * with "use nodeType's default" — so there's no clean way to declare
 * "MIDI in, zero MIDI out" for a nodeType=1 Pax, exactly the same
 * limitation AudioToDmxPax's own header documents for nodeType=2/audio.
 * Same workaround: declare 1 MIDI output and pass every message through
 * unchanged, so the unused port is at least useful if ever connected
 * downstream, rather than leaving it silently unusable.
 *
 * Design (matching ValueToDMXPax's own conventions throughout):
 *   - MIDI CC (0-127, default 1): which CC number to listen for. Any
 *     other CC number, or any non-CC MIDI message entirely, is passed
 *     through untouched but otherwise ignored by this Pax's own logic.
 *   - DMX Channel (1-512, default 1): which channel to drive.
 *   - CC value (0-127) maps directly to a DMX byte (0-255), scaled by
 *     255/127 and rounded — stored and exposed as the real byte value,
 *     not a normalized 0.0-1.0 fraction the way ValueToDMXPax's own
 *     "Current Value" is, since a decimal has no real meaning in a DMX
 *     context (the actual unit is a 0-255 byte) — see the "Current
 *     Value" bullet below for the full reasoning.
 *   - The last matching CC's value persists across blocks where nothing
 *     new arrives (a member, not reset to 0 each call) — matches how a
 *     physical DMX channel actually holds its value, and specifically
 *     avoids the exact rebuild-reset bug ValueToDMXPax's own "Current
 *     Value" parameter was built to fix (see that file's header for the
 *     full story) — only recomputed when a genuinely new, matching CC
 *     arrives this block, never unconditionally every block, so a value
 *     freshly restored via PAX_setParameter after a graph rebuild can't
 *     be silently overwritten back to 0 by the very next call.
 *   - Multiple matching CC messages in the same block: the last one wins
 *     (same simple, predictable v1 choice as ValueToDMXPax's own).
 *   - "Current Value" parameter (index 2) — a read-only live display
 *     (see PaxAPI.h's PAX_isParameterReadOnly) mirroring the actual DMX
 *     byte (0-255) driving the channel, using the same cog/fold mechanism
 *     built for ValueToDMXPax (this Pax has 3 parameters, so it gets a
 *     settings cog too). Deliberately stored/displayed as the real byte,
 *     not ValueToDMXPax's own normalized-0.0-1.0 convention — a decimal
 *     fraction has no real meaning in a DMX context, where the actual
 *     unit is a 0-255 byte. "Read-only" is a UI-level restriction only —
 *     PAX_setParameter still genuinely writes it, which is exactly what
 *     lets the settingsJson restoration mechanism repopulate it
 *     immediately on a rebuild rather than sitting at 0 until the next
 *     matching CC happens to arrive.
 *
 * Colour: no ports mirror (MIDI in gets passed through unchanged rather
 * than meaningfully "output", DMX-typed Value out), so the
 * Hybrid/Converter auto-detection rule correctly colours this node
 * Converter/fuchsia with no colourCategory override needed.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC MidiToDmxPax.cpp -o MidiToDmxPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC MidiToDmxPax.cpp -o MidiToDmxPax.so
 *   Win:    cl /std=c++20 /LD MidiToDmxPax.cpp /Fe:MidiToDmxPax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>

// ── Pax state ─────────────────────────────────────────────────────────────────
struct MidiToDmxPax
{
    // Parameters
    float midiCC     = 1.f;     // 0-127 — which CC number to listen for
    float dmxChannel = 1.f;     // 1-based, 1-512 — see file header re: the frame path

    // Mirrors the actual DMX byte (0-255) driving the channel right now
    // — exposed as a read-only parameter (index 2, see
    // PAX_isParameterReadOnly below) so the settings panel can show it as
    // a live display. Stored as the real 0-255 byte value rather than
    // ValueToDMXPax's own normalized-0.0-1.0 convention, deliberately —
    // a decimal fraction has no real meaning in a DMX context, where the
    // actual unit is a 0-255 byte; showing the real byte here is more
    // useful than showing an internal normalization the user never asked
    // about. Still genuinely settable via PAX_setParameter, not a dead
    // code path — see file header re: why (rebuild-restoration relies on
    // this, matching ValueToDMXPax's own currentValue exactly).
    float currentValue = 0.f;
};

// ── Pax exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    // nodeType 1 (MIDI): 1 MIDI in, 1 MIDI out (unused-but-declared
    // workaround — see file header). DMX output declared separately
    // below via PAX_getValueOutputCount/Type.
    static PAX_Descriptor d { "MIDI to DMX", "Patchy Examples", "1.0.0", 1, PAX_API_VERSION, 0, 0, 1, 1 };
    return &d;
}

PAX_Instance* PAX_create() { return new MidiToDmxPax(); }

void PAX_destroy (PAX_Instance* i) { delete static_cast<MidiToDmxPax*> (i); }

void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<MidiToDmxPax*> (i);

    // Pass every MIDI message through unchanged — see file header re:
    // the nodeType=1 "can't declare zero MIDI outputs" limitation.
    int written = 0;
    if (ctx->midiIn && ctx->midiOut)
    {
        for (int e = 0; e < ctx->midiInCount && written < ctx->midiMaxCount; ++e)
            ctx->midiOut[written++] = ctx->midiIn[e];
    }
    if (ctx->midiOutCount) *ctx->midiOutCount = written;

    // Only recompute when a genuinely new, matching CC arrives this block
    // — critical, not just tidy, matching ValueToDMXPax's own identical
    // reasoning: currentValue can also be freshly restored via
    // PAX_setParameter (after a graph rebuild), and recomputing
    // unconditionally every block would silently overwrite a
    // just-restored value back to 0 on the very next call.
    const int targetCC = std::clamp ((int) std::lround (a->midiCC), 0, 127);
    if (ctx->midiIn)
    {
        for (int e = 0; e < ctx->midiInCount; ++e)
        {
            const auto& msg = ctx->midiIn[e];
            if (msg.byteCount < 3) continue;   // a CC message is 3 bytes: status, CC#, value

            const uint8_t status = msg.bytes[0] & 0xF0;
            if (status != 0xB0) continue;      // not a Control Change message
            if (msg.bytes[1] != targetCC) continue;

            // Map 0-127 to a DMX byte (0-255) directly — last matching
            // message in this block wins.
            a->currentValue = std::clamp (std::lround (msg.bytes[2] * 255.f / 127.f), 0L, 255L);
        }
    }

    const uint8_t dmxByte = (uint8_t) std::clamp ((int) a->currentValue, 0, 255);

    // Real payload: the dedicated DMX frame path (API v4). Host already
    // zeroed ctx->dmxFrameOut and cleared *ctx->dmxFrameOutValid before
    // this call, so only this one channel needs touching, not all 512.
    if (ctx->dmxFrameOut && ctx->dmxFrameOutValid)
    {
        const int chIndex = std::clamp ((int) a->dmxChannel - 1, 0, 511);
        ctx->dmxFrameOut[chIndex] = dmxByte;
        *ctx->dmxFrameOutValid = true;
    }

    // Minimal Value mirror alongside it, purely so the existing port/edge
    // machinery keeps working — see file header. No blob; the frame above
    // is what DmxOutDeviceNode/DmxMonitorNode actually read.
    if (ctx->valuesOut && ctx->valueMaxCount > 0)
    {
        PAX_Value out {};
        out.type      = PAX_TYPE_DMX;
        out.dataType  = PAX_DATA_FLOAT;
        out.key       = 0;
        out.value     = a->currentValue;
        out.portIndex = 0;
        ctx->valuesOut[0] = out;
        if (ctx->valueOutCount) *ctx->valueOutCount = 1;
    }
    else if (ctx->valueOutCount)
    {
        *ctx->valueOutCount = 0;
    }
}

int PAX_getParameterCount (PAX_Instance*) { return 3; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="MIDI CC";       info->minValue=0.f; info->maxValue=127.f; info->defaultValue=1.f; info->step=1.f; break;
        case 1: info->name="DMX Channel";   info->minValue=1.f; info->maxValue=512.f; info->defaultValue=1.f; info->step=1.f; break;
        case 2: info->name="Current Value"; info->minValue=0.f; info->maxValue=255.f; info->defaultValue=0.f; info->step=1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<MidiToDmxPax*> (i);
    switch (index)
    {
        case 0: return a->midiCC;
        case 1: return a->dmxChannel;
        case 2: return a->currentValue;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<MidiToDmxPax*> (i);
    switch (index)
    {
        case 0: a->midiCC       = std::clamp (value, 0.f, 127.f); break;
        case 1: a->dmxChannel   = std::clamp (value, 1.f, 512.f); break;
        // Genuinely settable, not ignored — see the currentValue member's
        // own comment for why (rebuild-restoration relies on this).
        case 2: a->currentValue = std::clamp (value, 0.f, 255.f); break;
        default: break;
    }
}

// See PaxAPI.h's PAX_isParameterReadOnly doc — only "Current Value" (index
// 2) is read-only; every other parameter here is a normal editable control.
int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 2 ? 1 : 0; }

// Value ports — no generic input (MIDI comes in via ctx->midiIn instead),
// one DMX-typed output.
int PAX_getValueOutputCount() { return 1; }

int PAX_getValueOutputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_DMX : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
