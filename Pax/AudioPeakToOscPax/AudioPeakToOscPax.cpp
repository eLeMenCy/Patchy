/**
 * AudioPeakToOscPax — Audio → OSC converter (Phase 4)
 *
 * The 7th and final originally-planned Phase 4 adapter. Listens to audio,
 * extracts a level from it, scales it by a dB-based Sensitivity control,
 * and emits it as an OSC float — the OSC counterpart to AudioToDmxPax's
 * own Audio → DMX conversion.
 *
 * Design lineage: closely mirrors AudioToDmxPax throughout (see that
 * file's own header for the fuller history behind each choice) —
 * Sensitivity in dB, Damping 0-500ms, the same BandpassFilter (state-
 * variable topology), the same "audio in, zero audio out" nodeType=2
 * workaround (pass audio through unchanged, see that file's header for
 * why an explicit audioOutputs=0 doesn't work), the same final value
 * clamped 0.0-1.0 before being sent onward.
 *
 * Genuinely new here, added 2026-08-25 after discussion — this Pax's own
 * planned spec (Architecture.md) named it "Audio Peak → OSC", but
 * AudioToDmxPax's own proven Mode toggle is RMS vs Freq Range (a signal-
 * *scope* choice: whole signal or an isolated band), with no Peak option
 * at all. Rather than bolt Peak on as a third, mutually-exclusive Mode
 * value (which would only ever let Peak apply to the whole signal, never
 * an isolated band), Peak is a second, fully orthogonal toggle —
 * Measurement (RMS vs Peak) — crossed with the existing Mode (whole
 * signal vs Freq Range) axis, giving all four combinations for free:
 * whole-signal RMS, whole-signal Peak, band RMS, and band Peak. Both
 * measurements are computed in the same single pass through the block's
 * samples (whichever one Measurement doesn't select is simply the
 * unused half of already-computed work, not a second, wasted pass) —
 * see PAX_process below.
 *
 * Output mechanism: unlike DMX, OSC has no dedicated per-channel frame
 * path in PaxAPI.h — a Pax reaches OscOutDeviceNode purely through the
 * general Value mechanism (ctx->valuesOut, PAX_VALUETYPE_OSC), the same
 * way OscToValuePax/MqttToValuePax already do for their own protocols.
 * Confirmed by reading OscDeviceNodes.h directly before assuming this
 * shape: OscOutDeviceNode's own serialise() sends v.value as the OSC
 * message's float argument whenever dataType==PAX_DATA_FLOAT — v.data[]
 * is a *separate*, independent mechanism (an optional per-message OSC
 * address override, checked regardless of dataType) that this Pax
 * deliberately doesn't use, leaving the address to whatever's configured
 * on the downstream OscOutDeviceNode itself (default "/patchy") — Pax
 * parameters are numeric-only (PAX_ParameterInfo has no string fields at
 * all), so there's no way to expose a configurable address as a Pax
 * parameter in the first place, and the existing address-override
 * mechanism is there for exactly this kind of gap already.
 *
 * Send Mode (added 2026-08-25, after real testing): the mechanism above
 * always computes and holds a value every single audio block, exactly
 * like AudioToDmxPax's own DMX output does — correct for DMX, which the
 * standard itself transmits continuously at a fixed rate regardless of
 * change, but wrong for OSC, copied here without reconsidering that OSC
 * is explicitly one of this project's own discrete-event protocols (see
 * the project's established visual-language principle — flash, not held
 * intensity), not a continuous one. Confirmed directly: with a bare
 * AudioPeakToOscPax -> OscMonitorNode graph and *no audio connected at
 * all*, this Pax was still emitting an identical 0.0 message every
 * single block — around 86 messages/second at a typical 512-sample
 * buffer, a genuine flood for a monitor to display, not a hypothetical
 * edge case. UdpValueToMidiCCPax (built 2026-08-22) already got this
 * right for MIDI, deliberately only emitting on change — the same
 * reasoning wasn't applied here at first.
 *
 * Rather than pick one fix, Send Mode (0=Change, 1=Rate) makes it a
 * per-node choice — the user's own explicit request, so one node in a
 * graph can behave one way and another node the same way or differently,
 * rather than the whole Pax type being locked to a single fixed policy:
 *   - Mode 0 (default) — only emits when the value has genuinely
 *     changed since the last emission (exact float comparison — Damping
 *     already smooths away sub-perceptible jitter upstream of this
 *     check, so no separate epsilon is needed here). No rate cap at
 *     all — silence stays silent, a fast-changing signal can still emit
 *     every block if it's genuinely changing every block.
 *   - Mode 1 — continuous, throttled to Max Rate (Hz), unconditionally,
 *     whether or not the value actually changed — for a consumer that
 *     expects a steady stream to animate against (e.g. a VU-meter-style
 *     OSC widget) rather than falling silent whenever the level happens
 *     to hold steady. Samples accumulate across blocks toward the
 *     configured interval, since a single audio block (e.g. ~11.6ms at
 *     512 samples/44.1kHz) is typically much shorter than any reasonable
 *     OSC rate (Max Rate's own range is capped at 100Hz, i.e. 10ms, so
 *     even at the fastest setting more than one block's worth of samples
 *     is needed per emission).
 * In both modes the underlying level computation, Sensitivity, and
 * Damping are entirely unaffected — Send Mode only decides whether the
 * already-computed value actually goes out as a message this block, not
 * how that value itself is derived.
 *
 * Colour: audio only on the input side, OSC only on the output side —
 * neither mirrors, so the Hybrid/Converter auto-detection rule correctly
 * colours this node Converter/fuchsia with no colourCategory override
 * needed.
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC AudioPeakToOscPax.cpp -o AudioPeakToOscPax.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC AudioPeakToOscPax.cpp -o AudioPeakToOscPax.so
 *   Win:    cl /std=c++20 /LD AudioPeakToOscPax.cpp /Fe:AudioPeakToOscPax.dll
 */

