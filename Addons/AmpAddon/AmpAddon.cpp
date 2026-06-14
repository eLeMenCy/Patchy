/**
 * AmpAddon — Audio Node addon
 *
 * Amplifier from 0 dB (unity) to +24 dB.
 * Use to boost signals, drive levels up or compensate for weak sources.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC AmpAddon.cpp -o AmpAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC AmpAddon.cpp -o AmpAddon.so
 *   Win:    cl /std:c++20 /LD AmpAddon.cpp /Fe:AmpAddon.dll
 */

#include "../AddonAPI.h"
#include <cmath>
#include <algorithm>

struct AmpAddon
{
    float gainDb = 0.0f;   // 0 to +24 dB, default 0 dB (unity)

    float linearGain() const
    {
        return std::pow (10.0f, gainDb / 20.0f);
    }
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Amp", "Patchy Examples", "1.0.0", 2, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()           { return new AmpAddon(); }
void PAX_destroy (PAX_Instance* i)   { delete static_cast<AmpAddon*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    if (! ctx->audioIn || ! ctx->audioOut) return;
    const float gain = static_cast<AmpAddon*> (i)->linearGain();
    for (int ch = 0; ch < ctx->numChannels; ++ch)
        if (ctx->audioIn[ch] && ctx->audioOut[ch])
            for (int s = 0; s < ctx->numSamples; ++s)
                ctx->audioOut[ch][s] = ctx->audioIn[ch][s] * gain;
}

int  PAX_getParameterCount (PAX_Instance*) { return 1; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    if (index == 0) {
        info->name = "Amp (dB)"; info->minValue = 0.0f;
        info->maxValue = 24.0f; info->defaultValue = 0.0f; info->step = 0.1f;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    return index == 0 ? static_cast<AmpAddon*> (i)->gainDb : 0.f;
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<AmpAddon*> (i)->gainDb = std::clamp (value, 0.0f, 24.0f);
}

} // extern "C"
