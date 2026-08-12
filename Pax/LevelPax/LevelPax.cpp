/**
 * LevelPax — Audio Node Pax
 *
 * Gain control from -60 dB (silence) to +6 dB.
 * Use for level control, attenuation and mixing.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC LevelPax.cpp -o LevelPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC LevelPax.cpp -o LevelPax.so
 *   Win:    cl /std:c++20 /LD LevelPax.cpp /Fe:LevelPax.dll
 */

#include "../PaxAPI.h"
#include <cmath>
#include <algorithm>

struct LevelPax
{
    float gainDb = 0.0f;   // -60 to +6 dB, default 0 dB (unity)

    float linearGain() const
    {
        if (gainDb <= -60.0f) return 0.0f;
        return std::pow (10.0f, gainDb / 20.0f);
    }
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Level", "Patchy Examples", "1.0.0", 2, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()           { return new LevelPax(); }
void PAX_destroy (PAX_Instance* i)   { delete static_cast<LevelPax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    if (! ctx->audioIn || ! ctx->audioOut) return;
    const float gain = static_cast<LevelPax*> (i)->linearGain();
    for (int ch = 0; ch < ctx->numChannels; ++ch)
        if (ctx->audioIn[ch] && ctx->audioOut[ch])
            for (int s = 0; s < ctx->numSamples; ++s)
                ctx->audioOut[ch][s] = ctx->audioIn[ch][s] * gain;
}

int  PAX_getParameterCount (PAX_Instance*) { return 1; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    if (index == 0) {
        info->name = "Level (dB)"; info->minValue = -60.0f;
        info->maxValue = 6.0f; info->defaultValue = 0.0f; info->step = 0.1f;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    return index == 0 ? static_cast<LevelPax*> (i)->gainDb : 0.f;
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<LevelPax*> (i)->gainDb = std::clamp (value, -60.0f, 6.0f);
}

} // extern "C"
