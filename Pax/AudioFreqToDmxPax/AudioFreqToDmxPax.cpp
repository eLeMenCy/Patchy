/**
 * AudioFreqToDmxPax — Audio → DMX converter (Phase 4)
 *
 * Listens to audio, extracts a level from it (either the whole signal's
 * RMS, or a specific isolated frequency band's RMS), scales it by a
 * dB-based Sensitivity control, and drives a single DMX channel value
 * from it directly — a genuine cross-domain conversion, not a bridge.
 *
 * Deliberately the first Pax exercised hosted inside a DAW (as a plugin),
 * not just standalone — everything Pax-related before this was only ever
 * tested standalone.
 *
 * Design lineage (see Architecture.md, 2026-07-26/27 discussion + follow-up):
 *   - Bandpass/RMS shape is EnvelopePax's, almost verbatim — same
 *     BandpassFilter (state-variable topology). Only the output differs:
 *     a DMX-typed PAX_Value instead of a MIDI CC.
 *   - Mode switch (RMS vs Freq Range) also mirrors Envelope's own
 *     Amplitude/Spectral mode toggle, added per explicit request so the
 *     person placing this node can choose whole-signal RMS or an isolated
 *     band, rather than baking in "always spectral" as the original
 *     design discussion assumed.
 *   - Attack/Release (Envelope's smoothing stage) deliberately NOT carried
 *     over — removed 2026-07-27 after physical testing showed it made
 *     dialing in a usable setting harder, not easier. Sensitivity's raw
 *     RMS × scalar now drives the DMX value directly, unsmoothed.
 *   - Sensitivity switched to dB 2026-07-27, per explicit request — the
 *     linear-multiplier version made it hard to land on a usable setting
 *     without pushing the raw scalar very high, which read as "needing
 *     very high dB" even though the control wasn't dB-based at all.
 *     `sensitivityGain()` converts the stored dB value to a linear
 *     multiplier (`10^(dB/20)`, the standard amplitude convention) at the
 *     point of use — the parameter itself, and everything it's clamped
 *     to (-20 to +60 dB), is dB throughout.
  *   - DMX output uses the dedicated DMX frame path (PaxAPI.h v4 —
 *     ctx->dmxFrameOut/dmxFrameOutValid), not the general Value mechanism —
 *     see the "DMX Channel switched to the frame path" note below for why.
 *   - Value normalized 0.0-1.0 internally before being written to the frame
 *     as a byte, matching the existing convention (DmxDeviceNodes.h divides
 *     raw 0-255 by 255 the same way).
  *   - Known pre-existing limitation (not introduced or fixed here): there's
 *     no clean way for nodeType=2 to declare "audio in, zero audio out" —
 *     an explicit audioOutputs=0 doesn't override the nodeType default.
 *     Workaround, same as Envelope's own precedent: declare 1 audio output
 *     and pass audio through unchanged rather than leaving it silent, so
 *     the unused port is at least useful if ever connected downstream.
 *   - DMX Channel switched to the frame path 2026-07-28 — originally
 *     (2026-07-27) this Pax addressed a channel by writing one byte at a
 *     chosen offset inside PAX_Value.data[], the same trick DmxConsoleNode
 *     used. That investigation surfaced that data[] is a fixed 56-byte
 *     inline buffer (PaxAPI.h) — DmxConsoleNode's own real 512-channel
 *     board was *also* silently truncated to its first 56 channels through
 *     that mechanism, a project-wide gap, not something specific to this
 *     Pax. Fixed properly with a new API v4 path — ctx->dmxFrameOut is a
 *     real 512-byte buffer, zeroed by the host before each call, so this
 *     Pax now just writes its one channel's byte at dmxChannel-1 and sets
 *     *ctx->dmxFrameOutValid = true. DMX Channel's range is genuinely 1-512
 *     now, no longer capped at 56. A minimal Value (type=DMX, dataType=
 *     FLOAT, value=this node's level, no blob) is still emitted alongside
 *     purely so the existing port/edge-matching machinery keeps working —
 *     DmxOutDeviceNode/DmxMonitorNode read the frame, not this.
 *   - Real caveat still worth knowing before wiring this into a real rig:
 *     DmxOutDeviceNode rebuilds its entire 512-byte frame from whichever
 *     DMX frame it last received (see DmxDeviceNodes.h) — there's no
 *     merging. If more than one source feeds the same DMX Out — this Pax
 *     plus, say, a DMX Console — each new frame resets every channel the
 *     other one was driving. Fine for a single-source rig; not yet a real
 *     multi-source mixer. A property of the DMX frame path generally, not
 *     something specific to this Pax or fixed here.
 *
 * Colour: audio only on the input side, DMX only on the output side —
 * neither mirrors, so the Hybrid/Converter auto-detection rule correctly
 * colours this node Converter/fuchsia with no colourCategory override needed.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC AudioFreqToDmxPax.cpp -o AudioFreqToDmxPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC AudioFreqToDmxPax.cpp -o AudioFreqToDmxPax.so
 *   Win:    cl /std=c++20 /LD AudioFreqToDmxPax.cpp /Fe:AudioFreqToDmxPax.dll
 */

