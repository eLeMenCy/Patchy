#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <vector>
#include <cmath>

/**
 * AudioPlayerState  (owned by PatchyProcessor, survives graph rebuilds)
 *
 * AudioPlayerNode is a SOURCE, not a monitor — unlike AudioMonitorBuffer's
 * short scrolling-history ring buffer, this holds the full, real state a
 * player genuinely needs to keep working correctly across a rebuild: the
 * entire loaded file (once resampled to the graph's own rate), the current
 * playback position, and play/pause/loop status. Every node instance is
 * destroyed and recreated on every single graph edit anywhere (this
 * project's own long-established, unavoidable pattern) — without this
 * living outside the node instance itself, loading a file and pressing
 * play would be undone by the next completely unrelated edit anywhere in
 * the graph, making the node nearly unusable in normal use.
 *
 * Mode/frequency/noise type/level/loop/file path are the small, discrete,
 * user-set values also mirrored into settingsJson for cross-session
 * persistence (surviving a full app restart, not just a rebuild) — this
 * struct holds the live, thread-safe runtime copies actually read by
 * process(), restored from settingsJson once per rebuild rather than
 * parsed on every single audio block.
 */
struct AudioPlayerState
{
    enum class Mode  { File = 0, Sine = 1, Noise = 2 };
    enum class Noise { White = 0, Pink = 1 };

    // ── Loaded file data (message thread writes on load; audio thread only
    //    reads, never during a load itself — see loadFile()'s own comment) ──
    juce::AudioBuffer<float> fileBuffer;     // already resampled to the graph's own rate
    juce::String             fileName;       // display name only (e.g. "kick.wav")
    double                   fileSampleRate = 0.0;   // the FILE's own original rate, info only
    std::vector<std::pair<float, float>> waveformPeaks;   // (min,max) per bucket, computed once on load

    // ── Playback control (message thread writes via UI actions; audio
    //    thread is the sole writer of playbackPosition — see process()'s
    //    own comment for why a plain load-modify-store from another
    //    thread would race it) ──
    std::atomic<int64_t> playbackPosition { 0 };     // sample index into fileBuffer
    std::atomic<int64_t> seekRequest      { -1 };    // -1 = no pending seek; else target sample index
    std::atomic<bool>    playing          { false };
    std::atomic<bool>    looping          { false };

    // Set by loadFile() on every successful load, regardless of which path
    // triggered it (manual browse or the restore-on-rebuild path) — the
    // restore path loads the file correctly on the backend (which is why
    // playback works after a project reload) but has no direct way to push
    // anything to the UI itself (see restoreAudioPlayerSettings()'s own
    // comment on why it can't push events directly), unlike the manual
    // browse path's own dedicated callback. The periodic status push
    // consumes this flag and reports the file + peaks the same way either
    // path's own load would, so the frontend's own waveform never silently
    // stays empty after a reload.
    std::atomic<bool>    needsUIPush     { false };

    // ── Small, discrete settings — restored from settingsJson once per
    //    rebuild (see AudioPlayerNode.cpp's own restoreAudioPlayerSettings) ──
    std::atomic<Mode>  mode          { Mode::File };
    std::atomic<float> sineFrequency { 440.0f };
    std::atomic<Noise> noiseType     { Noise::White };
    std::atomic<float> level         { 0.1f };   // deliberately quiet by default — see AudioPlayerNode.tsx's own DEFAULT_SETTINGS.level comment

    // ── Generator-internal state — audio-thread-only, no cross-thread
    //    concern, but kept here (not on the node instance) so a sine
    //    doesn't click or a pink-noise filter doesn't reset its own
    //    colouring on every unrelated graph edit ──
    double sinePhase = 0.0;
    float  pinkB0 = 0.0f, pinkB1 = 0.0f, pinkB2 = 0.0f;
    juce::Random noiseRandom;

    /** Loads an audio file fully into memory, resampling to targetSampleRate
     *  if the file's own rate differs. Called on the message thread (from
     *  the async FileChooser's own completion callback) — never from the
     *  audio thread. Returns true on success. */
    bool loadFile (const juce::File& file, double targetSampleRate);

    /** Computes a fixed-size (min,max) peaks summary across the whole
     *  buffer, for a static waveform display — independent of the file's
     *  own length, so a 3-minute file and a 3-second file send the same
     *  small amount of data to the UI. */
    void computeWaveformPeaks (int numBuckets = 600);
};

/**
 * AudioPlayerNode  (nodeType 26)
 *
 * 1 Audio Out only — this is a source, it has no audio input at all.
 * Holds a raw pointer to an AudioPlayerState owned by PatchyProcessor.
 */
class AudioPlayerNode : public NodeProcessor
{
public:
    AudioPlayerNode (const juce::String& nodeId, AudioPlayerState* sharedState)
        : NodeProcessor (nodeId, Type::Audio), state (sharedState) {}

    /** For WebBridge's own periodic status push (see pushAudioPlayerStatus())
     *  — the frontend has no other way to track a continuously-advancing
     *  playhead, or to notice a non-looping file having reached its own
     *  end and auto-stopped, without this. Returns 0.0-1.0, or 0.0 if
     *  there's no file loaded at all. */
    double getPlaybackFraction() const
    {
        if (state == nullptr) return 0.0;
        int64_t numSamples = state->fileBuffer.getNumSamples();
        if (numSamples <= 0) return 0.0;
        int64_t pos = state->playbackPosition.load (std::memory_order_relaxed);
        return juce::jlimit (0.0, 1.0, (double) pos / (double) numSamples);
    }

