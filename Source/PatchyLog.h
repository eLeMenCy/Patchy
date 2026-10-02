#pragma once
#include <juce_core/juce_core.h>

/**
 * PatchyLog.h — v0.0.919 (2026-09-29)
 *
 * Every juce::Logger line also goes to a file, so the log survives in hosts
 * with no console (Logic Pro runs AUs in its own helper process; stderr is
 * lost there). Console output (std::cerr, what CLion shows) is unchanged.
 *
 * File: <user Library>/Logs/Patchy/Patchy.log (FileLogger's system log
 * folder). If the host sandboxes the plugin, macOS silently redirects that
 * into the sandbox container (~/Library/Containers/<id>/Data/Library/Logs/
 * Patchy/) — so WHERE the file lands tells whether Patchy is sandboxed; the
 * session header also logs the home folder the process sees. Rolled over to
 * Patchy.old.log when it passes 2 MB at startup.
 *
 * Audio-thread safe(ish): writeToLog() is called from the audio thread too
 * (fifo STARVED/TRIM lines, swap timings). logMessage() never touches the
 * disk: it appends to an in-memory queue under a short lock; a background
 * thread writes the queue out every 200 ms. File I/O on the audio thread
 * could itself cause the clicks the diagnostics are hunting.
 *
 * Ref-counted and process-wide: several Patchy instances in one DAW share
 * the logger; it's installed by the first Scope and removed by the last.
 * PatchyProcessor derives from Scope FIRST, so the log is up before any of
 * its members can log and outlives all of them.
 */
namespace PatchyLog
{
    struct Scope
    {
        Scope();
        ~Scope();
        Scope (const Scope&) = delete;
        Scope& operator= (const Scope&) = delete;
    };

    /** The log file currently in use (empty if none). */
    juce::File getLogFile();
}
