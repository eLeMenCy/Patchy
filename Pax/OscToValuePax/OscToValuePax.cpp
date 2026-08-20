/**
 * OscToValuePax — OSC → Value adapter Pax (Phase 4)
 *
 * Bridges OSC data into the generic, protocol-agnostic "Value" graph — so
 * an OSC message's numeric argument can flow into any Value-consuming
 * node (Value → DMX, a future Value Math Pax, etc.) without that
 * downstream node needing to know anything about OSC addresses.
 *
 * Value only (nodeType 4), single input/output, same shape as
 * MqttToValuePax — this file follows that one's proven template closely,
 * adapted for OSC's actual PAX_Value shape rather than assumed by
 * symmetry with MQTT's. The two protocols don't populate PAX_Value quite
 * the same way:
 *   - MQTT always sets dataType = PAX_DATA_STRING (the topic lives in
 *     data[]) and separately, always populates `value` numerically
 *     alongside it, regardless of dataType.
 *   - OSC starts dataType = PAX_DATA_BLOB (the address lives in data[])
 *     and only switches it to PAX_DATA_FLOAT if the message's first
 *     argument was numeric (OSC type tag 'f' or 'i' — see
 *     OscDeviceNodes.h's parse()). A message with no numeric argument
 *     (no arguments at all, or a string argument, which currently
 *     overwrites data[] entirely rather than appending) leaves `value` at
 *     its zero-initialised default.
 * The practical effect on this adapter is identical either way: pass
 * `value` through unconditionally, exactly as MqttToValuePax does — 0.0f
 * is the correct, safe result for a message that had no numeric payload,
 * not a case this adapter needs to special-case or filter out.
 *
 * Design (matching MqttToValuePax's own confirmed-in-conversation scope):
 *   - Passes every incoming value through unconditionally — no address
 *     filter. Same reasoning as MQTT's topic: not expressible as a
 *     float-only Pax parameter, and out of scope for a v1 plain bridge.
 *   - Strips the OSC address on the way out — output is a clean
 *     PAX_TYPE_GENERIC value carrying just the number, so a downstream
 *     Value consumer never needs to know what an OSC address even is.
 *   - "Current Value" parameter (index 0, added 2026-08-17) — a read-only
 *     live display (see PaxAPI.h's PAX_isParameterReadOnly) mirroring the
 *     most recently converted value, added alongside ValueToDMXPax's own
 *     equivalent so the whole OSC → Value → DMX chain is visible end to
 *     end, not just at the final DMX step. Purely a visibility addition
 *     here, not a bug fix the way ValueToDMXPax's was — this Pax is a
 *     stateless passthrough with nothing held across blocks in the
 *     absence of new events, so there's no "survive a rebuild" concern:
 *     the display just naturally reflects whatever last came through,
 *     the same before or after a rebuild, exactly as expected for a
 *     discrete-event bridge.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC OscToValuePax.cpp -o OscToValuePax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC OscToValuePax.cpp -o OscToValuePax.so
 *   Win:    cl /std=c++20 /LD OscToValuePax.cpp /Fe:OscToValuePax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>

// ── Pax state ─────────────────────────────────────────────────────────────────
// lastValue is purely a display mirror for the read-only "Current Value"
// parameter (see file header) — the actual passthrough logic in
// PAX_process below is still fully stateless.
struct OscToValuePax { float lastValue = 0.f; };

// ── Pax exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "OSC to Value", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()                 { return new OscToValuePax(); }
void PAX_destroy (PAX_Instance* i)         { delete static_cast<OscToValuePax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<OscToValuePax*> (i);

    // Value only — nothing to do for audio/MIDI, but stay defensive per
    // the API's own guidance and always guard/zero what we don't use.
    if (ctx->midiOutCount) *ctx->midiOutCount = 0;

    if (! ctx->valuesOut || ctx->valueMaxCount <= 0 || ! ctx->valuesIn)
    {
        if (ctx->valueOutCount) *ctx->valueOutCount = 0;
        return;
    }

    int written = 0;
    for (int i2 = 0; i2 < ctx->valueInCount && written < ctx->valueMaxCount; ++i2)
    {
        const PAX_Value& in = ctx->valuesIn[i2];

        PAX_Value out {};
        out.type     = PAX_TYPE_GENERIC;   // strip the OSC domain tag — see file header
        out.key      = in.key;
        out.dataType = PAX_DATA_FLOAT;     // strip address/blob payload — value only
        out.value    = in.value;           // 0.0f default is correct for a non-numeric message

        ctx->valuesOut[written++] = out;
        a->lastValue = in.value;   // display mirror — most recent wins if several arrive this block
    }

    *ctx->valueOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 1; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    if (index == 0)
    {
        info->name = "Current Value"; info->minValue = -1000.f; info->maxValue = 1000.f;
        info->defaultValue = 0.f; info->step = 0.f;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<OscToValuePax*> (i);
    return index == 0 ? a->lastValue : 0.f;
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    // Genuinely settable, not ignored — same reasoning as
    // ValueToDMXPax's own read-only parameter, even though this one
    // isn't fixing a rebuild bug (see file header): a graph rebuild still
    // calls this to restore every saved parameter, and there's no reason
    // to special-case skipping it here just because this Pax's own
    // display naturally recovers on the next event either way.
    auto* a = static_cast<OscToValuePax*> (i);
    if (index == 0) a->lastValue = value;
}

// See PaxAPI.h's PAX_isParameterReadOnly doc — "Current Value" (index 0,
// the only parameter this Pax has) is read-only.
int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 0 ? 1 : 0; }

// Value port counts — see PaxAPI.h's PAX_getValueInputCount/OutputCount doc.
int PAX_getValueInputCount()  { return 1; }
int PAX_getValueOutputCount() { return 1; }

// Per-port typing: the single input port is specifically OSC-typed, not
// generic Value — this is what actually lets it connect to an OSC In
// device node at all. The output stays generic (PAX_VALUETYPE_GENERIC is
// the default for any index not explicitly declared, so
// PAX_getValueOutputType isn't even needed here) — the whole point of
// this adapter is bridging INTO the generic Value graph.
int PAX_getValueInputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_OSC : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
