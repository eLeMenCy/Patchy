/**
 * StereoSplitterAddon — Audio Node addon
 *
 * Splits a stereo input into two mono outputs:
 *   Audio In  → left channel  → Audio Out 1
 *             → right channel → Audio Out 2
 *
 * Parameters:
 *   Balance — -1.0 (full left) to +1.0 (full right), default 0.0 (centre)
 *             At centre: L and R pass through unchanged.
 *             Negative: L boosted, R attenuated.
 *             Positive: R boosted, L attenuated.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC StereoSplitterAddon.cpp -o StereoSplitterAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC StereoSplitterAddon.cpp -o StereoSplitterAddon.so
 *   Win:    cl /std:c++20 /LD StereoSplitterAddon.cpp /Fe:StereoSplitterAddon.dll
 */

#include "../AddonAPI.h"
#include <cstring>
#include <algorithm>
#include <cmath>

struct StereoSplitterAddon
{
    float balance = 0.0f;   // -1.0 (full left) to +1.0 (full right)

    // Returns gain for left and right channels based on balance
    void gains (float& leftGain, float& rightGain) const
    {
        // Simple linear pan law:
        // balance = -1 → L=1.0, R=0.0
        // balance =  0 → L=1.0, R=1.0 (unity both)
        // balance = +1 → L=0.0, R=1.0
        leftGain  = std::max (0.0f, 1.0f - balance);
        rightGain = std::max (0.0f, 1.0f + balance);
        // Normalise so centre = unity
        leftGain  = std::min (leftGain,  1.0f);
        rightGain = std::min (rightGain, 1.0f);
    }
};

extern "C" {

const NGA_Descriptor* NGA_getDescriptor()
{
    static NGA_Descriptor d {
        "Splitter",        // name
        "Patchy Examples", // vendor
        "1.1.0",           // version
        2,                 // nodeType: Audio
        NGA_API_VERSION,   // apiVersion
        1,                 // audioInputs:  1 stereo in
        2,                 // audioOutputs: 2 mono out (L + R)
        0,                 // midiInputs
        0                  // midiOutputs
    };
    return &d;
}

NGA_Instance* NGA_create()           { return new StereoSplitterAddon(); }
void NGA_destroy (NGA_Instance* i)   { delete static_cast<StereoSplitterAddon*> (i); }
void NGA_prepare (NGA_Instance*, double, int) {}

void NGA_process (NGA_Instance* i,
                  float** audioIn, float** audioOut,
                  int /*numChannels*/, int numSamples,
                  const NGA_MidiEvent*, int,
                  NGA_MidiEvent*, int* outCount, int)
{
    *outCount = 0;
    if (! audioIn || ! audioOut) return;

    auto* a = static_cast<StereoSplitterAddon*> (i);
    float lGain, rGain;
    a->gains (lGain, rGain);

    // Audio Out 1 = Left channel × left gain — mono: copy to both channels
    if (audioIn[0] && audioOut[0])
        for (int s = 0; s < numSamples; ++s)
            audioOut[0][s] = audioIn[0][s] * lGain;
    if (audioIn[0] && audioOut[1])   // ch1 of port 0 = same as ch0
        for (int s = 0; s < numSamples; ++s)
            audioOut[1][s] = audioIn[0][s] * lGain;

    // Audio Out 2 = Right channel × right gain — mono: copy to both channels
    if (audioIn[1] && audioOut[2])
        for (int s = 0; s < numSamples; ++s)
            audioOut[2][s] = audioIn[1][s] * rGain;
    if (audioIn[1] && audioOut[3])   // ch1 of port 1 = same as ch0
        for (int s = 0; s < numSamples; ++s)
            audioOut[3][s] = audioIn[1][s] * rGain;
}

int NGA_getParameterCount (NGA_Instance*) { return 1; }

void NGA_getParameterInfo (NGA_Instance*, int index, NGA_ParameterInfo* info)
{
    if (index == 0)
    {
        info->name         = "Balance";
        info->minValue     = -1.0f;
        info->maxValue     =  1.0f;
        info->defaultValue =  0.0f;
        info->step         =  0.0f;   // continuous
    }
}

float NGA_getParameter (NGA_Instance* i, int index)
{
    return index == 0 ? static_cast<StereoSplitterAddon*> (i)->balance : 0.f;
}

void NGA_setParameter (NGA_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<StereoSplitterAddon*> (i)->balance = std::clamp (value, -1.0f, 1.0f);
}

} // extern "C"
