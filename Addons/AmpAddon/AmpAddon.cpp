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

const NGA_Descriptor* NGA_getDescriptor()
{
    static NGA_Descriptor d { "Amp", "Patchy Examples", "1.0.0", 2, NGA_API_VERSION };
    return &d;
}

NGA_Instance* NGA_create()           { return new AmpAddon(); }
void NGA_destroy (NGA_Instance* i)   { delete static_cast<AmpAddon*> (i); }
void NGA_prepare (NGA_Instance*, double, int) {}

void NGA_process (NGA_Instance* i,
                  float** audioIn, float** audioOut,
                  int numChannels, int numSamples,
                  const NGA_MidiEvent*, int,
                  NGA_MidiEvent*, int* outCount, int)
{
    *outCount = 0;
    if (! audioIn || ! audioOut) return;
    const float gain = static_cast<AmpAddon*> (i)->linearGain();
    for (int ch = 0; ch < numChannels; ++ch)
        if (audioIn[ch] && audioOut[ch])
            for (int s = 0; s < numSamples; ++s)
                audioOut[ch][s] = audioIn[ch][s] * gain;
}

int  NGA_getParameterCount (NGA_Instance*) { return 1; }

void NGA_getParameterInfo (NGA_Instance*, int index, NGA_ParameterInfo* info)
{
    if (index == 0) {
        info->name = "Amp (dB)"; info->minValue = 0.0f;
        info->maxValue = 24.0f; info->defaultValue = 0.0f; info->step = 0.1f;
    }
}

float NGA_getParameter (NGA_Instance* i, int index)
{
    return index == 0 ? static_cast<AmpAddon*> (i)->gainDb : 0.f;
}

void NGA_setParameter (NGA_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<AmpAddon*> (i)->gainDb = std::clamp (value, 0.0f, 24.0f);
}

} // extern "C"
