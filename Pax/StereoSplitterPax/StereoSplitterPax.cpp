/**
 * StereoSplitterPax — Audio Node Pax
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
 *   macOS:  clang++ -std=c++20 -shared -fPIC StereoSplitterPax.cpp -o StereoSplitterPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC StereoSplitterPax.cpp -o StereoSplitterPax.so
 *   Win:    cl /std:c++20 /LD StereoSplitterPax.cpp /Fe:StereoSplitterPax.dll
 */

#include "../PaxAPI.h"
#include <cstring>
#include <algorithm>
#include <cmath>

struct StereoSplitterPax
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

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d {
        "Splitter",        // name
        "Patchy Examples", // vendor
        "1.1.0",           // version
        2,                 // nodeType: Audio
        PAX_API_VERSION,   // apiVersion
        1,                 // audioInputs:  1 stereo in
        2,                 // audioOutputs: 2 mono out (L + R)
        0,                 // midiInputs
        0                  // midiOutputs
    };
    return &d;
}

PAX_Instance* PAX_create()           { return new StereoSplitterPax(); }
void PAX_destroy (PAX_Instance* i)   { delete static_cast<StereoSplitterPax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    if (! ctx->audioIn || ! ctx->audioOut) return;

    auto* a = static_cast<StereoSplitterPax*> (i);
    float lGain, rGain;
    a->gains (lGain, rGain);

    // Port 0 (Audio Out 1): Left channel → both ch0 and ch1 (mono)
    if (ctx->audioIn[0] && ctx->audioOut[0])
        for (int s = 0; s < ctx->numSamples; ++s)
            ctx->audioOut[0][s] = ctx->audioIn[0][s] * lGain;
    if (ctx->audioIn[0] && ctx->audioOut[1])
        for (int s = 0; s < ctx->numSamples; ++s)
            ctx->audioOut[1][s] = ctx->audioIn[0][s] * lGain;

    // Port 1 (Audio Out 2): Right channel → both ch0 and ch1 (mono)
    if (ctx->audioIn[1] && ctx->audioOut[2])
        for (int s = 0; s < ctx->numSamples; ++s)
            ctx->audioOut[2][s] = ctx->audioIn[1][s] * rGain;
    if (ctx->audioIn[1] && ctx->audioOut[3])
        for (int s = 0; s < ctx->numSamples; ++s)
            ctx->audioOut[3][s] = ctx->audioIn[1][s] * rGain;
}

int PAX_getParameterCount (PAX_Instance*) { return 1; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
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

float PAX_getParameter (PAX_Instance* i, int index)
{
    return index == 0 ? static_cast<StereoSplitterPax*> (i)->balance : 0.f;
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<StereoSplitterPax*> (i)->balance = std::clamp (value, -1.0f, 1.0f);
}

} // extern "C"
