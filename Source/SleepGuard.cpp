// SleepGuard.cpp — v0.0.915 (2026-09-27). See SleepGuard.h.
// Kept in its own translation unit so the IOKit headers never meet the
// rest of Patchy's JUCE-heavy code.

#include "SleepGuard.h"
#include <juce_core/juce_core.h>
#include <mutex>

#if JUCE_MAC
 #include <IOKit/pwr_mgt/IOPMLib.h>
#endif

namespace SleepGuard
{
namespace
{
    std::mutex lock;
    int        holdCount = 0;
   #if JUCE_MAC
    IOPMAssertionID assertionId = kIOPMNullAssertionID;
   #endif
}

void acquire()
{
    std::lock_guard<std::mutex> g (lock);
    if (holdCount++ > 0)
        return;

   #if JUCE_MAC
    const IOReturn r = IOPMAssertionCreateWithName (kIOPMAssertionTypePreventUserIdleSystemSleep,
                                                    kIOPMAssertionLevelOn,
                                                    CFSTR ("Patchy (audio device open)"),
                                                    &assertionId);
    if (r == kIOReturnSuccess)
        juce::Logger::writeToLog ("SleepGuard: idle sleep blocked (audio device open)");
    else
    {
        assertionId = kIOPMNullAssertionID;
        juce::Logger::writeToLog ("SleepGuard: could not create power assertion (IOReturn "
                                  + juce::String ((int) r) + ")");
    }
   #endif
}

void release()
{
    std::lock_guard<std::mutex> g (lock);
    if (holdCount <= 0)
    {
        jassertfalse;   // unbalanced release
        holdCount = 0;
        return;
    }
    if (--holdCount > 0)
        return;

   #if JUCE_MAC
    if (assertionId != kIOPMNullAssertionID)
    {
        IOPMAssertionRelease (assertionId);
        assertionId = kIOPMNullAssertionID;
        juce::Logger::writeToLog ("SleepGuard: idle sleep allowed again (no audio device open)");
    }
   #endif
}
} // namespace SleepGuard
