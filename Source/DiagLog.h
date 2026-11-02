#pragma once
#include <juce_core/juce_core.h>

/**
 * DiagLog.h — timestamped log lines: "[HH:MM:SS.mmm] message", so a moment
 * the user noticed can be matched to the log. Born as the 2026-09-25 click
 * diagnostic (removed v0.0.962 — the STARVED / TRIM / CLICK / XRUN lines and
 * the callback-gap timer); kept for the lifecycle lines that use it (host
 * idle driver, panic, Pax folder, virtual ports, track name…).
 */
inline void diagLog (const juce::String& message)
{
    const auto now = juce::Time::getCurrentTime();
    juce::Logger::writeToLog ("[" + now.formatted ("%H:%M:%S") + "."
                              + juce::String (now.getMilliseconds()).paddedLeft ('0', 3)
                              + "] " + message);
}
