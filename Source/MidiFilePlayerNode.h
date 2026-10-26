#pragma once
// MidiFilePlayerNode.h — v0.0.954 (user's idea, design agreed 2026-10-25)
//
// Plays a standard MIDI file (.mid) into the graph: 1 MIDI output, no input.
//   • Tempo (3-state button on the node): File (the file's own tempo map, the
//     default) / Host (the DAW's tempo AND transport: plays when it plays,
//     follows its position — plugin only; the Standalone falls back to File)
//     / Manual (own BPM).
//   • Tracks: all played together, each event keeps its own channel (as every
//     player / DAW does); per-track mute; optional "Force channel" (1-16).
//   • Transport: play / pause, back to start, seek, loop. Pause / stop / seek /
//     a jump of the host's position send a Note Off for every sounding note.
//
// Same "survives every graph rebuild" design as the Audio Player: the loaded
// file and the playback position live in MidiFilePlayerState, owned by
// PatchyProcessor; the node instance (recreated on every edit) only points to
// it. Settings (file path, tempo mode, BPM, loop, force channel, muted tracks)
// are also kept in the node's settingsJson and restored on each rebuild.
#include "NodeProcessor.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

struct MidiFileSequence
{
    struct Event { double tick; juce::uint8 bytes[3]; int size; int track; };
    struct Tempo { double tick; double usPerQuarter; };
    struct Track { juce::String name; std::uint16_t channels = 0; int notes = 0; };

    std::vector<Event> events;    // channel-voice messages only, sorted by tick
    std::vector<Tempo> tempos;    // sorted; always at least one (tick 0)
    std::vector<Track> tracks;
    double ppq         = 480.0;   // ticks per quarter note
    double lengthTicks = 0.0;
    juce::String fileName, filePath;
    bool isDemo = false;          // v0.0.955 — the built-in demo (no file set)

    double usPerQuarterAt (double tick) const
    {
        double us = tempos.empty() ? 500000.0 : tempos.front().usPerQuarter;
        for (const auto& t : tempos) { if (t.tick > tick) break; us = t.usPerQuarter; }
        return us;
    }
    double fileBpmAt (double tick) const { return 60000000.0 / usPerQuarterAt (tick); }

    /** Message thread. nullptr if the file can't be read. */
    static std::unique_ptr<MidiFileSequence> load (const juce::File& f)
    {
        juce::FileInputStream in (f);
        if (! in.openedOk()) return nullptr;
        juce::MidiFile mf;
        if (! mf.readFrom (in, true)) return nullptr;

        auto seq = std::make_unique<MidiFileSequence>();
        seq->fileName = f.getFileName();
        seq->filePath = f.getFullPathName();

        // SMPTE time format (rare): play it on seconds, as 120 BPM / 960 ppq
        const short tf = mf.getTimeFormat();
        const bool smpte = tf <= 0;
        if (smpte) mf.convertTimestampTicksToSeconds();
        seq->ppq = smpte ? 960.0 : (double) tf;
        auto toTick = [&] (double t) { return smpte ? t * 2.0 * 960.0 : t; };   // 120 BPM: 2 quarters per second

        for (int ti = 0; ti < mf.getNumTracks(); ++ti)
        {
            const auto* tr = mf.getTrack (ti);
            Track info;
            for (int i = 0; i < tr->getNumEvents(); ++i)
            {
                const auto& m = tr->getEventPointer (i)->message;
                const double tick = toTick (m.getTimeStamp());
                seq->lengthTicks = std::max (seq->lengthTicks, tick);
                if (m.isTrackNameEvent() && info.name.isEmpty()) { info.name = m.getTextFromTextMetaEvent(); continue; }
                if (m.isTempoMetaEvent() && ! smpte)
                {
                    seq->tempos.push_back ({ tick, m.getTempoSecondsPerQuarterNote() * 1000000.0 });
                    continue;
                }
                if (m.isMetaEvent() || m.isSysEx() || m.getRawDataSize() > 3) continue;
                const int st = m.getRawData()[0] & 0xF0;
                if (st < 0x80 || st > 0xE0) continue;   // channel-voice only
                Event ev {};
                ev.tick = tick; ev.size = m.getRawDataSize(); ev.track = ti;
                std::memcpy (ev.bytes, m.getRawData(), (size_t) ev.size);
                seq->events.push_back (ev);
                info.channels |= (std::uint16_t) (1u << (m.getChannel() - 1));
                if (m.isNoteOn()) ++info.notes;
            }
            if (info.name.isEmpty()) info.name = "Track " + juce::String (ti + 1);
            seq->tracks.push_back (info);
        }
        std::stable_sort (seq->events.begin(), seq->events.end(), [] (const Event& a, const Event& b) { return a.tick < b.tick; });
        std::stable_sort (seq->tempos.begin(), seq->tempos.end(), [] (const Tempo& a, const Tempo& b) { return a.tick < b.tick; });
        if (seq->tempos.empty() || seq->tempos.front().tick > 0.0) seq->tempos.insert (seq->tempos.begin(), { 0.0, 500000.0 });
        return seq;
    }
};

