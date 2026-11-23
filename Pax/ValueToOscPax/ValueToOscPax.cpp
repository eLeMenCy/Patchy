/**
 * ValueToOscPax — Value → OSC converter (v0.0.995, the Value hub's OSC side)
 *
 * Any generic Value in → an OSC message out, to an OSC Out Device (user,
 * 2026-11-23).
 *
 *   Address (text parameter 0 — text parameters are new in the Pax API,
 *     v0.0.995): where the message goes, e.g. /mixer/fader1. Empty = the OSC
 *     Out Device's own address.
 *   Parameters:
 *     0 Type        0 Float · 1 Int (the mapped value rounded, sent with the
 *                   'i' type tag — some receivers, e.g. X32 / M32 switches,
 *                   cue or clip numbers, ignore a float where they expect an int)
 *     1 / 2 Input Min / Max    what counts as 0 and 1 of the incoming value
 *     3 / 4 Output Min / Max   what 0 and 1 become in the message
 *     5 Invert      0/1
 *     6 On change only  0/1 — nothing sent when the value to send is the same
 *     7 Max rate    messages per second at most (0 = no limit); a value held
 *                   back isn't lost: the latest is sent as soon as allowed
 *     8 Last sent   read-only
 *   The mapping is linear and not clamped (as OSC to Value).
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>
#include <cstring>

struct ValueToOscPax
{
    float type = 0.f, inMin = 0.f, inMax = 1.f, outMin = 0.f, outMax = 1.f, invert = 0.f,
          onChange = 1.f, maxRate = 50.f, lastSent = 0.f;
    char  address[64] = "/patchy/value";

    double sampleRate = 48000.0, sinceSend = 1e9;
    bool   pending = false, haveSent = false;
    float  pendingIn = 0.f, lastOut = 0.f;
    int    lastType = -1;
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Value to OSC", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()                 { return new ValueToOscPax(); }
void PAX_destroy (PAX_Instance* i)         { delete static_cast<ValueToOscPax*> (i); }
void PAX_prepare (PAX_Instance* i, double sr, int) { if (sr > 0.0) static_cast<ValueToOscPax*> (i)->sampleRate = sr; }

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<ValueToOscPax*> (i);
    if (ctx->midiOutCount) *ctx->midiOutCount = 0;
    int written = 0;
    a->sinceSend += (ctx->numSamples > 0 ? ctx->numSamples : 512) / a->sampleRate;

    const int type = a->type >= 0.5f ? 1 : 0;
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

        if (a->onChange >= 0.5f && a->haveSent && v == a->lastOut)
            a->pending = false;                                   // nothing new to say
        else if (a->maxRate > 0.f && a->sinceSend < 1.0 / a->maxRate)
            ;                                                     // too soon: keep it
        else if (ctx->valuesOut && written < ctx->valueMaxCount)
        {
            PAX_Value& o = ctx->valuesOut[written++];
            o = {};
            o.type     = PAX_TYPE_OSC;
            o.dataType = type == 1 ? PAX_DATA_INT : PAX_DATA_FLOAT;
            o.value    = v;
            const size_t len = std::strlen (a->address);
            if (len > 0)
            {
                std::memcpy (o.data, a->address, std::min (len, sizeof (o.data) - 1));
                o.dataSize = (uint16_t) (std::min (len, sizeof (o.data) - 1) + 1);
            }
            a->lastOut = v; a->lastSent = v; a->haveSent = true;
            a->sinceSend = 0.0; a->pending = false;
        }
    }

    if (ctx->valueOutCount) *ctx->valueOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 9; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name = "Type";           info->minValue = 0.f;   info->maxValue = 1.f;   info->defaultValue = 0.f;  info->step = 1.f; break;
        case 1: info->name = "Input Min";      info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 0.f;  info->step = 0.f; break;
        case 2: info->name = "Input Max";      info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 1.f;  info->step = 0.f; break;
        case 3: info->name = "Output Min";     info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 0.f;  info->step = 0.f; break;
        case 4: info->name = "Output Max";     info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 1.f;  info->step = 0.f; break;
        case 5: info->name = "Invert";         info->minValue = 0.f;   info->maxValue = 1.f;   info->defaultValue = 0.f;  info->step = 1.f; break;
        case 6: info->name = "On change only"; info->minValue = 0.f;   info->maxValue = 1.f;   info->defaultValue = 1.f;  info->step = 1.f; break;
        case 7: info->name = "Max rate";       info->minValue = 0.f;   info->maxValue = 100.f; info->defaultValue = 50.f; info->step = 1.f; break;
        case 8: info->name = "Last sent";      info->minValue = -1e6f; info->maxValue = 1e6f;  info->defaultValue = 0.f;  info->step = 0.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<ValueToOscPax*> (i);
    switch (index)
    {
        case 0: return a->type;   case 1: return a->inMin;  case 2: return a->inMax;
        case 3: return a->outMin; case 4: return a->outMax; case 5: return a->invert;
        case 6: return a->onChange; case 7: return a->maxRate; case 8: return a->lastSent;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<ValueToOscPax*> (i);
    switch (index)
    {
        case 0: a->type     = v >= 0.5f ? 1.f : 0.f; break;
        case 1: a->inMin    = v; break;
        case 2: a->inMax    = v; break;
        case 3: a->outMin   = v; break;
        case 4: a->outMax   = v; break;
        case 5: a->invert   = v >= 0.5f ? 1.f : 0.f; break;
        case 6: a->onChange = v >= 0.5f ? 1.f : 0.f; break;
        case 7: a->maxRate  = std::clamp (v, 0.f, 100.f); break;
        case 8: a->lastSent = v; break;   // restored on rebuild
        default: break;
    }
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 8 ? 1 : 0; }

// ── text parameter (v0.0.995): the address ───────────────────────────────────
int  PAX_getTextParameterCount (PAX_Instance*) { return 1; }
void PAX_getTextParameterInfo (PAX_Instance*, int index, PAX_TextParameterInfo* info)
{
    if (index == 0) { info->name = "Address"; info->defaultValue = "/patchy/value"; info->maxLength = 55; }   // data[] holds 56 with the 0
}
void PAX_setTextParameter (PAX_Instance* i, int index, const char* utf8)
{
    auto* a = static_cast<ValueToOscPax*> (i);
    if (index != 0) return;
    std::strncpy (a->address, utf8 ? utf8 : "", 55);
    a->address[55] = 0;
    a->haveSent = false;   // a new address: the next value goes there
}

int PAX_getValueInputCount()  { return 1; }   // generic Value
int PAX_getValueOutputCount() { return 1; }
int PAX_getValueOutputType (int portIndex) { return portIndex == 0 ? PAX_VALUETYPE_OSC : PAX_VALUETYPE_GENERIC; }

} // extern "C"
