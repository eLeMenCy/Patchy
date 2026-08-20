/**
 * ValueToDMXPax — generic Value → DMX channel adapter (Phase 4)
 *
 * Downstream of the Value-typed converters (OscToValuePax, MqttToValuePax,
 * etc.) — takes whatever generic value they produce and drives one DMX
 * channel from it, completing a real end-to-end pipeline (an OSC message,
 * say, ending up as an actual DMX channel value on a fixture).
 *
 * Value only (nodeType 4), same shape as OscToValuePax/MqttToValuePax —
 * single generic Value input (no PAX_getValueInputType override needed;
 * PAX_VALUETYPE_GENERIC is the default for any port not explicitly
 * declared, which is exactly what's wanted here — this Pax accepts a
 * value from *any* generic-Value-producing source, not one specific
 * protocol).
 *
 * DMX output uses the dedicated DMX frame path (PaxAPI.h v4 —
 * ctx->dmxFrameOut/dmxFrameOutValid), matching AudioToDmxPax's own
 * established convention, not the general Value mechanism — that's what
 * DmxOutDeviceNode/DmxMonitorNode actually read. A minimal DMX-typed
 * Value is still mirrored alongside (portIndex 0), same reasoning as
 * AudioToDmxPax's own: purely so the port/edge-matching machinery shows
 * a proper DMX-coloured connection — the frame above is the real payload.
 *
 * Design:
 *   - Input Min/Max (defaults 0.0/1.0): incoming values from different
 *     sources land in wildly different ranges (an OSC sender might use
 *     0-1, 0-127, -1 to 1, whatever it chooses) — these two parameters
 *     map [Input Min, Input Max] to the project's own established
 *     internal convention of normalized 0.0-1.0 before scaling to a DMX
 *     byte (matching AudioToDmxPax's own "value normalized 0.0-1.0
 *     internally... matching the existing convention" — see that file's
 *     header). Defaulting to 0.0/1.0 means zero configuration needed for
 *     the common case where the upstream converter already sends
 *     normalized values.
 *   - Out-of-range inputs clamp to 0.0-1.0 after normalizing, same as
 *     AudioToDmxPax's own dmxVal clamp — a value outside [Input Min,
 *     Input Max] pins to the nearest DMX extreme rather than wrapping or
 *     producing an invalid byte.
 *   - The last received value persists across blocks where nothing new
 *     arrives (a member, not reset to 0 each call) — matches how a
 *     physical DMX channel actually behaves, holding its value until
 *     explicitly changed, not flickering to 0 between updates just
 *     because no new Value event happened to land in a given block.
 *   - Multiple values arriving in the same block: the last one wins (a
 *     simple, predictable v1 choice — no averaging or interpolation).
 *   - "Current Value" parameter (index 3, added 2026-08-17) — a read-only
 *     live display (see PaxAPI.h's PAX_isParameterReadOnly) mirroring the
 *     normalized value actually driving DMX. Exists specifically to fix a
 *     real, confirmed bug: ProcessingGraph::rebuild() destroys and
 *     recreates every node instance on any graph change (adding/deleting
 *     a node, undo/redo — anywhere in the graph, not just this node),
 *     which reset this Pax's held value to 0 until the next real incoming
 *     event happened to arrive — visibly, for 1-2 seconds, if the
 *     upstream source sends only occasionally rather than continuously.
 *     Exposing the value as a parameter lets it ride the same
 *     settingsJson restoration mechanism that already correctly restores
 *     DMX Channel/Input Min/Input Max across a rebuild (see
 *     ProcessingGraph.cpp). "Read-only" is a UI-level restriction only —
 *     PAX_setParameter still genuinely writes it, which is exactly what
 *     lets restoration work; see the currentValue member's own comment
 *     in the struct below for why PAX_process() had to change too (the
 *     value must NOT be unconditionally recomputed from lastValue every
 *     block, or a restored value gets silently overwritten back to 0 on
 *     the very next call, undoing the fix).
 *
 * Colour: no ports mirror (generic Value in, DMX-typed Value out), so the
 * Hybrid/Converter auto-detection rule correctly colours this node
 * Converter/fuchsia with no colourCategory override needed — same as
 * every other Phase 4 adapter so far.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC ValueToDMXPax.cpp -o ValueToDMXPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC ValueToDMXPax.cpp -o ValueToDMXPax.so
 *   Win:    cl /std=c++20 /LD ValueToDMXPax.cpp /Fe:ValueToDMXPax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>

// ── Pax state ─────────────────────────────────────────────────────────────────
struct ValueToDMXPax
{
    // Parameters
    float dmxChannel = 1.f;     // 1-based, 1-512 — see file header re: the frame path
    float inputMin   = 0.f;     // incoming value that maps to DMX 0
    float inputMax   = 1.f;     // incoming value that maps to DMX 255

    // State — persists the last received value across blocks; see file header
    float lastValue = 0.f;

    // Mirrors the normalized 0.0-1.0 value actually driving DMX right now
    // — exposed as a read-only parameter (index 3, see PAX_isParameterReadOnly
    // below) so the settings panel can show it as a live display. Still
    // genuinely settable via PAX_setParameter, not a dead code path — that's
    // exactly what lets ProcessingGraph::rebuild()'s existing settingsJson
    // restoration mechanism (see PaxRegistry.cpp) repopulate it immediately
    // on reconstruction, rather than sitting at 0 until the next real event
    // arrives. "Read-only" here is a UI-level restriction (no slider, no
    // user-initiated edits) — this Pax's own real value being present
    // across a rebuild in the first place is the whole point of the fix.
    float currentValue = 0.f;
};

// ── Pax exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Value to DMX", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create() { return new ValueToDMXPax(); }

void PAX_destroy (PAX_Instance* i) { delete static_cast<ValueToDMXPax*> (i); }

void PAX_prepare (PAX_Instance* i, double, int)
{
    auto* a = static_cast<ValueToDMXPax*> (i);
    a->lastValue = 0.f;
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<ValueToDMXPax*> (i);

    if (ctx->midiOutCount) *ctx->midiOutCount = 0;

    // Only recompute when a genuinely new value arrives this block —
    // critical, not just tidy: currentValue can also be freshly restored
    // via PAX_setParameter (after a graph rebuild — see that member's own
    // comment), and lastValue stays at its PAX_prepare()-reset 0 until a
    // real event arrives. Recomputing unconditionally every block, from
    // whatever lastValue currently holds, would silently overwrite a
    // just-restored currentValue back to 0 on the very next call — the
    // exact bug this parameter exists to fix. Trade-off, noted rather
    // than hidden: this means adjusting Input Min/Max alone, with no new
    // incoming value, doesn't retroactively re-scale a currently-held
    // value until the next real event — a minor UX difference from the
    // simpler always-recompute version, accepted deliberately since
    // surviving a rebuild is what was actually asked for here.
    if (ctx->valuesIn && ctx->valueInCount > 0)
    {
        a->lastValue = ctx->valuesIn[ctx->valueInCount - 1].value;

        // Map [inputMin, inputMax] to the project's own established
        // internal 0.0-1.0 convention before scaling to a DMX byte (see
        // file header).
        const float range = a->inputMax - a->inputMin;
        a->currentValue = (range != 0.f)
            ? std::clamp ((a->lastValue - a->inputMin) / range, 0.f, 1.f)
            : 0.f;
    }

    const uint8_t dmxByte = (uint8_t) std::clamp ((int) std::lround (a->currentValue * 255.f), 0, 255);

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

int PAX_getParameterCount (PAX_Instance*) { return 4; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="DMX Channel";   info->minValue=1.f;     info->maxValue=512.f;  info->defaultValue=1.f; info->step=1.f; break;
        case 1: info->name="Input Min";     info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=0.f; info->step=0.f; break;
        case 2: info->name="Input Max";     info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=1.f; info->step=0.f; break;
        case 3: info->name="Current Value"; info->minValue=0.f;     info->maxValue=1.f;    info->defaultValue=0.f; info->step=0.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<ValueToDMXPax*> (i);
    switch (index)
    {
        case 0: return a->dmxChannel;
        case 1: return a->inputMin;
        case 2: return a->inputMax;
        case 3: return a->currentValue;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<ValueToDMXPax*> (i);
    switch (index)
    {
        case 0: a->dmxChannel   = std::clamp (value, 1.f, 512.f);      break;
        case 1: a->inputMin     = std::clamp (value, -1000.f, 1000.f); break;
        case 2: a->inputMax     = std::clamp (value, -1000.f, 1000.f); break;
        // Genuinely settable, not ignored — see the currentValue member's
        // own comment for why (rebuild-restoration relies on this).
        case 3: a->currentValue = std::clamp (value, 0.f, 1.f);        break;
        default: break;
    }
}

// See PaxAPI.h's PAX_isParameterReadOnly doc — only "Current Value" (index
// 3) is read-only; every other parameter here is a normal editable control.
int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 3 ? 1 : 0; }

// Value ports — one generic input, one DMX-typed output.
int PAX_getValueInputCount()  { return 1; }
int PAX_getValueOutputCount() { return 1; }

int PAX_getValueOutputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_DMX : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