    bool isPlaying() const
    {
        return state != nullptr && state->playing.load (std::memory_order_relaxed);
    }

    void process (int numSamples) override
    {
        if (state == nullptr) { outputAudio.clear(); return; }

        const float lvl = state->level.load (std::memory_order_relaxed);

        switch (state->mode.load (std::memory_order_relaxed))
        {
            case AudioPlayerState::Mode::File:   processFile  (numSamples, lvl); break;
            case AudioPlayerState::Mode::Sine:   processSine  (numSamples, lvl); break;
            case AudioPlayerState::Mode::Noise:  processNoise (numSamples, lvl); break;
        }
    }

private:
    void processFile (int numSamples, float lvl)
    {
        const int numFileChannels = state->fileBuffer.getNumChannels();
        const int numFileSamples  = state->fileBuffer.getNumSamples();

        if (numFileChannels == 0 || numFileSamples == 0)
        {
            outputAudio.clear();
            return;
        }

        // Consume any pending seek FIRST, REGARDLESS of playing state — a
        // real, once-shipped bug had this consumption happen only after an
        // early return for "not playing", meaning a seek requested while
        // paused was silently dropped until play resumed, then jumped all
        // at once. A seek should move the playhead immediately, even
        // though no audio is actually output until playback resumes. This
        // node's own process() is the sole writer of playbackPosition (see
        // its own declaration comment for why a plain cross-thread store
        // would race the read-advance-store sequence below).
        int64_t pendingSeek = state->seekRequest.exchange (-1, std::memory_order_acq_rel);
        int64_t pos = (pendingSeek >= 0) ? pendingSeek
                                          : state->playbackPosition.load (std::memory_order_relaxed);
        // A seek can in principle land past the end (e.g. a shorter file
        // loaded after the request was made) — clamp defensively.
        pos = juce::jlimit ((int64_t) 0, (int64_t) numFileSamples, pos);

        if (! state->playing.load (std::memory_order_relaxed))
        {
            // Not playing — commit the (possibly just-seeked) position so
            // the UI reflects it right away, but output silence.
            state->playbackPosition.store (pos, std::memory_order_relaxed);
            outputAudio.clear();
            return;
        }

        const bool loop = state->looping.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            if (pos >= numFileSamples)
            {
                if (loop) { pos = 0; }
                else
                {
                    // Reached the end without looping — stop playback and
                    // output silence for the remainder of this block.
                    state->playing.store (false, std::memory_order_relaxed);
                    for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
                        outputAudio.clear (ch, i, numSamples - i);
                    break;
                }
            }

            for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
            {
                // Mono files duplicate to both L/R; a genuinely stereo (or
                // wider) file's own extra channels beyond what's available
                // simply repeat the last one rather than reading out of range.
                int srcCh = juce::jmin (ch, numFileChannels - 1);
                outputAudio.setSample (ch, i, state->fileBuffer.getSample (srcCh, (int) pos) * lvl);
            }

            ++pos;
        }

        state->playbackPosition.store (pos, std::memory_order_relaxed);
    }

    void processSine (int numSamples, float lvl)
    {
        const double freq = (double) state->sineFrequency.load (std::memory_order_relaxed);
        const double increment = 2.0 * juce::MathConstants<double>::pi * freq / currentSampleRate;

        const bool isPlaying = state->playing.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            // A real, confirmed bug had this generator running unconditionally,
            // regardless of play/pause state — Sine/Noise modes were always
            // audible and Play/Pause had no effect at all. Phase still
            // advances while paused (harmless — no audible artifact is
            // possible while silent, and this avoids needing extra state
            // just to freeze/resume it).
            float s = isPlaying ? (float) std::sin (state->sinePhase) * lvl : 0.0f;
            for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
                outputAudio.setSample (ch, i, s);

            state->sinePhase += increment;
            if (state->sinePhase >= 2.0 * juce::MathConstants<double>::pi)
                state->sinePhase -= 2.0 * juce::MathConstants<double>::pi;
        }
    }

    void processNoise (int numSamples, float lvl)
    {
        const bool pink = state->noiseType.load (std::memory_order_relaxed) == AudioPlayerState::Noise::Pink;
        const bool isPlaying = state->playing.load (std::memory_order_relaxed);

        for (int i = 0; i < numSamples; ++i)
        {
            float white = state->noiseRandom.nextFloat() * 2.0f - 1.0f;   // -1..1
            float s;

            if (pink)
            {
                // Paul Kellet's "economy" pink noise filter (+/-0.5dB
                // accuracy) — verified against multiple independent public
                // references before use, not assumed from memory alone.
                // Kept continuously fed even while paused (see below) so
                // its own internal colouring doesn't decay and need to
                // ramp back up audibly when playback resumes.
                state->pinkB0 = 0.99765f * state->pinkB0 + white * 0.0990460f;
                state->pinkB1 = 0.96300f * state->pinkB1 + white * 0.2965164f;
                state->pinkB2 = 0.57000f * state->pinkB2 + white * 1.0526913f;
                s = (state->pinkB0 + state->pinkB1 + state->pinkB2 + white * 0.1848f) * 0.2f;   // scaled back toward +/-1
            }
            else
            {
                s = white;
            }

            // A real, confirmed bug had this generator running unconditionally,
            // regardless of play/pause state — Sine/Noise modes were always
            // audible and Play/Pause had no effect at all.
            s = isPlaying ? s * lvl : 0.0f;
            for (int ch = 0; ch < outputAudio.getNumChannels(); ++ch)
                outputAudio.setSample (ch, i, s);
        }
    }

    AudioPlayerState* state = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPlayerNode)
};
