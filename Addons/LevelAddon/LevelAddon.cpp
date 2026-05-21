/**
 * LevelAddon — Audio Node addon
 *
 * Gain control from -60 dB (silence) to +6 dB.
 * Use for level control, attenuation and mixing.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC LevelAddon.cpp -o LevelAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC LevelAddon.cpp -o LevelAddon.so
 *   Win:    cl /std:c++20 /LD LevelAddon.cpp /Fe:LevelAddon.dll
 */

#include "../AddonAPI.h"
#include <cmath>
#include <algorithm>

struct LevelAddon
{
    float gainDb = 0.0f;   // -60 to +6 dB, default 0 dB (unity)

    float linearGain() const
    {
        if (gainDb <= -60.0f) return 0.0f;
        return std::pow (10.0f, gainDb / 20.0f);
    }
};

extern "C" {

const NGA_Descriptor* NGA_getDescriptor()
{
    static NGA_Descriptor d { "Level", "Patchy Examples", "1.0.0", 2, NGA_API_VERSION };
    return &d;
}

NGA_Instance* NGA_create()           { return new LevelAddon(); }
void NGA_destroy (NGA_Instance* i)   { delete static_cast<LevelAddon*> (i); }
void NGA_prepare (NGA_Instance*, double, int) {}

void NGA_process (NGA_Instance* i,
                  float** audioIn, float** audioOut,
                  int numChannels, int numSamples,
                  const NGA_MidiEvent*, int,
                  NGA_MidiEvent*, int* outCount, int)
{
    *outCount = 0;
    if (! audioIn || ! audioOut) return;
    const float gain = static_cast<LevelAddon*> (i)->linearGain();
    for (int ch = 0; ch < numChannels; ++ch)
        if (audioIn[ch] && audioOut[ch])
            for (int s = 0; s < numSamples; ++s)
                audioOut[ch][s] = audioIn[ch][s] * gain;
}

int  NGA_getParameterCount (NGA_Instance*) { return 1; }

void NGA_getParameterInfo (NGA_Instance*, int index, NGA_ParameterInfo* info)
{
    if (index == 0) {
        info->name = "Level (dB)"; info->minValue = -60.0f;
        info->maxValue = 6.0f; info->defaultValue = 0.0f; info->step = 0.1f;
    }
}

float NGA_getParameter (NGA_Instance* i, int index)
{
    return index == 0 ? static_cast<LevelAddon*> (i)->gainDb : 0.f;
}

void NGA_setParameter (NGA_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<LevelAddon*> (i)->gainDb = std::clamp (value, -60.0f, 6.0f);
}

} // extern "C"
