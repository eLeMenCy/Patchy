/**
 * ValueToMqttPax — Value → MQTT converter (v0.0.996, the Value hub's MQTT side)
 *
 * Any generic Value in → an MQTT message out, to an MQTT Publish node (user,
 * 2026-11-24 — Home Assistant, Zigbee2MQTT, Tasmota, Shelly compatibility a
 * must).
 *
 *   Topic (text parameter 0): e.g. home/livingroom/lamp/set. Empty = the
 *     MQTT Publish node's own topic.
 *   Parameters:
 *     0 Type        0 Float ("23.5") · 1 Int ("24") · 2 ON/OFF ("ON" when the
 *                   input, mapped to 0–1, is at or above Threshold, else
 *                   "OFF" — switches and relays, Home Assistant style)
 *     1 / 2 Input Min / Max    what counts as 0 and 1 of the incoming value
 *     3 / 4 Output Min / Max   what 0 and 1 become in the payload (Float / Int)
 *     5 Invert      0/1
 *     6 On change only  0/1
 *     7 Max rate    messages per second at most (0 = no limit); the latest
 *                   held-back value is sent as soon as allowed
 *     8 Last sent   read-only (ON/OFF: 1 / 0)
 *     9 Threshold   0–1, ON/OFF only
 *   The mapping is linear and not clamped. QoS and retain stay on the
 *   MQTT Publish node.
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>

struct ValueToMqttPax
{
    float type = 0.f, inMin = 0.f, inMax = 1.f, outMin = 0.f, outMax = 1.f, invert = 0.f,
          onChange = 1.f, maxRate = 50.f, lastSent = 0.f, threshold = 0.5f;
    char  topic[64] = "patchy/value";

    double sampleRate = 48000.0, sinceSend = 1e9;
    bool   pending = false, haveSent = false;
    float  pendingIn = 0.f, lastOut = 0.f;
    int    lastType = -1;
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Value to MQTT", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()                 { return new ValueToMqttPax(); }
void PAX_destroy (PAX_Instance* i)         { delete static_cast<ValueToMqttPax*> (i); }
void PAX_prepare (PAX_Instance* i, double sr, int) { if (sr > 0.0) static_cast<ValueToMqttPax*> (i)->sampleRate = sr; }

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<ValueToMqttPax*> (i);
    if (ctx->midiOutCount) *ctx->midiOutCount = 0;
    int written = 0;
    a->sinceSend += (ctx->numSamples > 0 ? ctx->numSamples : 512) / a->sampleRate;

    const int type = std::clamp ((int) std::lround (a->type), 0, 2);
    if (type != a->lastType) { a->lastType = type; a->haveSent = false; }

    if (ctx->valuesIn && ctx->valueInCount > 0)   // the newest value of this block
    {
        a->pendingIn = ctx->valuesIn[ctx->valueInCount - 1].value;
        a->pending   = true;
    }

    if (a->pending)
    {
        const float range = a->inMax - a->inMin;
        float n = range != 0.f ? (a->pendingIn - a->inMin) / range : 0.f;
        if (a->invert >= 0.5f) n = 1.f - n;
        float v = a->outMin + n * (a->outMax - a->outMin);
        if (type == 1) v = (float) std::lround (v);
        if (type == 2) v = n >= a->threshold ? 1.f : 0.f;   // ON / OFF: the input against Threshold

        if (a->onChange >= 0.5f && a->haveSent && v == a->lastOut)
            a->pending = false;                                   // nothing new to say
        else if (a->maxRate > 0.f && a->sinceSend < 1.0 / a->maxRate)
            ;                                                     // too soon: keep it
        else if (ctx->valuesOut && written < ctx->valueMaxCount)
        {
            PAX_Value& o = ctx->valuesOut[written++];
            o = {};
            o.type     = PAX_TYPE_MQTT;
            o.dataType = type == 1 ? PAX_DATA_INT : type == 2 ? PAX_DATA_BOOL : PAX_DATA_FLOAT;
            o.value    = v;
            const size_t len = std::strlen (a->topic);
            if (len > 0)
            {
                std::memcpy (o.data, a->topic, std::min (len, sizeof (o.data) - 1));
                o.dataSize = (uint16_t) (std::min (len, sizeof (o.data) - 1) + 1);
            }
            a->lastOut = v; a->lastSent = v; a->haveSent = true;
            a->sinceSend = 0.0; a->pending = false;
        }
    }

    if (ctx->valueOutCount) *ctx->valueOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 10; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name = "Type";           info->minValue = 0.f;   info->maxValue = 2.f;   info->defaultValue = 0.f;  info->step = 1.f; break;
        case 1: info->name = "Input Min";      info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 0.f;  info->step = 0.f; break;
        case 2: info->name = "Input Max";      info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 1.f;  info->step = 0.f; break;
        case 3: info->name = "Output Min";     info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 0.f;  info->step = 0.f; break;
        case 4: info->name = "Output Max";     info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 1.f;  info->step = 0.f; break;
        case 5: info->name = "Invert";         info->minValue = 0.f;   info->maxValue = 1.f;   info->defaultValue = 0.f;  info->step = 1.f; break;
        case 6: info->name = "On change only"; info->minValue = 0.f;   info->maxValue = 1.f;   info->defaultValue = 1.f;  info->step = 1.f; break;
        case 7: info->name = "Max rate";       info->minValue = 0.f;   info->maxValue = 100.f; info->defaultValue = 50.f; info->step = 1.f; break;
        case 8: info->name = "Last sent";      info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 0.f;  info->step = 0.f; break;
        case 9: info->name = "Threshold";      info->minValue = 0.f;   info->maxValue = 1.f;   info->defaultValue = 0.5f; info->step = 0.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<ValueToMqttPax*> (i);
    switch (index)
    {
        case 0: return a->type;   case 1: return a->inMin;  case 2: return a->inMax;
        case 3: return a->outMin; case 4: return a->outMax; case 5: return a->invert;
        case 6: return a->onChange; case 7: return a->maxRate; case 8: return a->lastSent; case 9: return a->threshold;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<ValueToMqttPax*> (i);
    switch (index)
    {
        case 0: a->type     = std::clamp (std::round (v), 0.f, 2.f); break;
        case 1: a->inMin    = v; break;
        case 2: a->inMax    = v; break;
        case 3: a->outMin   = v; break;
        case 4: a->outMax   = v; break;
        case 5: a->invert   = v >= 0.5f ? 1.f : 0.f; break;
        case 6: a->onChange = v >= 0.5f ? 1.f : 0.f; break;
        case 7: a->maxRate  = std::clamp (v, 0.f, 100.f); break;
        case 8: a->lastSent = v; break;   // restored on rebuild
        case 9: a->threshold = std::clamp (v, 0.f, 1.f); break;
        default: break;
    }
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 8 ? 1 : 0; }

// ── text parameter: the topic ─────────────────────────────────────────────────
int  PAX_getTextParameterCount (PAX_Instance*) { return 1; }
void PAX_getTextParameterInfo (PAX_Instance*, int index, PAX_TextParameterInfo* info)
{
    if (index == 0) { info->name = "Topic"; info->defaultValue = "patchy/value"; info->maxLength = 55; }   // data[] holds 56 with the 0
}
void PAX_setTextParameter (PAX_Instance* i, int index, const char* utf8)
{
    auto* a = static_cast<ValueToMqttPax*> (i);
    if (index != 0) return;
    std::strncpy (a->topic, utf8 ? utf8 : "", 55);
    a->topic[55] = 0;
    a->haveSent = false;   // a new topic: the next value goes there
}

int PAX_getValueInputCount()  { return 1; }   // generic Value
int PAX_getValueOutputCount() { return 1; }
int PAX_getValueOutputType (int portIndex) { return portIndex == 0 ? PAX_VALUETYPE_MQTT : PAX_VALUETYPE_GENERIC; }

} // extern "C"