#include "../PaxAPI.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static constexpr float kTwoPi = 6.283185307f;

// ── 2nd-order IIR bandpass filter (state-variable topology) ── same as AudioToDmxPax/EnvelopePax
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
struct AudioPeakToOscPax
{
    // Parameters
    float mode         =    1.f;   // 0 = whole signal, 1 = Freq Range (bandpass first) — signal SCOPE
    float measurement  =    0.f;   // 0 = RMS, 1 = Peak — measurement TYPE, orthogonal to mode
    float sensitivityDb=   20.f;   // dB — converted to a linear multiplier before use
    float bandLow      =  200.f;   // Hz
    float bandHigh     = 2000.f;   // Hz
    float dampingMs    =    0.f;   // 0 = none (instant)
    float sendMode     =    0.f;   // 0 = only on change, 1 = continuous at Max Rate — see file header
    float maxRateHz    =   30.f;   // Send Mode=1 only: steady emission rate regardless of change

    // DSP state
    double         sampleRate = 44100.0;
    BandpassFilter filterL, filterR;
    float          dampedValue = 0.f;   // one-pole smoother state, persists across blocks

    // Send-mode state (added 2026-08-25, see file header)
    double sampleAccum   = 0.0;    // samples accumulated since the last actual emission (Send Mode=1)
    float  lastSentValue = 0.f;    // the value last actually sent, for change comparison (Send Mode=0)
    bool   hasSentOnce   = false;  // ensures the very first genuine value always sends

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
    // workaround — see file header). No MIDI. OSC output declared
    // separately below via PAX_getValueOutputCount/Type.
    static PAX_Descriptor d { "Audio Peak to OSC", "Patchy Examples", "1.0.0", 2, PAX_API_VERSION, 1, 1, 0, 0 };
    return &d;
}

PAX_Instance* PAX_create() { return new AudioPeakToOscPax(); }

void PAX_destroy (PAX_Instance* i) { delete static_cast<AudioPeakToOscPax*> (i); }