#include "../PaxAPI.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static constexpr float kTwoPi = 6.283185307f;

// ── 2nd-order IIR bandpass filter (state-variable topology) ── same as EnvelopePax
struct BandpassFilter
{
    float low = 0.f, band = 0.f;

    void reset() { low = band = 0.f; }

    float process (float x, float f0, float q, float sampleRate)
    {
        const float w  = 2.f * std::sin (kTwoPi * f0 / (2.f * sampleRate));
        const float r  = 1.f / q;
        const float hp = x - r * band - low;
        band          += w * hp;
        low           += w * band;
        return band;
    }
};

// ── Pax state ─────────────────────────────────────────────────────────────────
struct AudioFreqToDmxPax
{
    // Parameters
    float mode         =    1.f;   // 0 = RMS (whole signal), 1 = Freq Range (bandpass first)
    float sensitivityDb=   20.f;   // dB — converted to a linear multiplier before use
    float bandLow      =  200.f;   // Hz
    float bandHigh     = 2000.f;   // Hz
    float dmxChannel   =    1.f;   // 1-based, 1-512 — see file header re: the frame path

    // DSP state
    double         sampleRate = 44100.0;
    BandpassFilter filterL, filterR;

    float bandCentre() const { return std::sqrt (bandLow * bandHigh); }
    float bandQ()      const
    {
        const float centre = bandCentre();
        return centre / std::max (1.f, bandHigh - bandLow);
    }

    // dB → linear amplitude multiplier (standard 20*log10 convention)
    float sensitivityGain() const { return std::pow (10.f, sensitivityDb / 20.f); }
};

// ── Pax exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    // nodeType 2 (Audio): 1 audio in, 1 audio out (unused-but-declared
    // workaround — see file header). No MIDI. DMX output declared
    // separately below via PAX_getValueOutputCount/Type.
    static PAX_Descriptor d { "Audio Freq to DMX", "Patchy Examples", "1.0.0", 2, PAX_API_VERSION, 1, 1, 0, 0 };
    return &d;
}

PAX_Instance* PAX_create() { return new AudioFreqToDmxPax(); }

void PAX_destroy (PAX_Instance* i) { delete static_cast<AudioFreqToDmxPax*> (i); }

