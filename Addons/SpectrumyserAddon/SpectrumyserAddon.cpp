// SPDX-License-Identifier: MIT
/**
 * SpectrumyserAddon — Audio spectrum analyser with frequency band outputs
 *
 * Takes 1 stereo audio input and splits it into 1-5 configurable frequency
 * band outputs. Each output port carries the audio filtered to that band.
 * A mini FFT spectrum is available for UI display via AudioSnapshot.
 *
 * Parameters:
 *   0  — Band Count      (1-5, default 3)
 *   1  — Band 1 Low Hz   (default 20)
 *   2  — Band 1 High Hz  (default 200)
 *   3  — Band 2 Low Hz   (default 200)
 *   4  — Band 2 High Hz  (default 2000)
 *   5  — Band 3 Low Hz   (default 2000)
 *   6  — Band 3 High Hz  (default 8000)
 *   7  — Band 4 Low Hz   (default 8000)
 *   8  — Band 4 High Hz  (default 16000)
 *   9  — Band 5 Low Hz   (default 16000)
 *   10 — Band 5 High Hz  (default 20000)
 *
 * Build:
 *   macOS:  clang++ -std=c++20 -shared -fPIC SpectrumyserAddon.cpp -o SpectrumyserAddon.dylib
 *   Linux:  g++     -std=c++20 -shared -fPIC SpectrumyserAddon.cpp -o SpectrumyserAddon.so
 *   Win:    cl /std:c++20 /LD SpectrumyserAddon.cpp /Fe:SpectrumyserAddon.dll
 */

#include "../AddonAPI.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <array>
#include <vector>

// ── Simple 2nd-order Butterworth bandpass filter ──────────────────────────────
struct BiquadFilter
{
    double b0=1, b1=0, b2=0, a1=0, a2=0;
    double z1=0, z2=0;

    void setLowpass (double freq, double sampleRate)
    {
        double w0  = 2.0 * M_PI * freq / sampleRate;
        double cos0 = std::cos (w0);
        double sin0 = std::sin (w0);
        double Q   = 0.7071;
        double alpha = sin0 / (2.0 * Q);
        double a0r = 1.0 / (1.0 + alpha);
        b0 = ((1.0 - cos0) / 2.0) * a0r;
        b1 =  (1.0 - cos0) * a0r;
        b2 = b0;
        a1 = (-2.0 * cos0) * a0r;
        a2 = (1.0 - alpha) * a0r;
    }

    void setHighpass (double freq, double sampleRate)
    {
        double w0   = 2.0 * M_PI * freq / sampleRate;
        double cos0 = std::cos (w0);
        double sin0 = std::sin (w0);
        double Q    = 0.7071;
        double alpha = sin0 / (2.0 * Q);
        double a0r  = 1.0 / (1.0 + alpha);
        b0 =  ((1.0 + cos0) / 2.0) * a0r;
        b1 = -(1.0 + cos0) * a0r;
        b2 = b0;
        a1 = (-2.0 * cos0) * a0r;
        a2 = (1.0 - alpha) * a0r;
    }

    void reset() { z1 = z2 = 0.0; }

    float process (float x)
    {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return static_cast<float> (y);
    }
};

// ── Bandpass = highpass followed by lowpass ───────────────────────────────────
struct BandpassFilter
{
    BiquadFilter hp, lp;
    float lowHz = 20.f, highHz = 20000.f;
    double currentSampleRate = 44100.0;

    void setup (float lo, float hi, double sr)
    {
        lowHz  = lo;
        highHz = hi;
        currentSampleRate = sr;
        hp.setHighpass (std::max (1.0, (double) lo),  sr);
        lp.setLowpass  (std::min ((double)(sr * 0.49), (double) hi), sr);
        // Don't reset filter state — resetting causes clicks when moving sliders
        // State is only reset on prepare() (initial setup)
    }

    void resetState()
    {
        hp.reset();
        lp.reset();
    }

    float process (float x)
    {
        return lp.process (hp.process (x));
    }
};

