/**
 * AudioToDmxPax — Audio → DMX converter (Phase 4)
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
 *     RMS × scalar drove the DMX value directly, unsmoothed, from then
 *     until Damping was added below.
 *   - Damping added 2026-08-02, per explicit request — deliberately NOT a
 *     return to Attack/Release's two-knob asymmetric design. One control,
 *     one-pole, symmetric (same time constant rising and falling), 0-500ms,
 *     applied to the final value (post-Sensitivity) rather than the raw
 *     RMS, identically in both Mode settings — same "Sensitivity already
 *     treats both modes the same way" reasoning applies here too. Default
 *     0 (none) preserves prior behaviour exactly for anyone who doesn't
 *     touch it. Coefficient is block-rate correct (scaled by
 *     ctx->numSamples, not sample rate alone) — a coefficient meant for
 *     per-sample application but only ever applied once per block, which
 *     is what the old Attack/Release did, silently makes the real smoothing
 *     time depend on host block size; this doesn't.
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
 *   - Multi-source merging fixed 2026-08-02, at the graph level not here:
 *     ProcessingGraph.cpp used to let the last edge processed each block
 *     simply overwrite a destination's whole frame — confirmed via real
 *     DAW testing that this broke exactly the intended use case (several
 *     AudioToDmxPax instances, each on a different channel, feeding one
 *     DmxOutDeviceNode: each one wiped out whatever the previous one had
 *     set). Now merges HTP-style (Highest Takes Precedence, the standard
 *     convention real DMX consoles/mergers use) — byte-wise max across
 *     every source feeding the same destination in a block, so channels
 *     each Pax addresses independently combine correctly instead of
 *     fighting. See ProcessingGraph.cpp's own comment for the full
 *     reasoning; nothing about this Pax itself changed for the fix.
 *
 * Colour: audio only on the input side, DMX only on the output side —
 * neither mirrors, so the Hybrid/Converter auto-detection rule correctly
 * colours this node Converter/fuchsia with no colourCategory override needed.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC AudioToDmxPax.cpp -o AudioToDmxPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC AudioToDmxPax.cpp -o AudioToDmxPax.so
 *   Win:    cl /std=c++20 /LD AudioToDmxPax.cpp /Fe:AudioToDmxPax.dll
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
struct AudioToDmxPax
{
    // Parameters
    float mode         =    1.f;   // 0 = RMS (whole signal), 1 = Freq Range (bandpass first)
    float sensitivityDb=   20.f;   // dB — converted to a linear multiplier before use
    float bandLow      =  200.f;   // Hz
    float bandHigh     = 2000.f;   // Hz
    float dmxChannel   =    1.f;   // 1-based, 1-512 — see file header re: the frame path
    float dampingMs    =    0.f;   // 0 = none (instant, matches pre-existing behaviour)

    // DSP state
    double         sampleRate = 44100.0;
    BandpassFilter filterL, filterR;
    float          dampedValue = 0.f;   // one-pole smoother state, persists across blocks

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
    static PAX_Descriptor d { "Audio to DMX", "Patchy Examples", "1.0.0", 2, PAX_API_VERSION, 1, 1, 0, 0 };
    return &d;
}

PAX_Instance* PAX_create() { return new AudioToDmxPax(); }

void PAX_destroy (PAX_Instance* i) { delete static_cast<AudioToDmxPax*> (i); }

void PAX_prepare (PAX_Instance* i, double sampleRate, int /*blockSize*/)
{
    auto* a = static_cast<AudioToDmxPax*> (i);
    a->sampleRate = sampleRate;
    a->filterL.reset();
    a->filterR.reset();
    a->dampedValue = 0.f;
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<AudioToDmxPax*> (i);

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

    // Damping — one pole, symmetric (same time constant rising and
    // falling), applied to the final value rather than the raw RMS. Not
    // the old Attack/Release (removed 2026-07-27, "made dialing in a
    // usable setting harder, not easier") — that was two separate knobs
    // with different rise/fall times; this is deliberately one simple
    // control from none to a modest, sensible maximum. Identical
    // treatment in both Mode settings — damping smooths whatever value
    // ends up driving DMX, same reasoning as Sensitivity itself already
    // being mode-agnostic (see file header). Coefficient is block-rate
    // correct (uses ctx->numSamples, not just sample rate) — a coefficient
    // derived from sample rate alone but applied once per block, as the
    // removed Attack/Release did, would make the real smoothing time
    // silently depend on host block size; this doesn't.
    if (a->dampingMs > 0.1f)
    {
        const float coeff = 1.f - std::exp (-(float) ctx->numSamples / (a->dampingMs * 0.001f * (float) a->sampleRate));
        a->dampedValue += coeff * (dmxVal - a->dampedValue);
    }
    else
    {
        a->dampedValue = dmxVal; // none — instant, matches pre-Damping behaviour exactly
    }

    const uint8_t dmxByte = (uint8_t) std::clamp ((int) std::lround (a->dampedValue * 255.f), 0, 255);

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
        out.value     = a->dampedValue;
        out.portIndex = 0;
        ctx->valuesOut[0] = out;
        if (ctx->valueOutCount) *ctx->valueOutCount = 1;
    }
    else if (ctx->valueOutCount)
    {
        *ctx->valueOutCount = 0;
    }
}

int PAX_getParameterCount (PAX_Instance*) { return 6; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="Mode (0=RMS,1=Freq Range)"; info->minValue=0.f;  info->maxValue=1.f;     info->defaultValue=1.f;    info->step=1.f; break;
        case 1: info->name="Sensitivity (dB)";           info->minValue=-20.f;info->maxValue=60.0f;   info->defaultValue=20.0f;  info->step=0.f; break;
        case 2: info->name="Band Low (Hz)";              info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=200.f;  info->step=1.f; break;
        case 3: info->name="Band High (Hz)";             info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=2000.f; info->step=1.f; break;
        case 4: info->name="DMX Channel";                info->minValue=1.f;  info->maxValue=512.f;   info->defaultValue=1.f;    info->step=1.f; break;
        case 5: info->name="Damping (ms)";               info->minValue=0.f;  info->maxValue=500.f;   info->defaultValue=0.f;    info->step=0.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<AudioToDmxPax*> (i);
    switch (index)
    {
        case 0: return a->mode;
        case 1: return a->sensitivityDb;
        case 2: return a->bandLow;
        case 3: return a->bandHigh;
        case 4: return a->dmxChannel;
        case 5: return a->dampingMs;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<AudioToDmxPax*> (i);
    switch (index)
    {
        case 0: a->mode        = std::clamp (value, 0.f, 1.f);      break;
        case 1: a->sensitivityDb = std::clamp (value, -20.f, 60.f);  break;
        case 2: a->bandLow     = std::clamp (value, 20.f, 20000.f); a->filterL.reset(); a->filterR.reset(); break;
        case 3: a->bandHigh    = std::clamp (value, 20.f, 20000.f); a->filterL.reset(); a->filterR.reset(); break;
        case 4: a->dmxChannel  = std::clamp (value, 1.f, 512.f);    break;
        case 5: a->dampingMs   = std::clamp (value, 0.f, 500.f);    break;
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