// v0.0.955 — the built-in demo, "Catchy Patchy", played when no file is set: 8 bars of
// melodic trance in A minor at 138 BPM (Am – F – C – G, 2 bars each), five
// tracks on their own channels, so mute / force channel can be tried at once.
inline std::unique_ptr<MidiFileSequence> makeDemoSequence()
{
    auto seq = std::make_unique<MidiFileSequence>();
    seq->ppq = 480.0;
    seq->fileName = "Catchy Patchy";   // the project's mascot tune (named by the user)
    seq->isDemo = true;
    seq->tempos.push_back ({ 0.0, 60000000.0 / 138.0 });
    const double Q = seq->ppq;              // one beat
    const int bars = 8;
    seq->lengthTicks = bars * 4 * Q;

    auto note = [&] (int track, int ch, double beat, double len, int n, int vel)
    {
        MidiFileSequence::Event on {}, off {};
        on.tick  = beat * Q;                 on.size  = 3; on.track  = track;
        off.tick = (beat + len) * Q;         off.size = 3; off.track = track;
        on.bytes[0]  = (juce::uint8) (0x90 | (ch - 1)); on.bytes[1]  = (juce::uint8) n; on.bytes[2]  = (juce::uint8) vel;
        off.bytes[0] = (juce::uint8) (0x80 | (ch - 1)); off.bytes[1] = (juce::uint8) n; off.bytes[2] = 0;
        seq->events.push_back (on);
        seq->events.push_back (off);
        auto& t = seq->tracks[(size_t) track];
        t.channels |= (std::uint16_t) (1u << (ch - 1));
        ++t.notes;
    };
    for (auto* name : { "Drums", "Bass", "Pad", "Arp", "Lead" })
    {
        MidiFileSequence::Track t; t.name = name; seq->tracks.push_back (t);
    }

    // chords per 2 bars: root (bass, octave 1) and pad voicing
    struct Chord { int root; int pad[3]; int arp[4]; };
    const Chord chords[4] = {
        { 33, { 57, 60, 64 }, { 69, 72, 76, 81 } },   // Am
        { 29, { 57, 60, 65 }, { 65, 69, 72, 77 } },   // F
        { 36, { 55, 60, 64 }, { 67, 72, 76, 79 } },   // C
        { 31, { 55, 59, 62 }, { 67, 71, 74, 79 } },   // G
    };

    for (int bar = 0; bar < bars; ++bar)
    {
        const Chord& c = chords[bar / 2];
        for (int b = 0; b < 4; ++b)
        {
            const double beat = bar * 4 + b;
            // Drums (ch 10): four-on-the-floor kick, clap on 2 & 4, offbeat open hat, 16th closed hats
            note (0, 10, beat, 0.25, 36, 112);
            if (b == 1 || b == 3) note (0, 10, beat, 0.25, 39, 96);
            note (0, 10, beat + 0.5, 0.25, 46, 78);
            for (int s : { 1, 3 }) note (0, 10, beat + s * 0.25, 0.125, 42, 52);
            // Bass (ch 2): rolling offbeat 16ths (the kick takes the beat)
            for (int s = 1; s < 4; ++s) note (1, 2, beat + s * 0.25, 0.2, c.root + (s == 2 ? 12 : 0), s == 2 ? 104 : 92);
            // Arp (ch 4): 16ths up and down the chord
            static const int pat[4] = { 0, 1, 2, 3 };
            for (int s = 0; s < 4; ++s)
            {
                const int idx = (b % 2 == 0) ? pat[s] : 3 - pat[s];
                note (3, 4, beat + s * 0.25, 0.2, c.arp[idx], s == 0 ? 92 : 70);
            }
        }
        // Pad (ch 3): the chord held over its 2 bars
        if (bar % 2 == 0)
            for (int n : c.pad) note (2, 3, bar * 4, 7.9, n, 64);
    }
    // Snare roll into the loop (bar 8, beats 3-4, rising)
    for (int s = 0; s < 8; ++s) note (0, 10, 30 + s * 0.25, 0.125, 38, 60 + s * 8);

    // Lead (ch 5): the hook — {beat, length, note}
    const double lead[][3] = {
        { 0, 1, 76 }, { 1, .5, 74 }, { 1.5, .5, 72 }, { 2, 1, 74 }, { 3, 1, 76 },
        { 4, 2, 81 }, { 6, 1, 79 }, { 7, 1, 76 },
        { 8, 1, 77 }, { 9, 1, 76 }, { 10, 1, 72 }, { 11, 1, 69 },
        { 12, 2, 72 }, { 14, 1, 74 }, { 15, 1, 76 },
        { 16, 1, 79 }, { 17, 1, 76 }, { 18, 1, 72 }, { 19, 1, 76 },
        { 20, 2, 79 }, { 22, 1, 81 }, { 23, 1, 79 },
        { 24, 1, 74 }, { 25, 1, 71 }, { 26, 1, 74 }, { 27, 1, 79 },
        { 28, 4, 76 },
    };
    for (const auto& l : lead) note (4, 5, l[0], l[1] * 0.9, (int) l[2], 100);

    // note-offs before note-ons at the same tick (a repeated note restarts cleanly)
    std::stable_sort (seq->events.begin(), seq->events.end(), [] (const MidiFileSequence::Event& a, const MidiFileSequence::Event& b)
    {
        if (a.tick < b.tick || b.tick < a.tick) return a.tick < b.tick;   // (no float ==: -Wfloat-equal)
        return (a.bytes[0] & 0xF0) == 0x80 && (b.bytes[0] & 0xF0) != 0x80;
    });
    return seq;
}

