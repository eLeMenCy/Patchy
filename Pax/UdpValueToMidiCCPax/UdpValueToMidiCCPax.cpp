/**
 * UdpValueToMidiCCPax — generic Value → MIDI CC adapter (Phase 4)
 *
 * The 6th of the 7 originally-planned Phase 4 adapters, and per
 * Architecture.md's own note, the first real use of UDP In's output —
 * takes whatever generic value a UDP In (or any other generic-Value
 * source) produces and emits it as a MIDI CC message.
 *
 * Structurally the mirror image of MidiToDmxPax: that one reads MIDI and
 * writes DMX; this one reads a generic-shaped Value port and writes
 * MIDI — but that Value input is specifically declared UDP-typed (via
 * PAX_getValueInputType, see below), not left as the PAX_VALUETYPE_GENERIC
 * default, matching MqttToValuePax's own established precedent: a generic
 * Value input can't actually connect to a UDP-typed output at all under
 * Patchy's strict port typing, so this isn't just a colour preference,
 * it's what lets this Pax connect to UdpInDeviceNode in the first place.
 * (An earlier version of this file left it generic — a genuine oversight,
 * caught and fixed 2026-08-22 after the user noticed the input port
 * wasn't rendering in UDP's own colour.)
 *
 * MIDI output workaround, the mirror of MidiToDmxPax's own: PaxAPI.h's
 * own PAX_Descriptor doc confirms "0 = use nodeType's default" applies
 * uniformly to all four port-count override fields — meaning a
 * nodeType=1 Pax can't cleanly declare zero MIDI *input* either, the
 * exact same limitation MidiToDmxPax's header documents for its own
 * zero-MIDI-*output* case. Same workaround, same direction reversed:
 * this Pax has an unwanted-but-declared MIDI input it doesn't actually
 * want, so it passes any incoming MIDI through unchanged first, then
 * appends its own generated CC message after — the unused input port is
 * at least useful if ever connected, and the generated message still
 * gets through regardless.
 *
 * Design:
 *   - MIDI CC (0-127, default 1): which CC number to emit on. Two
 *     accepted UDP packet shapes, additive — see PAX_process below for
 *     the full reasoning: a plain 4-byte float packet (UdpInDeviceNode's
 *     default for any 4-byte datagram) uses this configured value as-is,
 *     unchanged since this Pax was first built; a 5-byte packet (1 byte
 *     CC number + 4-byte big-endian float payload — UdpInDeviceNode's
 *     existing "not exactly 4 bytes -> raw blob" rule already covers
 *     this with zero changes needed there) overrides it per-message with
 *     the packet's own first byte, added 2026-08-23 after the user
 *     pointed out a fixed, pre-configured CC number left this Pax unable
 *     to drive different CC numbers dynamically from one UDP source.
 *   - The override also writes back into this same "MIDI CC" parameter
 *     itself (not just used locally for that one message), and the
 *     parameter is marked live-synced (PAX_isParameterLiveSynced, added
 *     2026-08-24) — so the slider stays a completely normal, draggable
 *     control the whole time, but a backend-side override moves its
 *     on-screen position to follow along live, reusing the same push
 *     path that already restores slider positions on undo/redo. The
 *     user's own framing: "even if one moves it, it will jump to the
 *     new value received if it is 5 bytes" — deliberately no separate
 *     lock/unlock state at all, since the override simply keeps winning
 *     on every subsequent 5-byte message regardless of what the slider
 *     was moved to in between.
 *   - MIDI Channel (1-16, default 1): which channel to emit on — needed
 *     here specifically because this Pax *generates* a new message from
 *     scratch rather than transforming an existing one (TransposePax's
 *     own precedent doesn't need this, since it just keeps whatever
 *     channel the original incoming message already had).
 *   - Input Min/Max (defaults 0.0/1.0), same convention and same
 *     reasoning as ValueToDMXPax's own: incoming values from different
 *     sources land in wildly different ranges, so these two parameters
 *     map [Input Min, Input Max] to the project's own established
 *     internal 0.0-1.0 convention before scaling to a MIDI CC value
 *     (0-127 this time, not a DMX byte).
 *   - Deliberately does NOT continuously re-emit the last value every
 *     block the way ValueToDMXPax/MidiToDmxPax hold and rewrite a DMX
 *     frame every block — MIDI is a discrete-event protocol (see this
 *     project's own established visual-language principle: discrete
 *     protocols flash, continuous ones like DMX show held intensity), so
 *     a CC message is only emitted when a new incoming Value genuinely
 *     arrives this block, matching how MIDI CC is actually used in
 *     practice — sent on change, not held/repeated.
 *   - Multiple values arriving in the same block: the last one wins,
 *     same simple, predictable v1 choice as ValueToDMXPax's own.
 *   - "MIDI CC Value" parameter (index 4, renamed from "Current Value"
 *     2026-08-24, at the user's own request, though staying at its
 *     original index — a separate attempt to also move it next to "MIDI
 *     CC" was tried and reverted the same day, once the user realised
 *     this parameter is read-only, so reordering alone wouldn't fix the
 *     visual inconsistency with the sliders around it; see that revert's
 *     own note further down and Architecture.md for the full story) — a
 *     read-only live display
 *     (see PaxAPI.h's PAX_isParameterReadOnly) mirroring the last CC
 *     value actually emitted (0-127, a real MIDI CC value — not a
 *     normalized 0.0-1.0 fraction, matching the same "a decimal has no
 *     real meaning in this protocol's own context" reasoning that led to
 *     ValueToDMXPax/MidiToDmxPax's own 2026-08-22 byte-display fix,
 *     applied correctly from the start here rather than needing a
 *     follow-up correction). Five parameters means this Pax gets the
 *     cog/fold treatment automatically. Still genuinely settable via
 *     PAX_setParameter, not a dead code path — that's exactly what lets
 *     the settingsJson restoration mechanism repopulate it immediately
 *     on a graph rebuild rather than sitting at 0 until the next value
 *     happens to arrive. Only recomputed when a genuinely new value
 *     arrives this block, never unconditionally every block, matching
 *     ValueToDMXPax's own already-fixed reasoning exactly — a value
 *     freshly restored via PAX_setParameter after a rebuild must not be
 *     silently overwritten back to 0 by the very next call.
 *
 *     Note (2026-08-24): a rename + reorder of this parameter, moving it
 *     to sit right next to "MIDI CC" at index 1, was tried and then
 *     reverted the same day, after the user realised this is a
 *     read-only field, not a slider — the frontend's own read-only
 *     rendering path (label+value side by side, no slider at all) looks
 *     inconsistent sitting between two sliders, so reordering alone
 *     wouldn't have fixed the visual mismatch. The name itself ("MIDI CC
 *     Value") was kept, applied separately and immediately after, since
 *     the naming clarity was independently worthwhile regardless of
 *     position — only the *reorder* half of the original change was
 *     reverted, this parameter genuinely stayed at its original index 4
 *     throughout. The real fix for the visual mismatch belongs in that
 *     shared rendering path (affecting every Pax with a read-only
 *     parameter, not just this one's ordering) rather than in this
 *     file's own parameter layout. Revisit alongside that redesign
 *     rather than reordering parameters again in the meantime.
 *
 * Colour: no ports mirror (generic Value in, MIDI out — MIDI is a real,
 * native port here via nodeType, not a cosmetic Value-typed mirror the
 * way DMX needed one), so the Hybrid/Converter auto-detection rule
 * correctly colours this node Converter/fuchsia with no colourCategory
 * override needed.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC UdpValueToMidiCCPax.cpp -o UdpValueToMidiCCPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC UdpValueToMidiCCPax.cpp -o UdpValueToMidiCCPax.so
 *   Win:    cl /std=c++20 /LD UdpValueToMidiCCPax.cpp /Fe:UdpValueToMidiCCPax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Network byte order (big-endian) float decode for the 5-byte
// "CC number + CC payload" packet shape (see PAX_process below) — same
// convention and same reasoning as UdpInDeviceNode/UdpOutDeviceNode's own
// 2026-08-23 byte-order fix in UdpDeviceNodes.h, but necessarily a
// separate, independent copy here: Pax files have zero dependency on any
// backend source file, by design (see PaxAPI.h), so this can't be shared
// code even though the logic is identical. Correct regardless of host
// endianness — builds the bit pattern explicitly via bit-shifts rather
// than relying on any assumption about memory layout.
static float bigEndianFloatFromBytes (const uint8_t* b)
{
    uint32_t bits = (static_cast<uint32_t> (b[0]) << 24)
                  | (static_cast<uint32_t> (b[1]) << 16)
                  | (static_cast<uint32_t> (b[2]) << 8)
                  |  static_cast<uint32_t> (b[3]);
    float result;
    std::memcpy (&result, &bits, 4);
    return result;
}

// ── Pax state ─────────────────────────────────────────────────────────────────
struct UdpValueToMidiCCPax
{
    // Parameters
    float midiCC      = 1.f;     // 0-127 — which CC number to emit on
    float midiChannel = 1.f;     // 1-16 — which MIDI channel to emit on
    float inputMin    = 0.f;     // incoming value that maps to CC 0
    float inputMax    = 1.f;     // incoming value that maps to CC 127

    // Mirrors the last CC value (0-127) actually emitted — exposed as a
    // read-only parameter (index 4, see PAX_isParameterReadOnly below) so
    // the settings panel can show it as a live display. A real MIDI CC
    // value, not a normalized 0.0-1.0 fraction — a decimal has no real
    // meaning in a MIDI CC context, matching the same reasoning behind
    // ValueToDMXPax/MidiToDmxPax's own 2026-08-22 byte-display fix,
    // applied correctly here from the start. Still genuinely settable
    // via PAX_setParameter, not a dead code path — that's exactly what
    // lets the settingsJson restoration mechanism repopulate it
    // immediately on a graph rebuild rather than sitting at 0 until the
    // next value happens to arrive.
    float currentValue = 0.f;
};

// ── Pax exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    // nodeType 1 (MIDI): 1 MIDI in, 1 MIDI out (unused-but-declared
    // workaround on the *input* side — see file header). Value input
    // declared separately below via PAX_getValueInputCount.
    static PAX_Descriptor d { "UDP Value to MIDI CC", "Patchy Examples", "1.0.0", 1, PAX_API_VERSION, 0, 0, 1, 1 };
    return &d;
}

PAX_Instance* PAX_create() { return new UdpValueToMidiCCPax(); }

void PAX_destroy (PAX_Instance* i) { delete static_cast<UdpValueToMidiCCPax*> (i); }

void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<UdpValueToMidiCCPax*> (i);

    int written = 0;

    // Pass through any (unwanted but declared) incoming MIDI unchanged —
    // see file header re: the nodeType=1 "can't declare zero MIDI
    // inputs" limitation.
    if (ctx->midiIn && ctx->midiOut)
    {
        for (int e = 0; e < ctx->midiInCount && written < ctx->midiMaxCount; ++e)
            ctx->midiOut[written++] = ctx->midiIn[e];
    }

    // Only emit (and only recompute currentValue) when a genuinely new
    // value arrives this block — deliberately not every block. Two
    // separate reasons stack here: (1) MIDI is a discrete-event
    // protocol, so continuously re-emitting the same CC every block
    // would be genuinely wrong for this protocol, not just wasteful —
    // unlike ValueToDMXPax/MidiToDmxPax, which correctly *do* rewrite
    // their DMX frame every block since DMX is a held/continuous signal;
    // (2) matching that same pair's own already-fixed reasoning,
    // currentValue can also be freshly restored via PAX_setParameter
    // after a graph rebuild, and recomputing unconditionally every block
    // would silently overwrite a just-restored value back to 0 on the
    // very next call.
    if (ctx->valuesIn && ctx->valueInCount > 0 && ctx->midiOut && written < ctx->midiMaxCount)
    {
        const PAX_Value& in = ctx->valuesIn[ctx->valueInCount - 1];

        // Two accepted packet shapes, additive — the original plain-float
        // one keeps working exactly as before, this doesn't replace it:
        //   - 4-byte plain float (PAX_DATA_FLOAT, UdpInDeviceNode's
        //     default for any 4-byte datagram): the *value* only — CC
        //     number comes from the configured "MIDI CC" parameter, same
        //     as this Pax has always worked.
        //   - 5-byte blob (PAX_DATA_BLOB, dataSize==5 — UdpInDeviceNode's
        //     existing "not exactly 4 bytes -> blob" rule already covers
        //     this without needing any change there): byte 0 is the CC
        //     number for *this specific message* (overriding the
        //     configured parameter), bytes 1-4 are the payload as a
        //     big-endian float, same convention as the plain-float case.
        //     This is what actually makes the node useful for driving
        //     different CC numbers dynamically from one UDP source,
        //     rather than being locked to whatever's configured — added
        //     2026-08-23 after the user pointed out the node was
        //     otherwise fairly limited in practice.
        int ccNum;
        float lastValue;
        if (in.dataType == PAX_DATA_BLOB && in.dataSize == 5)
        {
            ccNum     = std::clamp ((int) in.data[0], 0, 127);
            lastValue = bigEndianFloatFromBytes (in.data + 1);

            // Also update the configured "MIDI CC" parameter itself to
            // match, not just the local ccNum used for this one message
            // — this is what makes PAX_isParameterLiveSynced (see below)
            // actually have something to notice and push to the
            // frontend, moving the slider's own on-screen position to
            // follow the override live, exactly as the user asked for
            // ("even if one moves it, it will jump to the new value").
            // Deliberately still a completely normal, editable parameter
            // — this write doesn't make it read-only, it's just also
            // getting written from PAX_process() now, in addition to the
            // usual PAX_setParameter() from user interaction.
            a->midiCC = (float) ccNum;
        }
        else
        {
            ccNum     = std::clamp ((int) a->midiCC, 0, 127);
            lastValue = in.value;
        }

        // Map [inputMin, inputMax] to the project's own established
        // internal 0.0-1.0 convention (see file header), then scale to
        // an actual MIDI CC value (0-127) — currentValue stores that
        // real CC value directly, not the intermediate normalized
        // fraction, since a decimal has no real meaning in a MIDI CC
        // context.
        const float range = a->inputMax - a->inputMin;
        const float normalized = (range != 0.f)
            ? std::clamp ((lastValue - a->inputMin) / range, 0.f, 1.f)
            : 0.f;
        a->currentValue = std::clamp (std::lround (normalized * 127.f), 0L, 127L);

        const int channel = std::clamp ((int) a->midiChannel - 1, 0, 15);

        PAX_MidiEvent& out = ctx->midiOut[written++];
        out = {};
        out.byteCount = 3;
        out.bytes[0]  = (uint8_t) (0xB0 | channel);
        out.bytes[1]  = (uint8_t) ccNum;
        out.bytes[2]  = (uint8_t) a->currentValue;
    }

    if (ctx->midiOutCount) *ctx->midiOutCount = written;
    if (ctx->valueOutCount) *ctx->valueOutCount = 0;
}

int PAX_getParameterCount (PAX_Instance*) { return 5; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="MIDI CC";       info->minValue=0.f;     info->maxValue=127.f;  info->defaultValue=1.f; info->step=1.f; break;
        case 1: info->name="MIDI Channel";  info->minValue=1.f;     info->maxValue=16.f;   info->defaultValue=1.f; info->step=1.f; break;
        case 2: info->name="Input Min";     info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=0.f; info->step=0.f; break;
        case 3: info->name="Input Max";     info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=1.f; info->step=0.f; break;
        case 4: info->name="MIDI CC Value"; info->minValue=0.f;     info->maxValue=127.f;  info->defaultValue=0.f; info->step=1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<UdpValueToMidiCCPax*> (i);
    switch (index)
    {
        case 0: return a->midiCC;
        case 1: return a->midiChannel;
        case 2: return a->inputMin;
        case 3: return a->inputMax;
        case 4: return a->currentValue;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<UdpValueToMidiCCPax*> (i);
    switch (index)
    {
        case 0: a->midiCC      = std::clamp (value, 0.f, 127.f);   break;
        case 1: a->midiChannel = std::clamp (value, 1.f, 16.f);    break;
        case 2: a->inputMin    = std::clamp (value, -1000.f, 1000.f); break;
        case 3: a->inputMax    = std::clamp (value, -1000.f, 1000.f); break;
        // Genuinely settable, not ignored — see the currentValue member's
        // own comment for why (rebuild-restoration relies on this).
        case 4: a->currentValue = std::clamp (value, 0.f, 127.f);  break;
        default: break;
    }
}

// See PaxAPI.h's PAX_isParameterReadOnly doc — only "MIDI CC Value" (index
// 4) is read-only; every other parameter here is a normal editable control.
int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 4 ? 1 : 0; }

// See PaxAPI.h's PAX_isParameterLiveSynced doc — "MIDI CC" (index 0) stays
// a normal, editable slider, but a backend-side change to it (see the
// 5-byte packet's own override in PAX_process above) gets pushed to the
// frontend live, moving the slider to follow it. No other parameter here
// opts in — nothing else on this Pax is ever changed by PAX_process()
// itself, only by the user.
int PAX_isParameterLiveSynced (PAX_Instance*, int index) { return index == 0 ? 1 : 0; }

// Value ports — one generic input, no Value output (real output is MIDI).
int PAX_getValueInputCount() { return 1; }

// Per-port typing: this Pax's input is specifically UDP-typed, not
// generic Value — matching MqttToValuePax's own established precedent
// (see that file's header) for exactly the same reason: a generic Value
// input can't connect to a UDP-typed output at all under Patchy's strict
// port typing, so this declaration is what actually lets it connect to
// UdpInDeviceNode in the first place, not just a colour preference. A
// genuine oversight in the first version of this file, caught by the
// user noticing the input port wasn't rendering in UDP's own colour.
int PAX_getValueInputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_UDP : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
