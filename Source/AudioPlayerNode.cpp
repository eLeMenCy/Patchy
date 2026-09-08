#include "AudioPlayerNode.h"

bool AudioPlayerState::loadFile (const juce::File& file, double targetSampleRate)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();   // WAV/AIFF always available
    // FLAC/OggVorbis/MP3 registered here too, IF the relevant JUCE_USE_*
    // flags are enabled at compile time (see CMakeLists.txt) — codecs
    // simply aren't compiled in otherwise, registerBasicFormats() alone
    // never includes them.

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        juce::Logger::writeToLog ("AudioPlayerState: failed to open " + file.getFullPathName());
        return false;
    }

    const int   numFileChannels = (int) reader->numChannels;
    const int64_t numFileSamples  = reader->lengthInSamples;
    const double sourceSampleRate = reader->sampleRate;

    if (numFileChannels <= 0 || numFileSamples <= 0 || sourceSampleRate <= 0.0)
    {
        juce::Logger::writeToLog ("AudioPlayerState: " + file.getFullPathName() + " has no readable audio data");
        return false;
    }

    // Read the whole file, at its own native rate, into a temporary buffer —
    // resampling (below) needs the complete signal available up front, not
    // streamed block by block, since this is a one-shot, whole-file load,
    // not a real-time read.
    juce::AudioBuffer<float> raw (numFileChannels, (int) numFileSamples);
    reader->read (&raw, 0, (int) numFileSamples, 0, true, true);

    juce::AudioBuffer<float> resampled;

    // Plain, direct comparison rather than juce::approximatelyEqual() — a
    // relatively recent JUCE addition, avoided here since this project's
    // own exact JUCE version isn't directly confirmed; a simple epsilon
    // check needs no version assumption at all. Sample rates in practice
    // are always exact values (44100.0, 48000.0, etc), so an exact `==`
    // would likely work too, but a small tolerance costs nothing and
    // guards against any stray floating-point noise.
    if (std::abs (sourceSampleRate - targetSampleRate) < 0.01)
    {
        resampled = std::move (raw);
    }
    else
    {
        // speedRatio is documented as "the number of input samples to use
        // for each output sample" — source/target, not target/source
        // (verified against JUCE's own official parameter description
        // before use, since at least one public forum example appeared to
        // use the inverse — downsampling, e.g. 48kHz file into a 44.1kHz
        // graph, genuinely needs MORE than 1 input sample per output
        // sample, confirming source/target is the correct direction).
        const double speedRatio = sourceSampleRate / targetSampleRate;
        const int    outLength  = (int) std::ceil ((double) numFileSamples / speedRatio);

        resampled.setSize (numFileChannels, outLength);

        for (int ch = 0; ch < numFileChannels; ++ch)
        {
            juce::LagrangeInterpolator interpolator;
            interpolator.process (speedRatio, raw.getReadPointer (ch),
                                  resampled.getWritePointer (ch), outLength);
        }
    }

    // Mono duplicates to stereo (both L/R outputs carry the same signal) —
    // a genuinely wider file (3+ channels) is left as-is; AudioPlayerNode's
    // own process() already handles reading from however many channels
    // are actually here, repeating the last one if the graph asks for more.
    if (resampled.getNumChannels() == 1)
    {
        juce::AudioBuffer<float> stereo (2, resampled.getNumSamples());
        stereo.copyFrom (0, 0, resampled, 0, 0, resampled.getNumSamples());
        stereo.copyFrom (1, 0, resampled, 0, 0, resampled.getNumSamples());
        resampled = std::move (stereo);
    }

    fileBuffer      = std::move (resampled);
    fileName        = file.getFileName();
    fileSampleRate  = sourceSampleRate;

    playbackPosition.store (0, std::memory_order_relaxed);
    seekRequest.store (-1, std::memory_order_relaxed);
    playing.store (false, std::memory_order_relaxed);

    computeWaveformPeaks();
    needsUIPush.store (true, std::memory_order_relaxed);

    juce::Logger::writeToLog ("AudioPlayerState: loaded " + fileName
                              + " (" + juce::String (sourceSampleRate, 0) + "Hz -> "
                              + juce::String (targetSampleRate, 0) + "Hz, "
                              + juce::String (fileBuffer.getNumSamples()) + " samples)");
    return true;
}

void AudioPlayerState::computeWaveformPeaks (int numBuckets)
{
    waveformPeaks.clear();

    const int numSamples = fileBuffer.getNumSamples();
    if (numSamples <= 0) return;

    numBuckets = juce::jmax (1, numBuckets);
    waveformPeaks.reserve ((size_t) numBuckets);

    const int numChannels = fileBuffer.getNumChannels();

    for (int b = 0; b < numBuckets; ++b)
    {
        int64_t startSample = ((int64_t) b * numSamples) / numBuckets;
        int64_t endSample   = ((int64_t) (b + 1) * numSamples) / numBuckets;
        if (endSample <= startSample) endSample = startSample + 1;
        endSample = juce::jmin (endSample, (int64_t) numSamples);

        float mn = 0.0f, mx = 0.0f;
        for (int64_t s = startSample; s < endSample; ++s)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                float v = fileBuffer.getSample (ch, (int) s);
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
            }
        }
        waveformPeaks.push_back ({ mn, mx });
    }
}
