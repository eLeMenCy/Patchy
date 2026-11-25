/**
 * MqttToValuePax — MQTT → Value converter (Phase 4; the Value hub's MQTT
 * side, v0.0.996)
 *
 * Takes the numeric payload of incoming MQTT messages and outputs it as a
 * generic Value. v0.0.996 brought it into the Value hub (user, 2026-11-24 —
 * Home Assistant, Zigbee2MQTT, Tasmota, Shelly a must):
 *
 *   Topic (text parameter 0): only messages whose topic matches pass; empty
 *     = every message. MQTT wildcards: `+` one level (home/+/temp), `#` this
 *     level and everything below, last (home/#).
 *   Parameters:
 *     0 Last        read-only: the last incoming number
 *     1 / 2 Input Min / Max    what counts as 0 and 1 of the incoming number
 *     3 / 4 Output Min / Max   what 0 and 1 become on the way out
 *     5 Invert      0/1
 *   Linear and not clamped; the defaults (0–1 → 0–1) pass the number
 *   through unchanged, so a graph made before v0.0.996 behaves the same.
 *
 * Payloads: numbers, and since v0.0.996 the words ON / OFF, true / false
 * (MQTT Subscribe turns them into 1 / 0). JSON payloads: the next round.
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cstring>

struct MqttToValuePax
{
    float last = 0.f, inMin = 0.f, inMax = 1.f, outMin = 0.f, outMax = 1.f, invert = 0.f;
    char  pattern[128] = { 0 };
};

// MQTT topic filter: levels split on '/'; '+' matches exactly one level,
// '#' (last) matches this level and everything below — including the parent
// itself ("home/#" matches "home"), as the MQTT spec says.
static bool mqttMatch (const char* f, const char* t)
{
    for (;;)
    {
        if (f[0] == '#' && f[1] == 0) return true;
        if (f[0] == '+' && (f[1] == '/' || f[1] == 0))
        {
            while (*t && *t != '/') ++t;            // skip one level
            ++f;
        }
        else
        {
            while (*f && *f != '/') { if (*f != *t) return false; ++f; ++t; }
            if (*t && *t != '/') return false;      // the topic's level is longer
        }
        if (*f == 0) return *t == 0;
        // f is at '/'
        if (*t == 0) return f[1] == '#' && f[2] == 0;   // "home/#" matches "home"
        ++f; ++t;
    }
}

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "MQTT to Value", "Patchy Examples", "1.1.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()                 { return new MqttToValuePax(); }
void PAX_destroy (PAX_Instance* i)         { delete static_cast<MqttToValuePax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<MqttToValuePax*> (i);
    if (ctx->midiOutCount) *ctx->midiOutCount = 0;
    int written = 0;

    if (ctx->valuesIn && ctx->valuesOut)
        for (int k = 0; k < ctx->valueInCount && written < ctx->valueMaxCount; ++k)
        {
            const PAX_Value& in = ctx->valuesIn[k];
            if (a->pattern[0] != 0)
            {
                char topic[57] = { 0 };
                std::memcpy (topic, in.data, std::min<size_t> (in.dataSize, sizeof (in.data)));
                if (! mqttMatch (a->pattern, topic)) continue;
            }
            const float range = a->inMax - a->inMin;
            float n = range != 0.f ? (in.value - a->inMin) / range : 0.f;
            if (a->invert >= 0.5f) n = 1.f - n;

            PAX_Value out {};
            out.type     = PAX_TYPE_GENERIC;   // the topic is stripped: a plain Value
            out.dataType = PAX_DATA_FLOAT;
            out.value    = a->outMin + n * (a->outMax - a->outMin);
            ctx->valuesOut[written++] = out;
            a->last = in.value;
        }

    if (ctx->valueOutCount) *ctx->valueOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 6; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name = "Last";       info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 1: info->name = "Input Min";  info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 2: info->name = "Input Max";  info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 1.f; info->step = 0.f; break;
        case 3: info->name = "Output Min"; info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 4: info->name = "Output Max"; info->minValue = -1e6f; info->maxValue = 1e6f; info->defaultValue = 1.f; info->step = 0.f; break;
        case 5: info->name = "Invert";     info->minValue = 0.f;   info->maxValue = 1.f;  info->defaultValue = 0.f; info->step = 1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<MqttToValuePax*> (i);
    switch (index) { case 0: return a->last; case 1: return a->inMin; case 2: return a->inMax;
                     case 3: return a->outMin; case 4: return a->outMax; case 5: return a->invert; default: return 0.f; }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<MqttToValuePax*> (i);
    switch (index)
    {
        case 0: a->last   = v; break;   // restored on rebuild
        case 1: a->inMin  = v; break;
        case 2: a->inMax  = v; break;
        case 3: a->outMin = v; break;
        case 4: a->outMax = v; break;
        case 5: a->invert = v >= 0.5f ? 1.f : 0.f; break;
        default: break;
    }
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 0 ? 1 : 0; }

// ── text parameter (v0.0.996): the topic filter ────────────────────────────
int  PAX_getTextParameterCount (PAX_Instance*) { return 1; }
void PAX_getTextParameterInfo (PAX_Instance*, int index, PAX_TextParameterInfo* info)
{
    if (index == 0) { info->name = "Topic"; info->defaultValue = ""; info->maxLength = 127; }
}
void PAX_setTextParameter (PAX_Instance* i, int index, const char* utf8)
{
    auto* a = static_cast<MqttToValuePax*> (i);
    if (index != 0) return;
    std::strncpy (a->pattern, utf8 ? utf8 : "", sizeof (a->pattern) - 1);
    a->pattern[sizeof (a->pattern) - 1] = 0;
}

int PAX_getValueInputCount()  { return 1; }
int PAX_getValueOutputCount() { return 1; }
int PAX_getValueInputType (int portIndex) { return portIndex == 0 ? PAX_VALUETYPE_MQTT : PAX_VALUETYPE_GENERIC; }

} // extern "C"