// ── Simple FFT (Cooley-Tukey, power of 2) ────────────────────────────────────
static void fft (std::vector<float>& re, std::vector<float>& im)
{
    int n = (int) re.size();
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        double ang = -2.0 * M_PI / len;
        float wr = (float) std::cos (ang), wi = (float) std::sin (ang);
        for (int i = 0; i < n; i += len)
        {
            float cr = 1.f, ci = 0.f;
            for (int j = 0; j < len / 2; ++j)
            {
                float ur = re[i+j],         ui = im[i+j];
                float vr = re[i+j+len/2]*cr - im[i+j+len/2]*ci;
                float vi = re[i+j+len/2]*ci + im[i+j+len/2]*cr;
                re[i+j]         = ur + vr;  im[i+j]         = ui + vi;
                re[i+j+len/2]   = ur - vr;  im[i+j+len/2]   = ui - vi;
                float ncr = cr*wr - ci*wi;
                ci = cr*wi + ci*wr;  cr = ncr;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static constexpr int MAX_BANDS   = 5;
static constexpr int FFT_SIZE    = 1024;

struct SpectrumyserAddon
{
    // Band config
    int   bandCount = 3;
    float bandLow [MAX_BANDS] = { 20.f,   200.f,  2000.f,  8000.f, 16000.f };
    float bandHigh[MAX_BANDS] = { 200.f, 2000.f,  8000.f, 16000.f, 20000.f };

    // Per-band filters (stereo: 2 channels each)
    BandpassFilter filters[MAX_BANDS][2];

    double sampleRate = 44100.0;
    int    blockSize  = 512;

    // FFT accumulation buffer
    std::vector<float> fftBuf;
    int                fftPos = 0;

    // FFT magnitude output (for UI snapshot)
    std::vector<float> magnitudes;  // FFT_SIZE/2 bins

    void prepare (double sr, int bs)
    {
        sampleRate = sr;
        blockSize  = bs;
        fftBuf.assign (FFT_SIZE, 0.f);
        magnitudes.assign (FFT_SIZE / 2, 0.f);
        fftPos = 0;
        rebuildFilters();
        // Reset filter states on prepare only (not on parameter change)
        for (int b = 0; b < MAX_BANDS; ++b)
            for (int ch = 0; ch < 2; ++ch)
                filters[b][ch].resetState();
    }

    void rebuildFilters()
    {
        for (int b = 0; b < MAX_BANDS; ++b)
            for (int ch = 0; ch < 2; ++ch)
                filters[b][ch].setup (bandLow[b], bandHigh[b], sampleRate);
    }

    void updateFFT (const float* mono, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            fftBuf[fftPos] = mono[i];
            if (++fftPos >= FFT_SIZE)
            {
                fftPos = 0;
                computeFFT();
            }
        }
    }

    void computeFFT()
    {
        std::vector<float> re (FFT_SIZE), im (FFT_SIZE, 0.f);
        // Hann window
        for (int i = 0; i < FFT_SIZE; ++i)
        {
            float w = 0.5f * (1.f - std::cos (2.f * (float)M_PI * i / (FFT_SIZE - 1)));
            re[i] = fftBuf[i] * w;
        }
        fft (re, im);
        for (int i = 0; i < FFT_SIZE / 2; ++i)
        {
            float mag = std::sqrt (re[i]*re[i] + im[i]*im[i]) / (FFT_SIZE / 2);
            // Smooth with previous value
            magnitudes[i] = magnitudes[i] * 0.7f + mag * 0.3f;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

const PAX_Descriptor* PAX_getDescriptor()
{
    static PAX_Descriptor d {
        "Spectrumyser",    // name
        "Patchy",          // vendor
        "1.0.0",           // version
        2,                 // nodeType: Audio
        PAX_API_VERSION,
        1,                 // audioInputs
        3,                 // audioOutputs (default 3 bands — updated by bandCount param)
        0, 0
    };
    return &d;
}

PAX_Instance* PAX_create()         { return new SpectrumyserAddon(); }
void PAX_destroy (PAX_Instance* i) { delete static_cast<SpectrumyserAddon*>(i); }

void PAX_prepare (PAX_Instance* i, double sampleRate, int blockSize)
{
    static_cast<SpectrumyserAddon*>(i)->prepare (sampleRate, blockSize);
}

void PAX_process (PAX_Instance* i, const PAX_ProcessContext* ctx)
{
    if (! ctx->audioIn || ! ctx->audioOut) return;

    auto* s = static_cast<SpectrumyserAddon*>(i);

    // Update FFT with left channel
    if (ctx->audioIn[0])
        s->updateFFT (ctx->audioIn[0], ctx->numSamples);

    // Process each active band
    for (int b = 0; b < s->bandCount; ++b)
    {
        float* outL = ctx->audioOut[b * 2];
        float* outR = ctx->audioOut[b * 2 + 1];
        float* inL  = ctx->audioIn[0];
        float* inR  = ctx->audioIn[1] ? ctx->audioIn[1] : ctx->audioIn[0];

        if (! outL) continue;

        for (int n = 0; n < ctx->numSamples; ++n)
        {
            float l = inL ? s->filters[b][0].process (inL[n]) : 0.f;
            float r = inR ? s->filters[b][1].process (inR[n]) : l;
            if (outL) outL[n] = l;
            if (outR) outR[n] = r;
        }
    }
}

int PAX_getParameterCount (PAX_Instance*) { return 1 + MAX_BANDS * 2; }

void PAX_getParameterInfo (PAX_Instance* i, int index, PAX_ParameterInfo* info)
{
    auto* s = static_cast<SpectrumyserAddon*>(i);
    if (index == 0)
    {
        info->name         = "Bands";
        info->minValue     = 1.f;
        info->maxValue     = 5.f;
        info->defaultValue = 3.f;
        info->step         = 1.f;
        return;
    }
    int band = (index - 1) / 2;
    bool isLow = ((index - 1) % 2) == 0;

    static char nameBuf[32];
    snprintf (nameBuf, sizeof(nameBuf), "Band %d %s Hz", band + 1, isLow ? "Low" : "High");
    info->name         = nameBuf;
    info->minValue     = 20.f;
    info->maxValue     = 20000.f;
    info->defaultValue = isLow ? s->bandLow[band] : s->bandHigh[band];
    info->step         = 0.f;
}

float PAX_getParameter (PAX_Instance* i, int index)
{
    auto* s = static_cast<SpectrumyserAddon*>(i);
    if (index == 0) return (float) s->bandCount;
    int band  = (index - 1) / 2;
    bool isLow = ((index - 1) % 2) == 0;
    return isLow ? s->bandLow[band] : s->bandHigh[band];
}

void PAX_setParameter (PAX_Instance* i, int index, float value)
{
    auto* s = static_cast<SpectrumyserAddon*>(i);
    if (index == 0)
    {
        s->bandCount = (int) std::clamp (value, 1.f, 5.f);
        return;
    }
    int band  = (index - 1) / 2;
    bool isLow = ((index - 1) % 2) == 0;
    if (band < MAX_BANDS)
    {
        if (isLow)  s->bandLow [band] = std::clamp (value, 20.f, 20000.f);
        else        s->bandHigh[band] = std::clamp (value, 20.f, 20000.f);
        s->rebuildFilters();
    }
}

// Dynamic port count — returns bandCount (one stereo port per band)
int PAX_getAudioOutputCount (PAX_Instance* i)
{
    return static_cast<SpectrumyserAddon*>(i)->bandCount;
}

// Extra: expose FFT magnitudes for UI snapshot
// Returns magnitudes as a packed float array via a special parameter index
int PAX_getFFTSize (PAX_Instance*) { return FFT_SIZE / 2; }

const float* PAX_getFFTMagnitudes (PAX_Instance* i)
{
    return static_cast<SpectrumyserAddon*>(i)->magnitudes.data();
}

} // extern "C"
