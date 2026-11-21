/**
 * MidiToValuePax — MIDI → Value converter (v0.0.992, the Value hub's MIDI side)
 *
 * The converter matrix (2026-11-19) settled on a "Value hub": every protocol
 * gets an X → Value and a Value → X Pax, so any pair is two nodes (wrapped
 * into one box when wanted) instead of ~40 direct converters. This is MIDI's
 * way in.
 *
 * Reads one kind of MIDI message and outputs it as a generic Value, mapped
 * from its MIDI range onto [Output Min, Output Max] (default 0–1, the
 * project's internal convention — the Value → X side maps it on from there).
 *
 *   Message (index 0): 0 CC · 1 Note velocity (note off = 0) · 2 Note number ·
 *                      3 Pitch bend · 4 Aftertouch (channel) · 5 Program
 *   Number  (index 1): -1 = any, else the CC number (CC) or the note (Note
 *                      velocity); ignored by the others
 *   Channel (index 2): 0 = any, else 1–16
 *   Output Min / Max (3 / 4)
 *   Last    (index 5): read-only — the last raw MIDI value matched (0–127;
 *                      pitch bend −8192…8191)
 *   Invert  (index 6): 0/1 — MIDI high gives Output Min (v0.0.992: the
 *                      range's heads no longer cross; inverting is this)
 *   Channels (index 7): v0.0.993 — several channels (user): a 16-bit mask,
 *                      bit 0 = channel 1, 65535 = any. 0 = not set: Channel
 *                      (index 2) applies, as in graphs saved before.
 *
 * Discrete, like MIDI itself: a Value goes out only when a matching message
 * arrives (several in one block → each one, up to the host's limit).
 *
 * MIDI passthrough: a nodeType 1 Pax always has a MIDI out (see
 * MidiToDmxPax's header); the incoming MIDI is passed through unchanged, so
 * the node can sit inline.
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>

struct MidiToValuePax
{
    float message = 0.f;    // 0..5, see header
    float number  = -1.f;   // -1 = any
    float channel = 0.f;    // 0 = any
    float outMin  = 0.f;
    float outMax  = 1.f;
    float last    = 0.f;    // read-only display
    float invert  = 0.f;
    float chMask  = 0.f;    // v0.0.993 — 0 = use channel
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "MIDI to Value", "Patchy Examples", "1.0.0", 1, PAX_API_VERSION, 0, 0, 1, 1 };
    return &d;
}

PAX_Instance* PAX_create()                 { return new MidiToValuePax(); }
void          PAX_destroy (PAX_Instance* i) { delete static_cast<MidiToValuePax*> (i); }
void          PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<MidiToValuePax*> (i);
    int midiWritten = 0, valWritten = 0;

    if (ctx->midiIn && ctx->midiOut)
        for (int e = 0; e < ctx->midiInCount && midiWritten < ctx->midiMaxCount; ++e)
            ctx->midiOut[midiWritten++] = ctx->midiIn[e];

    const int msgKind = std::clamp ((int) std::lround (a->message), 0, 5);
    const int number  = (int) std::lround (a->number);
    const int chan    = (int) std::lround (a->channel);
    const int mask    = (int) std::lround (a->chMask) & 0xFFFF;
    const int accept  = mask != 0 ? mask : (chan >= 1 && chan <= 16 ? 1 << (chan - 1) : 0xFFFF);

    if (ctx->midiIn)
    {
        for (int e = 0; e < ctx->midiInCount; ++e)
        {
            const auto& m = ctx->midiIn[e];
            if (m.byteCount < 1) continue;
            const int status = m.bytes[0] & 0xF0;
            const int ch     = (m.bytes[0] & 0x0F) + 1;
            const int d1     = m.byteCount > 1 ? (m.bytes[1] & 0x7F) : 0;
            const int d2     = m.byteCount > 2 ? (m.bytes[2] & 0x7F) : 0;
            if (status < 0x80 || status == 0xF0) continue;   // not a channel message
            if (! ((accept >> (ch - 1)) & 1)) continue;

            bool  match = false;
            float raw = 0.f, norm = 0.f;
            switch (msgKind)
            {
                case 0:   // CC
                    if (status == 0xB0 && (number < 0 || d1 == number)) { match = true; raw = (float) d2; norm = d2 / 127.f; }
                    break;
                case 1:   // Note velocity — note off (or note on vel 0) = 0
                    if ((status == 0x90 || status == 0x80) && (number < 0 || d1 == number))
                    {
                        const int vel = status == 0x90 ? d2 : 0;
                        match = true; raw = (float) vel; norm = vel / 127.f;
                    }
                    break;
                case 2:   // Note number — note ons only
                    if (status == 0x90 && d2 > 0) { match = true; raw = (float) d1; norm = d1 / 127.f; }
                    break;
                case 3:   // Pitch bend
                    if (status == 0xE0)
                    {
                        const int bend = ((d2 << 7) | d1) - 8192;   // -8192 … 8191
                        match = true; raw = (float) bend; norm = (bend + 8192) / 16383.f;
                    }
                    break;
                case 4:   // Channel aftertouch
                    if (status == 0xD0) { match = true; raw = (float) d1; norm = d1 / 127.f; }
                    break;
                case 5:   // Program change
                    if (status == 0xC0) { match = true; raw = (float) d1; norm = d1 / 127.f; }
                    break;
                default: break;
            }
            if (! match) continue;

            a->last = raw;
            if (a->invert >= 0.5f) norm = 1.f - norm;
            if (ctx->valuesOut && valWritten < ctx->valueMaxCount)
            {
                PAX_Value& out = ctx->valuesOut[valWritten++];
                out = {};
                out.type      = PAX_TYPE_GENERIC;
                out.dataType  = PAX_DATA_FLOAT;
                out.value     = a->outMin + std::clamp (norm, 0.f, 1.f) * (a->outMax - a->outMin);
                out.portIndex = 0;
            }
        }
    }

    if (ctx->midiOutCount)  *ctx->midiOutCount  = midiWritten;
    if (ctx->valueOutCount) *ctx->valueOutCount = valWritten;
}

int PAX_getParameterCount (PAX_Instance*) { return 8; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="Message";    info->minValue=0.f;     info->maxValue=5.f;    info->defaultValue=0.f;  info->step=1.f; break;
        case 1: info->name="Number";     info->minValue=-1.f;    info->maxValue=127.f;  info->defaultValue=-1.f; info->step=1.f; break;
        case 2: info->name="Channel";    info->minValue=0.f;     info->maxValue=16.f;   info->defaultValue=0.f;  info->step=1.f; break;
        case 3: info->name="Output Min"; info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=0.f;  info->step=0.f; break;
        case 4: info->name="Output Max"; info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=1.f;  info->step=0.f; break;
        case 5: info->name="Last";       info->minValue=-8192.f; info->maxValue=8191.f; info->defaultValue=0.f;  info->step=1.f; break;
        case 6: info->name="Invert";     info->minValue=0.f;     info->maxValue=1.f;    info->defaultValue=0.f;  info->step=1.f; break;
        case 7: info->name="Channels";   info->minValue=0.f;     info->maxValue=65535.f;info->defaultValue=0.f;  info->step=1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<MidiToValuePax*> (i);
    switch (index)
    {
        case 0: return a->message;
        case 1: return a->number;
        case 2: return a->channel;
        case 3: return a->outMin;
        case 4: return a->outMax;
        case 5: return a->last;
        case 6: return a->invert;
        case 7: return a->chMask;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<MidiToValuePax*> (i);
    switch (index)
    {
        case 0: a->message = std::clamp (v, 0.f, 5.f);          break;
        case 1: a->number  = std::clamp (v, -1.f, 127.f);       break;
        case 2: a->channel = std::clamp (v, 0.f, 16.f);         break;
        case 3: a->outMin  = std::clamp (v, -1000.f, 1000.f);   break;
        case 4: a->outMax  = std::clamp (v, -1000.f, 1000.f);   break;
        case 5: a->last    = std::clamp (v, -8192.f, 8191.f);   break;   // restored on rebuild
        case 6: a->invert  = v >= 0.5f ? 1.f : 0.f;             break;
        case 7: a->chMask  = std::clamp (std::round (v), 0.f, 65535.f); break;
        default: break;
    }
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 5 ? 1 : 0; }

int PAX_getValueOutputCount() { return 1; }   // generic Value (the default type)

} // extern "C"
