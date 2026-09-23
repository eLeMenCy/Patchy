/**
 * ChannelFilterPax — MIDI Node Pax example
 *
 * Keeps only the messages on one chosen MIDI channel (1-16), in one of two
 * modes:
 *   - Filter (mode 0): pass a message through unchanged only if it's
 *     already on the chosen channel; drop everything else.
 *   - Force  (mode 1): rewrite EVERY message's own channel to the chosen
 *     one, regardless of what channel it originally arrived on.
 *
 * Deliberately built as a close sibling of TransposePax.cpp — same
 * minimal structure, same MIDI-type Pax shape — specifically so the two
 * can be read side by side. Differences worth noticing while comparing:
 *   - TransposePax has 1 parameter (a continuous-feeling range, -24..24);
 *     this Pax has 2 (a stepped 1..16 range, plus a 2-value mode toggle)
 *     — see PAX_getParameterInfo() below for how a second parameter is
 *     declared.
 *   - TransposePax always passes every message through, just modifying
 *     note numbers in place; this Pax's Filter mode is the first time in
 *     this file a message can be genuinely DROPPED (not written to
 *     midiOut at all) — see the `written` counter logic below.
 *   - Both leave non-note/channel-less messages (sysex etc) untouched by
 *     their own specific logic; this Pax goes one step further and lets
 *     them always pass through regardless of mode, since they were never
 *     addressable by a per-channel filter in the first place (the same
 *     convention the built-in MIDI CH. MATRIX node also follows).
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC ChannelFilterPax.cpp -o ChannelFilterPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC ChannelFilterPax.cpp -o ChannelFilterPax.so
 *   Win:    cl /std=c++20 /LD ChannelFilterPax.cpp /Fe:ChannelFilterPax.dll
 */

#include "../PaxAPI.h"
#include <algorithm>
#include <cmath>

struct ChannelFilterPax
{
    float channel = 1.0f;   // 1 to 16, default 1
    float mode    = 0.0f;   // 0 = Filter (drop), 1 = Force (rewrite)
};

extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d {
        "Channel Filter",
        "Patchy Examples",
        "1.0.0",
        1,               // nodeType: MIDI
        PAX_API_VERSION
    };
    return &d;
}

PAX_Instance* PAX_create()           { return new ChannelFilterPax(); }
void PAX_destroy (PAX_Instance* i)   { delete static_cast<ChannelFilterPax*> (i); }
void PAX_prepare (PAX_Instance*, double, int) {}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* node        = static_cast<ChannelFilterPax*> (i);
    const int wanted   = std::clamp ((int) std::round (node->channel), 1, 16);
    const bool force   = node->mode >= 0.5f;
    int  written        = 0;

    for (int e = 0; e < ctx->midiInCount && written < ctx->midiMaxCount; ++e)
    {
        const auto& msg = ctx->midiIn[e];

        if (msg.byteCount < 1)
        {
            ctx->midiOut[written++] = msg;
            continue;
        }

        const uint8_t status = msg.bytes[0];

        // A status byte of 0xF0 or above (sysex, clock, etc) has no
        // channel nibble at all — never addressable by this filter,
        // so it always passes through unchanged, same convention as
        // the built-in MIDI CH. MATRIX node.
        if (status >= 0xF0)
        {
            ctx->midiOut[written++] = msg;
            continue;
        }

        const int msgChannel = (status & 0x0F) + 1;   // 1-16

        if (force)
        {
            // Rewrite every message's own channel nibble to the chosen
            // one, leaving the high nibble (the message type) untouched.
            ctx->midiOut[written]            = msg;
            ctx->midiOut[written].bytes[0]   = (uint8_t) ((status & 0xF0) | (wanted - 1));
            ++written;
        }
        else if (msgChannel == wanted)
        {
            // Filter mode: only a message already on the wanted channel
            // survives, copied through byte-for-byte, unchanged.
            ctx->midiOut[written++] = msg;
        }
        // else: Filter mode, wrong channel — genuinely dropped, nothing
        // written for this message at all.
    }

    *ctx->midiOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 2; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    if (index == 0)
    {
        info->name         = "Channel";
        info->minValue     =  1.0f;
        info->maxValue     = 16.0f;
        info->defaultValue =  1.0f;
        info->step         =  1.0f;   // integer channel number
    }
    else if (index == 1)
    {
        info->name         = "Mode";
        info->minValue     = 0.0f;
        info->maxValue     = 1.0f;
        info->defaultValue = 0.0f;    // 0 = Filter
        info->step         = 1.0f;    // 2 discrete values only
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* node = static_cast<ChannelFilterPax*> (i);
    if (index == 0) return node->channel;
    if (index == 1) return node->mode;
    return 0.f;
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* node = static_cast<ChannelFilterPax*> (i);
    if (index == 0) node->channel = std::clamp (value, 1.0f, 16.0f);
    if (index == 1) node->mode    = std::clamp (value, 0.0f, 1.0f);
}

// Disable/Enable feature — same-type, in-place MIDI effect (like
// TransposePax), so disabling it should behave like a normal plugin
// bypass: every message keeps flowing through unchanged, on whatever
// channel it originally arrived on, regardless of Filter/Force mode.
int PAX_getMidiPassthrough (PAX_Instance*)
{
    return 1;
}

} // extern "C"