void PAX_prepare (PAX_Instance* i, double sampleRate, int /*blockSize*/)
{
    auto* a = static_cast<AudioFreqToDmxPax*> (i);
    a->sampleRate = sampleRate;
    a->filterL.reset();
    a->filterR.reset();
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<AudioFreqToDmxPax*> (i);

    // Pass audio through unchanged — see file header re: the "audio in,
    // zero audio out" limitation workaround.
    if (ctx->audioIn && ctx->audioOut)
        for (int ch = 0; ch < ctx->numChannels; ++ch)
            if (ctx->audioIn[ch] && ctx->audioOut[ch])
                std::memcpy (ctx->audioOut[ch], ctx->audioIn[ch], (size_t) ctx->numSamples * sizeof (float));

    if (ctx->midiOutCount) *ctx->midiOutCount = 0;

    const bool  spectral = a->mode >= 0.5f;
    const float f0       = a->bandCentre();
    const float q        = a->bandQ();
    const float sr       = (float) a->sampleRate;

    float sumSq = 0.f;
    int   count = 0;

    if (ctx->audioIn)
    {
        for (int ch = 0; ch < std::min (ctx->numChannels, 2); ++ch)
        {
            if (! ctx->audioIn[ch]) continue;
            auto& filt = (ch == 0) ? a->filterL : a->filterR;

            for (int s = 0; s < ctx->numSamples; ++s)
            {
                float v = ctx->audioIn[ch][s];
                if (spectral)
                    v = filt.process (v, f0, q, sr);
                sumSq += v * v;
                ++count;
            }
        }
    }

    const float rms    = count > 0 ? std::sqrt (sumSq / count) : 0.f;
    const float dmxVal = std::clamp (rms * a->sensitivityGain(), 0.f, 1.f);
    const uint8_t dmxByte = (uint8_t) std::clamp ((int) std::lround (dmxVal * 255.f), 0, 255);

    // Real payload: the dedicated DMX frame path (API v4). Host already
    // zeroed ctx->dmxFrameOut and cleared *ctx->dmxFrameOutValid before this
    // call, so only this one channel needs touching, not all 512.
    if (ctx->dmxFrameOut && ctx->dmxFrameOutValid)
    {
        const int chIndex = std::clamp ((int) a->dmxChannel - 1, 0, 511);
        ctx->dmxFrameOut[chIndex] = dmxByte;
        *ctx->dmxFrameOutValid = true;
    }

    // Minimal Value mirror alongside it, purely so the existing port/edge
    // machinery keeps working — see file header. No blob; the frame above
    // is what DmxOutDeviceNode/DmxMonitorNode actually read now.
    if (ctx->valuesOut && ctx->valueMaxCount > 0)
    {
        PAX_Value out {};
        out.type      = PAX_TYPE_DMX;
        out.dataType  = PAX_DATA_FLOAT;
        out.key       = 0;
        out.value     = dmxVal;
        out.portIndex = 0;
        ctx->valuesOut[0] = out;
        if (ctx->valueOutCount) *ctx->valueOutCount = 1;
    }
    else if (ctx->valueOutCount)
    {
        *ctx->valueOutCount = 0;
    }
}

int PAX_getParameterCount (PAX_Instance*) { return 5; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="Mode (0=RMS,1=Freq Range)"; info->minValue=0.f;  info->maxValue=1.f;     info->defaultValue=1.f;    info->step=1.f; break;
        case 1: info->name="Sensitivity (dB)";           info->minValue=-20.f;info->maxValue=60.0f;   info->defaultValue=20.0f;  info->step=0.f; break;
        case 2: info->name="Band Low (Hz)";              info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=200.f;  info->step=1.f; break;
        case 3: info->name="Band High (Hz)";             info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=2000.f; info->step=1.f; break;
        case 4: info->name="DMX Channel";                info->minValue=1.f;  info->maxValue=512.f;   info->defaultValue=1.f;    info->step=1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<AudioFreqToDmxPax*> (i);
    switch (index)
    {
        case 0: return a->mode;
        case 1: return a->sensitivityDb;
        case 2: return a->bandLow;
        case 3: return a->bandHigh;
        case 4: return a->dmxChannel;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<AudioFreqToDmxPax*> (i);
    switch (index)
    {
        case 0: a->mode        = std::clamp (value, 0.f, 1.f);      break;
        case 1: a->sensitivityDb = std::clamp (value, -20.f, 60.f);  break;
        case 2: a->bandLow     = std::clamp (value, 20.f, 20000.f); a->filterL.reset(); a->filterR.reset(); break;
        case 3: a->bandHigh    = std::clamp (value, 20.f, 20000.f); a->filterL.reset(); a->filterR.reset(); break;
        case 4: a->dmxChannel  = std::clamp (value, 1.f, 512.f);    break;
        default: break;
    }
}

// Value port — single DMX-typed output.
int PAX_getValueInputCount()  { return 0; }
int PAX_getValueOutputCount() { return 1; }

int PAX_getValueOutputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_DMX : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
