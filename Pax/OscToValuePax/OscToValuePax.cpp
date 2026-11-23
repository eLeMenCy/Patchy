/**
 * OscToValuePax — OSC → Value converter (Phase 4; the Value hub's OSC side,
 * v0.0.995)
 *
 * Takes the numeric first argument of incoming OSC messages (int or float)
 * and outputs it as a generic Value. v0.0.995 brought it into the Value hub
 * (user, 2026-11-23), next to MIDI to Value:
 *
 *   Address (text parameter 0, new in the Pax API v0.0.995): only messages
 *     whose address matches pass; empty = every message. OSC-style wildcards:
 *     `*` any run of characters, `?` any one character (e.g. /fader*,
 *     /ch/??/mix).
 *   Parameters:
 *     0 Last        read-only: the last incoming number (kept at index 0 —
 *                   it was "Current Value" before, so older graphs load the same)
 *     1 / 2 Input Min / Max    what counts as 0 and 1 of the incoming number
 *     3 / 4 Output Min / Max   what 0 and 1 become on the way out
 *     5 Invert      0/1
 *   The defaults (0–1 → 0–1) pass the number through unchanged, exactly as
 *   before — the mapping is linear and not clamped, so a graph made before
 *   v0.0.995 behaves the same.
 *
 * A message without a numeric first argument gives 0 (as before). The OSC
 * address is stripped: the output is a plain generic Value.
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cstring>

struct OscToValuePax
{
    float last = 0.f, inMin = 0.f, inMax = 1.f, outMin = 0.f, outMax = 1.f, invert = 0.f;
    char  pattern[64] = { 0 };
};

// OSC-style match (kept simple): * = any run of characters,
// ? = one character, anything else literal. Empty pattern matches everything.
static bool oscMatch (const char* p, const char* s)
{
    if (*p == 0) return *s == 0;
    if (*p == '*') { for (;;) { if (oscMatch (p + 1, s)) return true; if (*s == 0) return false; ++s; } }
    if (*s == 0) return false;
    if (*p == '?' || *p == *s) return oscMatch (p + 1, s + 1);
    return false;
}

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "OSC to Value", "Patchy Examples", "1.1.0", 4, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()                 { return new OscToValuePax(); }
void PAX_destroy (PAX_Instance* i)         { delete static_cast<OscToValuePax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<OscToValuePax*> (i);
    if (ctx->midiOutCount) *ctx->midiOutCount = 0;
    int written = 0;

    if (ctx->valuesIn && ctx->valuesOut)
        for (int k = 0; k < ctx->valueInCount && written < ctx->valueMaxCount; ++k)
        {
            const PAX_Value& in = ctx->valuesIn[k];
            if (a->pattern[0] != 0)
            {
                char addr[57] = { 0 };
                if (in.dataSize > 0) std::memcpy (addr, in.data, std::min<size_t> (in.dataSize, sizeof (in.data)));
                if (! oscMatch (a->pattern, addr)) continue;
            }
            const float range = a->inMax - a->inMin;
            float n = range != 0.f ? (in.value - a->inMin) / range : 0.f;
            if (a->invert >= 0.5f) n = 1.f - n;

            PAX_Value out {};
            out.type     = PAX_TYPE_GENERIC;
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
    auto* a = static_cast<OscToValuePax*> (i);
    switch (index) { case 0: return a->last; case 1: return a->inMin; case 2: return a->inMax;
                     case 3: return a->outMin; case 4: return a->outMax; case 5: return a->invert; default: return 0.f; }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<OscToValuePax*> (i);
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

// ── text parameter (v0.0.995): the address filter ────────────────────────────
int  PAX_getTextParameterCount (PAX_Instance*) { return 1; }
void PAX_getTextParameterInfo (PAX_Instance*, int index, PAX_TextParameterInfo* info)
{
    if (index == 0) { info->name = "Address"; info->defaultValue = ""; info->maxLength = 63; }
}
void PAX_setTextParameter (PAX_Instance* i, int index, const char* utf8)
{
    auto* a = static_cast<OscToValuePax*> (i);
    if (index != 0) return;
    std::strncpy (a->pattern, utf8 ? utf8 : "", sizeof (a->pattern) - 1);
    a->pattern[sizeof (a->pattern) - 1] = 0;
}

int PAX_getValueInputCount()  { return 1; }
int PAX_getValueOutputCount() { return 1; }
int PAX_getValueInputType (int portIndex) { return portIndex == 0 ? PAX_VALUETYPE_OSC : PAX_VALUETYPE_GENERIC; }

} // extern "C"
