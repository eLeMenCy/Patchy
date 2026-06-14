/**
 * EnvelopePax — MIDI+Audio Node Pax
 *
 * Converts audio amplitude or a frequency band level into a MIDI CC stream.
 * Use to drive external LED controllers, DAW automation, or any MIDI-capable device.
 *
 * Amplitude mode: tracks overall RMS level → CC
 * Spectral mode:  bandpass-filters the audio first, then tracks RMS → CC
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC EnvelopePax.cpp -o EnvelopeAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC EnvelopePax.cpp -o EnvelopePax.so
 *   Win:    cl /std=c++20 /LD EnvelopePax.cpp /Fe:EnvelopePax.dll
 */

#include "../PaxAPI.h"
#include <cmath>
#include <algorithm>
#include <cstring>

static constexpr float kTwoPi = 6.283185307f;

// ── 2nd-order IIR bandpass filter (state-variable topology) ──────────────────
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

// ── Envelope follower ─────────────────────────────────────────────────────────
struct EnvelopeFollower
{
    float env = 0.f;

    float process (float rectified, float attackCoeff, float releaseCoeff)
    {
        if (rectified > env)
            env += attackCoeff  * (rectified - env);
        else
            env += releaseCoeff * (rectified - env);
        return env;
    }
};

// ── Pax state ───────────────────────────────────────────────────────────────
struct EnvelopePax
{
    // Parameters
    float mode        =    0.f;   // 0 = Amplitude, 1 = Spectral
    float ccNumber    =   11.f;   // CC11 = Expression
    float midiChannel =    1.f;   // 1-16
    float attackMs    =   10.f;   // ms
    float releaseMs   =  200.f;   // ms
    float sensitivity =    1.f;   // linear scalar
    float bandLow     =  200.f;   // Hz
    float bandHigh    = 2000.f;   // Hz

    // DSP state
    double       sampleRate   = 44100.0;
    BandpassFilter filterL, filterR;
    EnvelopeFollower envFollower;
    int          lastCc       = -1;   // detect changes

    // Coefficients (recalculated when params change)
    float attackCoeff  = 0.f;
    float releaseCoeff = 0.f;

    void updateCoeffs()
    {
        // Simple 1-pole IIR attack/release
        attackCoeff  = 1.f - std::exp (-1.f / (attackMs  * 0.001f * (float) sampleRate));
        releaseCoeff = 1.f - std::exp (-1.f / (releaseMs * 0.001f * (float) sampleRate));
    }

    // Bandpass centre + Q from low/high edges
    float bandCentre() const { return std::sqrt (bandLow * bandHigh); }
    float bandQ()      const
    {
        const float centre = bandCentre();
        return centre / std::max (1.f, bandHigh - bandLow);
    }
};

// ── NGA exports ───────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d { "Envelope", "Patchy Examples", "1.0.0", 3, PAX_API_VERSION };
    return &d;
}

PAX_Instance* PAX_create()
{
    auto* a = new EnvelopePax();
    a->updateCoeffs();
    return a;
}

void PAX_destroy (PAX_Instance* i) { delete static_cast<EnvelopePax*> (i); }

