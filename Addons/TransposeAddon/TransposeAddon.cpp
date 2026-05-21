/**
 * TransposeAddon — MIDI Node addon example
 *
 * Transposes all note-on and note-off messages by a variable number of semitones.
 * Demonstrates parameters + the minimal structure of a MIDI-type addon.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC TransposeAddon.cpp -o TransposeAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC TransposeAddon.cpp -o TransposeAddon.so
 *   Win:    cl /std:c++20 /LD TransposeAddon.cpp /Fe:TransposeAddon.dll
 */

#include "../AddonAPI.h"
#include <algorithm>
#include <cmath>

struct TransposeAddon
{
    float semitones = 0.0f;   // -24 to +24, default 0
};

extern "C" {

const NGA_Descriptor* NGA_getDescriptor()
{
    static NGA_Descriptor d {
        "Transpose",
        "Patchy Examples",
        "1.1.0",
        1,               // nodeType: MIDI
        NGA_API_VERSION
    };
    return &d;
}

NGA_Instance* NGA_create()           { return new TransposeAddon(); }
void NGA_destroy (NGA_Instance* i)   { delete static_cast<TransposeAddon*> (i); }
void NGA_prepare (NGA_Instance*, double, int) {}

void NGA_process (NGA_Instance* i,
                  float**, float**, int, int,
                  const NGA_MidiEvent* midiIn,  int midiInCount,
                  NGA_MidiEvent*       midiOut, int* midiOutCount, int midiMaxCount)
{
    auto* node    = static_cast<TransposeAddon*> (i);
    int   shift   = (int) std::round (node->semitones);
    int   written = 0;

    for (int e = 0; e < midiInCount && written < midiMaxCount; ++e)
    {
        midiOut[written] = midiIn[e];

        if (midiIn[e].byteCount >= 2)
        {
            const uint8_t status  = midiIn[e].bytes[0] & 0xF0;
            const bool    isNote  = (status == 0x90 || status == 0x80);
            if (isNote)
            {
                int note = midiIn[e].bytes[1] + shift;
                midiOut[written].bytes[1] = (uint8_t) std::clamp (note, 0, 127);
            }
        }
        ++written;
    }

    *midiOutCount = written;
}

int NGA_getParameterCount (NGA_Instance*) { return 1; }

void NGA_getParameterInfo (NGA_Instance*, int index, NGA_ParameterInfo* info)
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

float NGA_getParameter (NGA_Instance* i, int index)
{
    if (index == 0) return static_cast<TransposeAddon*> (i)->semitones;
    return 0.f;
}

void NGA_setParameter (NGA_Instance* i, int index, float value)
{
    if (index == 0)
        static_cast<TransposeAddon*> (i)->semitones = std::clamp (value, -24.0f, 24.0f);
}

} // extern "C"
