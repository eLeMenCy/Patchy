/**
 * ValueConsolePax — Value Console (v0.0.992): faders that send Values
 *
 * The Value hub needed a way to inject a value by hand (user, 2026-11-20):
 * up to 16 faders, each with its own Value Out port, 1 shown to start and
 * "+ fader" up to the Preferences maximum (like the MIDI Keyboard's faders).
 * Each fader:
 *   Value (0–1, the fader's position), Low / High (what 0 and 1 send — the
 *   two-head range), Ramp (0/1: sweeps Low → High → Low on its own, a
 *   triangle), Period (seconds per full sweep), Out (read-only: the value sent
 *   last — the node draws the fader there while it ramps).
 * The node's Step button is UI-side: it jumps the fader to 0 or 1.
 *
 * Parameters: 0 Count (faders shown, 1–16), then 6 per fader from index 1:
 *   1 + 6k + { 0 Value, 1 Low, 2 High, 3 Ramp, 4 Period, 5 Out (ro) }.
 *
 * Sends on change only (a fader at rest sends nothing), once on load so what's
 * downstream starts from the console's state, and every block while ramping.
 * Only the faders shown send.
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static constexpr int kFaders = 16;
static constexpr int kPer    = 6;

struct Fader { float value = 0.f, low = 0.f, high = 1.f, ramp = 0.f, period = 4.f, out = 0.f; double phase = 0.0; bool sent = false; float lastSent = 0.f; };

struct ValueConsolePax
{
    float  count = 1.f;
    float  rampsOnOpen = 0.f;   // v0.0.996 — read by the host on open, nothing to do here
    Fader  f[kFaders];
    double sampleRate = 48000.0;
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Value Console", "Patchy Examples", "1.0.0", 4, PAX_API_VERSION };   // 4 = Value only
    return &d;
}

PAX_Instance* PAX_create()                 { return new ValueConsolePax(); }
void          PAX_destroy (PAX_Instance* i) { delete static_cast<ValueConsolePax*> (i); }
void          PAX_prepare (PAX_Instance* i, double sr, int) { if (sr > 0.0) static_cast<ValueConsolePax*> (i)->sampleRate = sr; }

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<ValueConsolePax*> (i);
    const double dt = (ctx->numSamples > 0 ? ctx->numSamples : 512) / a->sampleRate;
    const int n = std::clamp ((int) std::lround (a->count), 1, kFaders);
    int written = 0;

    for (int k = 0; k < n; ++k)
    {
        Fader& f = a->f[k];
        float pos = f.value;
        if (f.ramp >= 0.5f)
        {
            f.phase = std::fmod (f.phase + dt / std::max (0.1f, f.period), 1.0);
            pos = (float) (f.phase < 0.5 ? f.phase * 2.0 : 2.0 - f.phase * 2.0);   // triangle 0 → 1 → 0
        }
        const float v = f.low + std::clamp (pos, 0.f, 1.f) * (f.high - f.low);
        f.out = v;
        if (f.sent && std::fabs (v - f.lastSent) < 1e-6f) continue;
        if (! ctx->valuesOut || written >= ctx->valueMaxCount) continue;
        PAX_Value& o = ctx->valuesOut[written++];
        o = {};
        o.type      = PAX_TYPE_GENERIC;
        o.dataType  = PAX_DATA_FLOAT;
        o.value     = v;
        o.portIndex = k;
        f.sent = true;
        f.lastSent = v;
    }
    if (ctx->valueOutCount) *ctx->valueOutCount = written;
    if (ctx->midiOutCount)  *ctx->midiOutCount  = 0;
}

// + 1: "Ramps on open" (index 97, v0.0.996) — 0 (default) = a file or session
// opens with every ramp off (the host clears them, PatchyProcessor.cpp's
// setStateInformation); 1 = running ramps start again on open
int PAX_getParameterCount (PAX_Instance*) { return 1 + kFaders * kPer + 1; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    static char names[kFaders * kPer][24];
    static bool init = false;
    if (! init)
    {
        const char* what[kPer] = { "Value", "Low", "High", "Ramp", "Period", "Out" };
        for (int k = 0; k < kFaders; ++k)
            for (int j = 0; j < kPer; ++j)
                std::snprintf (names[k * kPer + j], sizeof (names[0]), "%s %d", what[j], k + 1);
        init = true;
    }
    if (index == 0) { info->name = "Count"; info->minValue = 1.f; info->maxValue = (float) kFaders; info->defaultValue = 1.f; info->step = 1.f; return; }
    if (index == 1 + kFaders * kPer) { info->name = "Ramps on open"; info->minValue = 0.f; info->maxValue = 1.f; info->defaultValue = 0.f; info->step = 1.f; return; }
    const int k = (index - 1) / kPer, j = (index - 1) % kPer;
    if (k >= kFaders) return;
    info->name = names[k * kPer + j];
    switch (j)
    {
        case 0: info->minValue = 0.f;     info->maxValue = 1.f;    info->defaultValue = 0.f; info->step = 0.f; break;
        case 1: info->minValue = -1000.f; info->maxValue = 1000.f; info->defaultValue = 0.f; info->step = 0.f; break;
        case 2: info->minValue = -1000.f; info->maxValue = 1000.f; info->defaultValue = 1.f; info->step = 0.f; break;
        case 3: info->minValue = 0.f;     info->maxValue = 1.f;    info->defaultValue = 0.f; info->step = 1.f; break;
        case 4: info->minValue = 0.1f;    info->maxValue = 60.f;   info->defaultValue = 4.f; info->step = 0.f; break;
        case 5: info->minValue = -1000.f; info->maxValue = 1000.f; info->defaultValue = 0.f; info->step = 0.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<ValueConsolePax*> (i);
    if (index == 0) return a->count;
    if (index == 1 + kFaders * kPer) return a->rampsOnOpen;
    const int k = (index - 1) / kPer, j = (index - 1) % kPer;
    if (k < 0 || k >= kFaders) return 0.f;
    const Fader& f = a->f[k];
    switch (j) { case 0: return f.value; case 1: return f.low; case 2: return f.high; case 3: return f.ramp; case 4: return f.period; case 5: return f.out; default: return 0.f; }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<ValueConsolePax*> (i);
    if (index == 0) { a->count = std::clamp (v, 1.f, (float) kFaders); return; }
    if (index == 1 + kFaders * kPer) { a->rampsOnOpen = v >= 0.5f ? 1.f : 0.f; return; }
    const int k = (index - 1) / kPer, j = (index - 1) % kPer;
    if (k < 0 || k >= kFaders) return;
    Fader& f = a->f[k];
    switch (j)
    {
        case 0: f.value  = std::clamp (v, 0.f, 1.f);           break;
        case 1: f.low    = std::clamp (v, -1000.f, 1000.f);    break;
        case 2: f.high   = std::clamp (v, -1000.f, 1000.f);    break;
        case 3: { const bool on = v >= 0.5f;
                  if (on && f.ramp < 0.5f) f.phase = f.value * 0.5;   // start the sweep from where the fader is
                  f.ramp = on ? 1.f : 0.f; break; }
        case 4: f.period = std::clamp (v, 0.1f, 60.f);         break;
        case 5: f.out    = v;                                   break;   // restored on rebuild
        default: break;
    }
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return (index >= 1 && index < 1 + kFaders * kPer && (index - 1) % kPer == 5) ? 1 : 0; }

int PAX_getValueOutputCount() { return kFaders; }   // one generic Value Out per fader

} // extern "C"
