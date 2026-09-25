#pragma once
#include <juce_core/juce_core.h>

/**
 * DiagLog.h — TEMPORARY diagnostic (2026-09-25), random-click investigation.
 *
 * Writes "[HH:MM:SS.mmm] message" to the log, so the user can note the
 * wall-clock time of an audible click and match it to whatever was logged
 * at that exact moment. Every caller is event-driven (only fires when
 * something noteworthy happens, and rate-limited where it could repeat),
 * so the occasional String allocation on an audio thread is acceptable
 * for a temporary diagnostic — same trade-off as the project's existing
 * timing logs. Remove together with its callers once resolved.
 */
inline void diagLog (const juce::String& message)
{
    const auto now = juce::Time::getCurrentTime();
    juce::Logger::writeToLog ("[" + now.formatted ("%H:%M:%S") + "."
                              + juce::String (now.getMilliseconds()).paddedLeft ('0', 3)
                              + "] " + message);
}
