/**
 * ValueToMidiPax — Value → MIDI converter (v0.0.992, the Value hub's MIDI side)
 *
 * Any generic Value in → one kind of MIDI message out. [Input Min, Input Max]
 * maps the incoming value to 0–1 first (the project's convention), then:
 *
 *   Message (index 0):
 *     0 CC         — CC #Number = value × 127
 *     1 Note gate  — the value crossing Threshold (0–1, after the input
 *                    mapping) upwards plays note #Number at Velocity; back
 *                    under it releases it (user: "both" note modes)
 *     2 Note pitch — the value picks the note (0–1 → 0–127), played at
 *                    Velocity; a new note releases the previous one
 *     3 Pitch bend — 0–1 → full bend range (0.5 = centre)
 *     4 Program    — 0–1 → program 0–127
 *   Number (1), Channel (2, 1–16), Input Min / Max (3 / 4), Threshold (5),
 *   Velocity (6), On change only (7, 0/1), Max rate (8, per second, 0 = no
 *   limit), Last sent (9, read-only: the MIDI value last sent), Invert (10,
 *   0/1: Input Max gives MIDI 0).
 *
 * Rate (user, 2026-11-19: "yes — adjustable"): with "On change only" a
 * message goes out only when its MIDI value differs from the last one sent;
 * Max rate spaces messages at least 1/rate s apart. A value held back by the
 * rate limit isn't lost: the latest one is kept and sent as soon as allowed,
 * so the last value always lands. Note on / off are never dropped: the gate
 * only ever sends on a state change, and a held note is released when the
 * message type, note or channel changes.
 *
 * Channels (index 11, v0.0.993 — user): a 16-bit mask, bit 0 = channel 1;
 * every message goes out on each channel set (65535 = all 16, "Any" / omni).
 * 0 = not set: Channel (index 2) applies, as in graphs saved before. The rate
 * limit counts a send to all its channels as one; held notes are tracked on
 * the channels they were played on and released there.
 *
 * MIDI is discrete: nothing is re-sent every block. MIDI passthrough as in
 * every nodeType 1 Pax (see MidiToDmxPax's header).
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>

struct ValueToMidiPax
{
    // parameters
    float message   = 0.f;
    float number    = 1.f;
    float channel   = 1.f;
    float inMin     = 0.f;
    float inMax     = 1.f;
    float threshold = 0.5f;
    float velocity  = 100.f;
    float onChange  = 1.f;
    float maxRate   = 50.f;
    float lastSent  = 0.f;    // read-only display
    float invert    = 0.f;
    float chMask    = 0.f;    // v0.0.993 — 0 = use channel

    // state
    double sampleRate  = 48000.0;
    double sinceSend   = 1e9;   // seconds since the last message went out
    bool   pending     = false;
    float  pendingNorm = 0.f;
    bool   haveSent    = false;
    int    lastKey     = -100000;
    int    lastKind    = -1;
    int    heldNote    = -1;    // a note we started (gate / pitch modes)
    int    lastMask    = -1;
    int    heldMask    = 0;     // the channels it sounds on (bit 0 = channel 1)
    bool   gateOn      = false;
};

static void addMidi (const PAX_ProcessContext* ctx, int& written, int b0, int b1, int b2, int count = 3)
{
    if (! ctx->midiOut || written >= ctx->midiMaxCount) return;
    PAX_MidiEvent& e = ctx->midiOut[written++];
    e = {};
    e.byteCount = count;
    e.bytes[0] = (uint8_t) b0;
    e.bytes[1] = (uint8_t) (b1 & 0x7F);
    if (count > 2) e.bytes[2] = (uint8_t) (b2 & 0x7F);
}

static void releaseHeld (ValueToMidiPax* a, const PAX_ProcessContext* ctx, int& written)
{
    if (a->heldNote >= 0)
        for (int c = 0; c < 16; ++c)
            if ((a->heldMask >> c) & 1) addMidi (ctx, written, 0x80 | c, a->heldNote, 0);
    a->heldNote = -1;
    a->gateOn   = false;
}

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Value to MIDI", "Patchy Examples", "1.0.0", 1, PAX_API_VERSION, 0, 0, 1, 1 };
    return &d;
}

PAX_Instance* PAX_create()                 { return new ValueToMidiPax(); }
void          PAX_destroy (PAX_Instance* i) { delete static_cast<ValueToMidiPax*> (i); }

void PAX_prepare (PAX_Instance* i, double sampleRate, int)
{
    auto* a = static_cast<ValueToMidiPax*> (i);
    if (sampleRate > 0.0) a->sampleRate = sampleRate;
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<ValueToMidiPax*> (i);
    int written = 0;

    if (ctx->midiIn && ctx->midiOut)
        for (int e = 0; e < ctx->midiInCount && written < ctx->midiMaxCount; ++e)
            ctx->midiOut[written++] = ctx->midiIn[e];

    a->sinceSend += (ctx->numSamples > 0 ? ctx->numSamples : 512) / a->sampleRate;

    const int kind    = std::clamp ((int) std::lround (a->message), 0, 4);
    const int number  = std::clamp ((int) std::lround (a->number), 0, 127);
    const int channel = std::clamp ((int) std::lround (a->channel), 1, 16) - 1;
    const int mask    = ((int) std::lround (a->chMask) & 0xFFFF) != 0 ? ((int) std::lround (a->chMask) & 0xFFFF) : 1 << channel;
    auto toAll = [&] (int status, int d1, int d2, int count = 3)
    { for (int c = 0; c < 16; ++c) if ((mask >> c) & 1) addMidi (ctx, written, status | c, d1, d2, count); };
    const int vel     = std::clamp ((int) std::lround (a->velocity), 1, 127);

    // A held note goes when what it belongs to changes (type, gate note, channel)
    if (a->heldNote >= 0 && (kind != a->lastKind || a->heldMask != mask
                             || (kind == 1 && a->heldNote != number) || (kind != 1 && kind != 2)))
        releaseHeld (a, ctx, written);
    if (kind != a->lastKind) { a->lastKind = kind; a->haveSent = false; }
    if (mask != a->lastMask) { a->lastMask = mask; a->haveSent = false; }   // v0.0.993 — new channels: the next value goes out there

    // The newest value of this block (earlier ones in the same block are superseded)
    if (ctx->valuesIn && ctx->valueInCount > 0)
    {
        const float v     = ctx->valuesIn[ctx->valueInCount - 1].value;
        const float range = a->inMax - a->inMin;
        a->pendingNorm = range != 0.f ? std::clamp ((v - a->inMin) / range, 0.f, 1.f) : 0.f;
        if (a->invert >= 0.5f) a->pendingNorm = 1.f - a->pendingNorm;
        a->pending     = true;
    }

    if (a->pending)
    {
        const float n = a->pendingNorm;
        int key = 0;
        switch (kind)
        {
            case 0: key = (int) std::lround (n * 127.f);       break;
            case 1: key = n >= a->threshold ? 1 : 0;            break;
            case 2: key = (int) std::lround (n * 127.f);       break;
            case 3: key = (int) std::lround (n * 16383.f);     break;
            case 4: key = (int) std::lround (n * 127.f);       break;
            default: break;
        }
        const bool same   = a->haveSent && key == a->lastKey;
        const bool gate   = kind == 1;
        if ((gate || a->onChange >= 0.5f) && same)
            a->pending = false;                                   // nothing new to say
        else if (a->maxRate > 0.f && a->sinceSend < 1.0 / a->maxRate)
            ;                                                     // too soon: keep it, send when allowed
        else
        {
            switch (kind)
            {
                case 0: toAll (0xB0, number, key); a->lastSent = (float) key; break;
                case 1:
                    if (key == 1) { releaseHeld (a, ctx, written); toAll (0x90, number, vel);
                                    a->heldNote = number; a->heldMask = mask; a->gateOn = true; a->lastSent = (float) vel; }
                    else          { releaseHeld (a, ctx, written); a->lastSent = 0.f; }
                    break;
                case 2:
                    releaseHeld (a, ctx, written);
                    toAll (0x90, key, vel);
                    a->heldNote = key; a->heldMask = mask; a->lastSent = (float) key;
                    break;
                case 3: toAll (0xE0, key & 0x7F, (key >> 7) & 0x7F); a->lastSent = (float) (key - 8192); break;
                case 4: toAll (0xC0, key, 0, 2); a->lastSent = (float) key; break;
                default: break;
            }
            a->lastKey   = key;
            a->haveSent  = true;
            a->sinceSend = 0.0;
            a->pending   = false;
        }
    }

    if (ctx->midiOutCount)  *ctx->midiOutCount  = written;
    if (ctx->valueOutCount) *ctx->valueOutCount = 0;
}

int PAX_getParameterCount (PAX_Instance*) { return 12; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="Message";        info->minValue=0.f;     info->maxValue=4.f;    info->defaultValue=0.f;   info->step=1.f; break;
        case 1: info->name="Number";         info->minValue=0.f;     info->maxValue=127.f;  info->defaultValue=1.f;   info->step=1.f; break;
        case 2: info->name="Channel";        info->minValue=1.f;     info->maxValue=16.f;   info->defaultValue=1.f;   info->step=1.f; break;
        case 3: info->name="Input Min";      info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=0.f;   info->step=0.f; break;
        case 4: info->name="Input Max";      info->minValue=-1000.f; info->maxValue=1000.f; info->defaultValue=1.f;   info->step=0.f; break;
        case 5: info->name="Threshold";      info->minValue=0.f;     info->maxValue=1.f;    info->defaultValue=0.5f;  info->step=0.f; break;
        case 6: info->name="Velocity";       info->minValue=1.f;     info->maxValue=127.f;  info->defaultValue=100.f; info->step=1.f; break;
        case 7: info->name="On change only"; info->minValue=0.f;     info->maxValue=1.f;    info->defaultValue=1.f;   info->step=1.f; break;
        case 8: info->name="Max rate";       info->minValue=0.f;     info->maxValue=100.f;  info->defaultValue=50.f;  info->step=1.f; break;
        case 9: info->name="Last sent";      info->minValue=-8192.f; info->maxValue=16383.f;info->defaultValue=0.f;   info->step=1.f; break;
        case 10: info->name="Invert";        info->minValue=0.f;     info->maxValue=1.f;    info->defaultValue=0.f;   info->step=1.f; break;
        case 11: info->name="Channels";      info->minValue=0.f;     info->maxValue=65535.f;info->defaultValue=0.f;   info->step=1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<ValueToMidiPax*> (i);
    switch (index)
    {
        case 0: return a->message;   case 1: return a->number;    case 2: return a->channel;
        case 3: return a->inMin;     case 4: return a->inMax;     case 5: return a->threshold;
        case 6: return a->velocity;  case 7: return a->onChange;  case 8: return a->maxRate;
        case 9: return a->lastSent;
        case 10: return a->invert;
        case 11: return a->chMask;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float v)
{
    auto* a = static_cast<ValueToMidiPax*> (i);
    switch (index)
    {
        case 0: a->message   = std::clamp (v, 0.f, 4.f);          break;
        case 1: a->number    = std::clamp (v, 0.f, 127.f);        break;
        case 2: a->channel   = std::clamp (v, 1.f, 16.f);         break;
        case 3: a->inMin     = std::clamp (v, -1000.f, 1000.f);   break;
        case 4: a->inMax     = std::clamp (v, -1000.f, 1000.f);   break;
        case 5: a->threshold = std::clamp (v, 0.f, 1.f);          break;
        case 6: a->velocity  = std::clamp (v, 1.f, 127.f);        break;
        case 7: a->onChange  = v >= 0.5f ? 1.f : 0.f;             break;
        case 8: a->maxRate   = std::clamp (v, 0.f, 100.f);        break;
        case 9: a->lastSent  = std::clamp (v, -8192.f, 16383.f);  break;   // restored on rebuild
        case 10: a->invert   = v >= 0.5f ? 1.f : 0.f;             break;
        case 11: a->chMask   = std::clamp (std::round (v), 0.f, 65535.f); break;
        default: break;
    }
}

int PAX_isParameterReadOnly (PAX_Instance*, int index) { return index == 9 ? 1 : 0; }

int PAX_getValueInputCount() { return 1; }   // generic Value (the default type): any X → Value

} // extern "C"