void PAX_prepare (PAX_Instance* i, double sampleRate, int /*blockSize*/)
{
    auto* a = static_cast<EnvelopePax*> (i);
    a->sampleRate = sampleRate;
    a->filterL.reset();
    a->filterR.reset();
    a->envFollower.env = 0.f;
    a->lastCc = -1;
    a->updateCoeffs();
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    auto* a = static_cast<EnvelopePax*> (i);

    // Pass audio through unchanged
    if (ctx->audioIn && ctx->audioOut)
        for (int ch = 0; ch < ctx->numChannels; ++ch)
            if (ctx->audioIn[ch] && ctx->audioOut[ch])
                std::memcpy (ctx->audioOut[ch], ctx->audioIn[ch], (size_t) ctx->numSamples * sizeof (float));

    // Pass MIDI through unchanged
    int written = 0;
    for (int e = 0; e < ctx->midiInCount && written < ctx->midiMaxCount; ++e)
        ctx->midiOut[written++] = ctx->midiIn[e];

    // ── Envelope detection ────────────────────────────────────────────────────
    const bool spectral = a->mode >= 0.5f;
    const float f0      = a->bandCentre();
    const float q       = a->bandQ();
    const float sr      = (float) a->sampleRate;

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

    // RMS → envelope → CC
    const float rms      = count > 0 ? std::sqrt (sumSq / count) : 0.f;
    const float scaled   = std::min (1.f, rms * a->sensitivity);
    const float env      = a->envFollower.process (scaled, a->attackCoeff, a->releaseCoeff);
    const int   cc       = (int) std::clamp (env * 127.f, 0.f, 127.f);

    if (cc != a->lastCc && written < ctx->midiMaxCount)
    {
        const uint8_t ch = (uint8_t) std::clamp ((int) a->midiChannel - 1, 0, 15);
        ctx->midiOut[written].bytes[0]  = 0xB0 | ch;
        ctx->midiOut[written].bytes[1]  = (uint8_t) std::clamp ((int) a->ccNumber, 0, 127);
        ctx->midiOut[written].bytes[2]  = (uint8_t) cc;
        ctx->midiOut[written].byteCount = 3;
        ctx->midiOut[written].sampleOffset = ctx->numSamples - 1;
        ++written;
        a->lastCc = cc;
    }

    *ctx->midiOutCount = written;
}

int PAX_getParameterCount (PAX_Instance*) { return 8; }

void PAX_getParameterInfo (PAX_Instance*, int index, PAX_ParameterInfo* info)
{
    switch (index)
    {
        case 0: info->name="Mode";         info->minValue=0.f;   info->maxValue=1.f;     info->defaultValue=0.f;   info->step=1.f;   break;
        case 1: info->name="CC Number";    info->minValue=0.f;   info->maxValue=127.f;   info->defaultValue=11.f;  info->step=1.f;   break;
        case 2: info->name="MIDI Channel"; info->minValue=1.f;   info->maxValue=16.f;    info->defaultValue=1.f;   info->step=1.f;   break;
        case 3: info->name="Attack (ms)";  info->minValue=1.f;   info->maxValue=500.f;   info->defaultValue=10.f;  info->step=1.f;   break;
        case 4: info->name="Release (ms)"; info->minValue=1.f;   info->maxValue=2000.f;  info->defaultValue=200.f; info->step=1.f;   break;
        case 5: info->name="Sensitivity";  info->minValue=0.1f;  info->maxValue=4.0f;    info->defaultValue=1.0f;  info->step=0.f;   break;
        case 6: info->name="Band Low (Hz)";info->minValue=20.f;  info->maxValue=20000.f; info->defaultValue=200.f; info->step=1.f;   break;
        case 7: info->name="Band High (Hz)";info->minValue=20.f; info->maxValue=20000.f; info->defaultValue=2000.f;info->step=1.f;   break;
        default: break;
    }
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* a = static_cast<EnvelopePax*> (i);
    switch (index)
    {
        case 0: return a->mode;
        case 1: return a->ccNumber;
        case 2: return a->midiChannel;
        case 3: return a->attackMs;
        case 4: return a->releaseMs;
        case 5: return a->sensitivity;
        case 6: return a->bandLow;
        case 7: return a->bandHigh;
        default: return 0.f;
    }
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* a = static_cast<EnvelopePax*> (i);
    switch (index)
    {
        case 0: a->mode        = std::clamp (value, 0.f, 1.f);     break;
        case 1: a->ccNumber    = std::clamp (value, 0.f, 127.f);   break;
        case 2: a->midiChannel = std::clamp (value, 1.f, 16.f);    break;
        case 3: a->attackMs    = std::clamp (value, 1.f, 500.f);   a->updateCoeffs(); break;
        case 4: a->releaseMs   = std::clamp (value, 1.f, 2000.f);  a->updateCoeffs(); break;
        case 5: a->sensitivity = std::clamp (value, 0.1f, 4.0f);   break;
        case 6: a->bandLow     = std::clamp (value, 20.f, 20000.f);a->filterL.reset(); a->filterR.reset(); break;
        case 7: a->bandHigh    = std::clamp (value, 20.f, 20000.f);a->filterL.reset(); a->filterR.reset(); break;
        default: break;
    }
}

} // extern "C"