struct MidiFilePlayerState
{
    enum class TempoMode { File = 0, Host = 1, Manual = 2 };

    // The sequence: swapped on the message thread under `lock`; the audio
    // thread only TRY-locks it (a block that loses the race simply plays
    // nothing). The old one is destroyed on the message thread.
    juce::SpinLock                    lock;
    std::unique_ptr<MidiFileSequence> seq;

    std::atomic<bool>      playing      { false };
    std::atomic<bool>      looping      { false };
    std::atomic<TempoMode> tempoMode    { TempoMode::File };
    std::atomic<double>    manualBpm    { 120.0 };
    std::atomic<int>       forceChannel { 0 };        // 0 = off, else 1-16
    std::atomic<std::uint64_t> muteMask { 0 };         // bit n = track n muted (first 64)
    std::atomic<double>    seekFraction { -1.0 };     // pending seek, 0-1; -1 none
    std::atomic<bool>      notesOffRequest { false };
    std::atomic<bool>      needsUIPush  { false };    // file info to (re)send

    // Audio thread only (positions read for the status push are atomics)
    std::atomic<double>    position     { 0.0 };      // ticks
    std::atomic<double>    currentBpm   { 120.0 };
    std::atomic<double>    lengthTicks  { 0.0 };
    std::atomic<bool>      hostFollowing { false };   // Host mode actually in use (plugin + valid playhead)
    std::atomic<bool>      shownPlaying  { false };   // what the ▶ / ❚❚ button shows (the DAW's transport in Host mode)
    std::array<std::bitset<128>, 16> held {};                 // sounding notes per output channel
    std::array<std::array<std::bitset<128>, 16>, 64> heldByTrack {};   // … and which track played them (mute releases only those)
    std::uint64_t appliedMute = 0;                            // mask the audio thread last applied
    std::atomic<bool> panicHold { false };                    // after a Panic: silent until ▶ (own transport) or the DAW stops (HOST)
    bool   wasPlaying = false;
    double expectedHostTick = -1.0;

    MidiFilePlayerState() { install (makeDemoSequence()); }   // no file set: the demo

    /** Message thread. */
    bool loadFile (const juce::File& f)
    {
        auto next = MidiFileSequence::load (f);
        if (next == nullptr) return false;
        install (std::move (next));
        return true;
    }
    /** Message thread — back to the built-in demo. */
    void loadDemo() { install (makeDemoSequence()); }

    bool isDemo()
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        return seq != nullptr && seq->isDemo;
    }

private:
    void install (std::unique_ptr<MidiFileSequence> next)
    {
        lengthTicks.store (next->lengthTicks);
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            std::swap (seq, next);   // old one leaves with `next`, destroyed here (message thread)
        }
        seekFraction.store (0.0);
        notesOffRequest.store (true);
        needsUIPush.store (true);
    }
};

class MidiFilePlayerNode : public NodeProcessor
{
public:
    MidiFilePlayerNode (const juce::String& nodeId, MidiFilePlayerState* shared)
        : NodeProcessor (nodeId, Type::Midi), state (shared) {}