void PAX_prepare (PAX_Instance* i, double sampleRate, int /*blockSize*/)
{
    auto* a = static_cast<AudioPeakToOscPax*> (i);
    a->sampleRate = sampleRate;
    a->filterL.reset();
    a->filterR.reset();
    a->dampedValue   = 0.f;
    a->sampleAccum   = 0.0;
    a->lastSentValue = 0.f;
    a->hasSentOnce   = false;
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<AudioPeakToOscPax*> (i);

    // Pass audio through unchanged — see file header re: the "audio in,
    // zero audio out" limitation workaround.
    if (ctx->audioIn && ctx->audioOut)
        for (int ch = 0; ch < ctx->numChannels; ++ch)
            if (ctx->audioIn[ch] && ctx->audioOut[ch])
                std::memcpy (ctx->audioOut[ch], ctx->audioIn[ch], (size_t) ctx->numSamples * sizeof (float));

    if (ctx->midiOutCount) *ctx->midiOutCount = 0;

    const bool  spectral = a->mode        >= 0.5f;
    const bool  usePeak  = a->measurement >= 0.5f;
    const float f0       = a->bandCentre();
    const float q        = a->bandQ();
    const float sr       = (float) a->sampleRate;

    // Both RMS and Peak computed in the same single pass — whichever
    // Measurement doesn't select below is simply the unused half of
    // already-completed work, not a second, wasted pass over the block.
    float sumSq   = 0.f;
    float peakAbs = 0.f;
    int   count   = 0;

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
                sumSq   += v * v;
                peakAbs  = std::max (peakAbs, std::fabs (v));
                ++count;
            }
        }
    }

    const float rms   = count > 0 ? std::sqrt (sumSq / count) : 0.f;
    const float level = usePeak ? peakAbs : rms;
    const float oscVal = std::clamp (level * a->sensitivityGain(), 0.f, 1.f);

    // Damping — one pole, symmetric, applied to the final value. Same
    // approach and same reasoning as AudioToDmxPax's own (see that file's
    // header) — identical treatment regardless of Mode or Measurement,
    // since Sensitivity already treats every combination the same way.
    // Coefficient is block-rate correct (uses ctx->numSamples, not just
    // sample rate alone).
    if (a->dampingMs > 0.1f)
    {
        const float coeff = 1.f - std::exp (-(float) ctx->numSamples / (a->dampingMs * 0.001f * (float) a->sampleRate));
        a->dampedValue += coeff * (oscVal - a->dampedValue);
    }
    else
    {
        a->dampedValue = oscVal; // none — instant
    }

    // Send Mode decides WHETHER to actually emit this block, on top of
    // (not instead of) everything above — the level itself, and damping,
    // are always computed and held in a->dampedValue regardless of Send
    // Mode; this just decides whether that value gets sent out as an OSC
    // message this particular block. See file header for the full
    // reasoning and the user's own framing that led to this being a
    // per-node choice rather than one fixed behaviour for every instance.
    bool shouldSend;

    if (a->sendMode < 0.5f)
    {
        // Mode 0: only on change. No rate cap at all — could in principle
        // send every block if the signal is genuinely changing every
        // block, but never sends anything while silent or steady, which
        // is exactly what fixes the original "huge amount of messages
        // with nothing connected" report this feature was built for.
        shouldSend = ! a->hasSentOnce || a->dampedValue != a->lastSentValue;
    }
    else
    {
        // Mode 1: continuous, throttled to Max Rate — sends on a steady
        // cadence regardless of whether the value actually changed,
        // useful for a consumer that expects a continuous stream to
        // animate against (e.g. a VU-meter-style OSC widget) rather than
        // silence whenever the level happens to hold steady. Accumulates
        // samples across blocks since a single audio block is typically
        // much shorter than any reasonable OSC rate (e.g. a 512-sample
        // block at 44.1kHz is ~11.6ms, well under a 30Hz/~33ms interval).
        a->sampleAccum += ctx->numSamples;
        const double samplesPerEmission = a->sampleRate / (double) std::max (1.f, a->maxRateHz);
        shouldSend = ! a->hasSentOnce || a->sampleAccum >= samplesPerEmission;
        if (shouldSend) a->sampleAccum = 0.0;
    }

    // Real payload: the general Value mechanism (see file header re: why
    // OSC has no dedicated frame path the way DMX does). No blob; just a
    // plain float, address left to whatever's configured on the
    // downstream OscOutDeviceNode.
    if (shouldSend && ctx->valuesOut && ctx->valueMaxCount > 0)
    {
        PAX_Value out {};
        out.type      = PAX_TYPE_OSC;
        out.dataType  = PAX_DATA_FLOAT;
        out.key       = 0;
        out.value     = a->dampedValue;
        out.portIndex = 0;
        ctx->valuesOut[0] = out;
        if (ctx->valueOutCount) *ctx->valueOutCount = 1;
        a->lastSentValue = a->dampedValue;
        a->hasSentOnce   = true;
    }
    else if (ctx->valueOutCount)
    {
        *ctx->valueOutCount = 0;
    }
}

