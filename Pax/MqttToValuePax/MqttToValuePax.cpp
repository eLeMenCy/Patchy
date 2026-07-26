/**
 * MqttToValuePax — MQTT → Value adapter Pax (Phase 4, first adapter)
 *
 * Bridges MQTT data into the generic, protocol-agnostic "Value" graph —
 * so an MQTT payload can flow into any Value-consuming node (a future
 * "Value Math" Pax, "Value → DMX", etc.) without that downstream node
 * needing to know anything about MQTT topics or brokers.
 *
 * Value only (nodeType 4) — no audio, no MIDI ports. First Pax to use the
 * PAX_getValueInputCount/PAX_getValueOutputCount optional exports
 * (see PaxAPI.h) rather than the audioInputs/midiInputs fields on
 * PAX_Descriptor, which only support the pre-existing MIDI/Audio/AV
 * nodeType categories. Also the first Pax to use the newer per-port
 * typing mechanism (PAX_getValueInputType) — its input port is
 * specifically MQTT-typed, not generic Value, which is what actually
 * lets it connect to an MQTT Subscribe/Publish node at all (a generic
 * Value input can't connect to an MQTT-typed output — Patchy's strict
 * port typing rejects the mismatch). Output stays generic, since the
 * whole point of this adapter is bridging INTO the generic Value graph.
 *
 * Single input, single output — no ambiguity in how data routes through
 * it. A future Pax with *multiple differently-typed* output ports would
 * need a separate, not-yet-built piece of work first (per-port-index-aware
 * value routing in ProcessingGraph.cpp, mirroring what audio already does
 * via port-index parsing — today, value events blind-copy to every
 * connected downstream node regardless of which port an edge represents,
 * which only matters once a node has more than one differently-routed
 * value output). Logged in Architecture.md as a follow-up; doesn't affect
 * this adapter at all given its single in/out shape.
 *
 * Design (confirmed in conversation before building):
 *   - Passes every incoming value through unconditionally — no topic
 *     filter. A per-message topic filter isn't expressible as a Pax
 *     parameter anyway (parameters are float-only, no string type), and
 *     the roadmap scope for v1 was a plain bridge, not a filter.
 *   - Strips the topic on the way out. The whole point of "Value" is to
 *     be protocol-agnostic — a downstream Value consumer shouldn't need
 *     to know what an MQTT topic even is. Output is a clean PAX_TYPE_GENERIC
 *     value carrying just the number.
 *   - Values with no numeric payload (dataType STRING/BLOB with no value
 *     set) still pass `value` through as-is (0.0f if never set) — no
 *     special-casing needed, since MqttSubscribeNode already always
 *     populates `value` numerically alongside the topic in data[].
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC MqttToValuePax.cpp -o MqttToValuePax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC MqttToValuePax.cpp -o MqttToValuePax.so
 *   Win:    cl /std=c++20 /LD MqttToValuePax.cpp /Fe:MqttToValuePax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>

// ── Pax state ─────────────────────────────────────────────────────────────────
// No per-instance state needed at all — this is a pure, stateless passthrough.
struct MqttToValuePax {};

// ── Pax exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "MQTT to Value", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()                 { return new MqttToValuePax(); }
void PAX_destroy (PAX_Instance* i)         { delete static_cast<MqttToValuePax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance*, const PAX_ProcessContext* ctx)
{
    // Value only — nothing to do for audio/MIDI, but stay defensive per
    // the API's own guidance and always guard/zero what we don't use.
    if (ctx->midiOutCount) *ctx->midiOutCount = 0;

    if (! ctx->valuesOut || ctx->valueMaxCount <= 0 || ! ctx->valuesIn)
    {
        if (ctx->valueOutCount) *ctx->valueOutCount = 0;
        return;
    }

    int written = 0;
    for (int i = 0; i < ctx->valueInCount && written < ctx->valueMaxCount; ++i)
    {
        const PAX_Value& in = ctx->valuesIn[i];

        PAX_Value out {};
        out.type     = PAX_TYPE_GENERIC;   // strip the MQTT domain tag — see file header
        out.key      = in.key;
        out.dataType = PAX_DATA_FLOAT;     // strip topic/string payload — value only
        out.value    = in.value;

        ctx->valuesOut[written++] = out;
    }

    *ctx->valueOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*)                          { return 0; }
void  PAX_getParameterInfo  (PAX_Instance*, int, PAX_ParameterInfo*) {}
float PAX_getParameter      (PAX_Instance*, int)                     { return 0.f; }
void  PAX_setParameter      (PAX_Instance*, int, float)              {}

// Value port counts — see PaxAPI.h's PAX_getValueInputCount/OutputCount doc.
int PAX_getValueInputCount()  { return 1; }
int PAX_getValueOutputCount() { return 1; }

// Per-port typing (added once the mechanism existed to declare it): the
// single input port is specifically MQTT-typed, not generic Value — this
// is what actually lets it connect to an MQTT Subscribe/Publish node at
// all. The output stays generic (PAX_VALUETYPE_GENERIC is the default for
// any index not explicitly declared, so PAX_getValueOutputType isn't even
// needed here) — the whole point of this adapter is bridging INTO the
// generic Value graph, so the output correctly stays untyped.
int PAX_getValueInputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_MQTT : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