    // Set by the graph before each block (PatchyProcessor reads the playhead)
    void setHost (bool valid, bool hostPlaying, double bpm, double ppqPosition)
    {
        hostValid = valid; hostIsPlaying = hostPlaying; hostBpm = bpm; hostPpq = ppqPosition;
    }

    void process (int numSamples) override
    {
        outputMidi.clear();
        if (state == nullptr) return;
        if (disabled)   // ⏻ off: silent, the position stays put, nothing left hanging
        {
            const juce::SpinLock::ScopedTryLockType sl (state->lock);
            if (sl.isLocked()) allNotesOff (0);
            state->shownPlaying.store (false, std::memory_order_relaxed);
            state->wasPlaying = false;
            state->expectedHostTick = -1.0;
            return;
        }

        const juce::SpinLock::ScopedTryLockType sl (state->lock);
        if (! sl.isLocked()) return;   // the message thread is swapping / reading the file: skip this block, nothing cut
        if (state->seq == nullptr) { allNotesOff (0); return; }
        const auto& seq = *state->seq;

        const auto mode   = state->tempoMode.load (std::memory_order_relaxed);
        const bool follow = mode == MidiFilePlayerState::TempoMode::Host && hostValid;
        state->hostFollowing.store (follow, std::memory_order_relaxed);

        double pos = state->position.load (std::memory_order_relaxed);

        if (state->notesOffRequest.exchange (false)) allNotesOff (0);
        const auto mute = state->muteMask.load (std::memory_order_relaxed);
        if (const auto newlyMuted = mute & ~state->appliedMute)
            for (int t = 0; t < 64; ++t) if ((newlyMuted >> t) & 1u) releaseTrack (t, 0);
        state->appliedMute = mute;
        const double seek = state->seekFraction.exchange (-1.0);
        if (seek >= 0.0) { allNotesOff (0); pos = seek * seq.lengthTicks; }

        bool playingNow;
        if (follow)
        {
            if (! hostIsPlaying) state->panicHold.store (false, std::memory_order_relaxed);   // the DAW stopped: armed again
            playingNow = hostIsPlaying && ! state->panicHold.load (std::memory_order_relaxed);
            const double hostTick = std::max (0.0, hostPpq) * seq.ppq;
            if (playingNow && (state->expectedHostTick < 0.0 || std::abs (hostTick - state->expectedHostTick) > seq.ppq / 8.0))
                allNotesOff (0);   // the DAW jumped (loop, locate): nothing hangs
            pos = state->looping.load() && seq.lengthTicks > 0.0 ? std::fmod (hostTick, seq.lengthTicks) : hostTick;
        }
        else
            playingNow = state->playing.load (std::memory_order_relaxed) && ! state->panicHold.load (std::memory_order_relaxed);

        state->shownPlaying.store (playingNow, std::memory_order_relaxed);
        if (! playingNow)
        {
            if (state->wasPlaying) allNotesOff (0);
            state->wasPlaying = false;
            state->expectedHostTick = -1.0;
            state->position.store (pos, std::memory_order_relaxed);
            return;
        }
        state->wasPlaying = true;

        const double bpm = follow ? (hostBpm > 0.0 ? hostBpm : seq.fileBpmAt (pos))
                         : mode == MidiFilePlayerState::TempoMode::Manual ? std::max (1.0, state->manualBpm.load())
                         : seq.fileBpmAt (pos);
        state->currentBpm.store (bpm, std::memory_order_relaxed);
        const double ticksPerSample = bpm / 60.0 * seq.ppq / std::max (1.0, currentSampleRate);
        const double end = pos + numSamples * ticksPerSample;
        if (follow) state->expectedHostTick = std::max (0.0, hostPpq) * seq.ppq + numSamples * ticksPerSample;

        emitRange (seq, pos, std::min (end, seq.lengthTicks + 1.0), pos, ticksPerSample, numSamples);

        double next = end;
        if (end > seq.lengthTicks)
        {
            if (state->looping.load (std::memory_order_relaxed) && seq.lengthTicks > 0.0)
            {
                const int offset = (int) std::min ((double) numSamples - 1, (seq.lengthTicks - pos) / ticksPerSample);
                allNotesOff (std::max (0, offset));
                next = std::fmod (end - seq.lengthTicks, std::max (1.0, seq.lengthTicks));
                emitRange (seq, 0.0, next, -(seq.lengthTicks - pos), ticksPerSample, numSamples);
            }
            else if (! follow)
            {
                allNotesOff (numSamples - 1);
                state->playing.store (false, std::memory_order_relaxed);   // end of a non-looping file: back to start
                state->wasPlaying = false;
                next = 0.0;
            }
        }
        state->position.store (next, std::memory_order_relaxed);
        if (! outputMidi.isEmpty()) recordMidiActivity (outputMidi.getNumEvents());
    }

