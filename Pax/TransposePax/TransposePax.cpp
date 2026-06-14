/**
 * TransposePax — MIDI Node Pax example
 *
 * Transposes all note-on and note-off messages by a variable number of semitones.
 * Demonstrates parameters + the minimal structure of a MIDI-type Pax.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC TransposePax.cpp -o TransposeAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC TransposePax.cpp -o TransposePax.so
 *   Win:    cl /std:c++20 /LD TransposePax.cpp /Fe:TransposePax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>

struct TransposePax
{
    float semitones = 0.0f;   // -24 to +24, default 0
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d {
        "Transpose",
        "Patchy Examples",
        "1.1.0",
        1,               // nodeType: MIDI
        PAX_API_VERSION
    };
    return &d;
}

PAX_Instance* PAX_create()           { return new TransposePax(); }
void PAX_destroy (PAX_Instance* i)   { delete static_cast<TransposePax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* node    = static_cast<TransposePax*> (i);
    int   shift   = (int) std::round (node->semitones);
    int   written = 0;

    for (int e = 0; e < ctx->midiInCount && written < ctx->midiMaxCount; ++e)
    {
        ctx->midiOut[written] = ctx->midiIn[e];

        if (ctx->midiIn[e].byteCount >= 2)
        {
            const uint8_t status  = ctx->midiIn[e].bytes[0] & 0xF0;
            const bool    isNote  = (status == 0x90 || status == 0x80);
            if (isNote)
            {
                int note = ctx->midiIn[e].bytes[1] + shift;
                ctx->midiOut[written].bytes[1] = (uint8_t) std::clamp (note, 0, 127);
            }
        }
        ++written;
    }

    *ctx->midiOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 1; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    if (index == 0)
    {
        info->name         = "Semitones";
        info->minValue     = -24.0f;
        info->maxValue     =  24.0f;
        info->defaultValue =   0.0f;
        info->step         =   1.0f;   // integer semitones
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    if (index == 0) return static_cast<TransposePax*> (i)->semitones;
    return 0.f;
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<TransposePax*> (i)->semitones = std::clamp (value, -24.0f, 24.0f);
}

} // extern "C"