int PAX_getParameterCount (PAX_Instance*) { return 8; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="Mode (0=Whole,1=Freq Range)";  info->minValue=0.f;  info->maxValue=1.f;     info->defaultValue=1.f;    info->step=1.f; break;
        case 1: info->name="Measurement (0=RMS,1=Peak)";    info->minValue=0.f;  info->maxValue=1.f;     info->defaultValue=0.f;    info->step=1.f; break;
        case 2: info->name="Sensitivity (dB)";              info->minValue=-20.f;info->maxValue=60.0f;   info->defaultValue=20.0f;  info->step=0.f; break;
        case 3: info->name="Band Low (Hz)";                 info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=200.f;  info->step=1.f; break;
        case 4: info->name="Band High (Hz)";                info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=2000.f; info->step=1.f; break;
        case 5: info->name="Damping (ms)";                  info->minValue=0.f;  info->maxValue=500.f;   info->defaultValue=0.f;    info->step=0.f; break;
        case 6: info->name="Send Mode (0=Change,1=Rate)";   info->minValue=0.f;  info->maxValue=1.f;     info->defaultValue=0.f;    info->step=1.f; break;
        case 7: info->name="Max Rate (Hz)";                 info->minValue=1.f;  info->maxValue=100.f;   info->defaultValue=30.f;   info->step=1.f; break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<AudioPeakToOscPax*> (i);
    switch (index)
    {
        case 0: return a->mode;
        case 1: return a->measurement;
        case 2: return a->sensitivityDb;
        case 3: return a->bandLow;
        case 4: return a->bandHigh;
        case 5: return a->dampingMs;
        case 6: return a->sendMode;
        case 7: return a->maxRateHz;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<AudioPeakToOscPax*> (i);
    switch (index)
    {
        case 0: a->mode          = std::clamp (value, 0.f, 1.f);      break;
        case 1: a->measurement   = std::clamp (value, 0.f, 1.f);      break;
        case 2: a->sensitivityDb = std::clamp (value, -20.f, 60.f);   break;
        case 3: a->bandLow       = std::clamp (value, 20.f, 20000.f); a->filterL.reset(); a->filterR.reset(); break;
        case 4: a->bandHigh      = std::clamp (value, 20.f, 20000.f); a->filterL.reset(); a->filterR.reset(); break;
        case 5: a->dampingMs     = std::clamp (value, 0.f, 500.f);    break;
        case 6: a->sendMode      = std::clamp (value, 0.f, 1.f);      break;
        case 7: a->maxRateHz     = std::clamp (value, 1.f, 100.f);    break;
        default: break;
    }
}

// Value port — single OSC-typed output, no Value input (audio only).
int PAX_getValueInputCount()  { return 0; }
int PAX_getValueOutputCount() { return 1; }

int PAX_getValueOutputType (int portIndex)
{
    return portIndex == 0 ? PAX_VALUETYPE_OSC : PAX_VALUETYPE_GENERIC;
}

} // extern "C"