    /** v0.0.954 — main Panic (audio thread, before process()): stops the
        player (HOST mode: silent until the DAW's transport stops) and
        releases everything it was sounding. */
    void panicStop()
    {
        if (state == nullptr) return;
        // whatever mode this block thinks it is in (a block run by the idle
        // driver may have no playhead): the hold covers both
        state->playing.store (false, std::memory_order_relaxed);
        state->panicHold.store (true, std::memory_order_relaxed);
        state->notesOffRequest.store (true);
    }

    // process() runs while ⏻ is off, only to release the sounding notes
    bool passesThroughWhenDisabled() const override { return true; }

    double getFraction() const
    {
        const double len = state ? state->lengthTicks.load() : 0.0;
        return len > 0.0 ? juce::jlimit (0.0, 1.0, state->position.load() / len) : 0.0;
    }
    bool isPlaying() const
    {
        if (! state) return false;
        return state->hostFollowing.load() ? state->shownPlaying.load() : state->playing.load();
    }
    double getBpm() const { return state ? state->currentBpm.load() : 0.0; }
    bool   isFollowingHost() const { return state && state->hostFollowing.load(); }

private:
    // Events with tick in [from, to) → offsets measured from `origin` (ticks)
    void emitRange (const MidiFileSequence& seq, double from, double to, double origin, double tps, int numSamples)
    {
        if (to <= from) return;
        const auto mute  = state->muteMask.load (std::memory_order_relaxed);
        const int  force = state->forceChannel.load (std::memory_order_relaxed);
        auto it = std::lower_bound (seq.events.begin(), seq.events.end(), from,
                                    [] (const MidiFileSequence::Event& e, double t) { return e.tick < t; });
        for (; it != seq.events.end() && it->tick < to; ++it)
        {
            if (it->track < 64 && ((mute >> it->track) & 1u)) continue;
            juce::uint8 b[3] = { it->bytes[0], it->bytes[1], it->bytes[2] };
            if (force >= 1 && force <= 16) b[0] = (juce::uint8) ((b[0] & 0xF0) | (force - 1));
            const int ch = b[0] & 0x0F, st = b[0] & 0xF0;
            const bool trk = it->track >= 0 && it->track < 64;
            if (st == 0x90 && it->size > 2 && b[2] > 0)
            {
                state->held[(size_t) ch].set (b[1]);
                if (trk) state->heldByTrack[(size_t) it->track][(size_t) ch].set (b[1]);
            }
            else if (st == 0x80 || st == 0x90)
            {
                state->held[(size_t) ch].reset (b[1]);
                if (trk) state->heldByTrack[(size_t) it->track][(size_t) ch].reset (b[1]);
            }
            const int offset = juce::jlimit (0, numSamples - 1, (int) ((it->tick - origin) / tps));
            outputMidi.addEvent (b, it->size, offset);
        }
    }

    void allNotesOff (int offset)
    {
        for (int c = 0; c < 16; ++c)
        {
            auto& h = state->held[(size_t) c];
            if (h.none()) continue;
            for (int n = 0; n < 128; ++n)
                if (h.test ((size_t) n)) outputMidi.addEvent (juce::MidiMessage::noteOff (c + 1, n), offset);
            outputMidi.addEvent (juce::MidiMessage::controllerEvent (c + 1, 64, 0), offset);
            h.reset();
        }
        for (auto& t : state->heldByTrack) for (auto& h : t) h.reset();
    }

    // Note Offs for one track's sounding notes (it was just muted)
    void releaseTrack (int t, int offset)
    {
        auto& tr = state->heldByTrack[(size_t) t];
        for (int c = 0; c < 16; ++c)
        {
            auto& h = tr[(size_t) c];
            if (h.none()) continue;
            for (int n = 0; n < 128; ++n)
                if (h.test ((size_t) n))
                {
                    outputMidi.addEvent (juce::MidiMessage::noteOff (c + 1, n), offset);
                    state->held[(size_t) c].reset ((size_t) n);
                }
            h.reset();
        }
    }

    MidiFilePlayerState* state = nullptr;
    bool   hostValid = false, hostIsPlaying = false;
    double hostBpm = 120.0, hostPpq = 0.0;
};
