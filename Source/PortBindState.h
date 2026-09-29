#pragma once
#include <juce_core/juce_core.h>
#include <atomic>

/**
 * PortBindState.h — v0.0.916 (2026-09-28)
 *
 * Tracks a listening node's failed UDP bind, so the node can show
 * "port N in use" instead of failing silently (UDP In, OSC In), and retries
 * the bind quietly until the port frees up.
 *
 * Two timings, both deliberate:
 *  - retry every 0.5 s (driven by WebBridge's 30 Hz activity timer, via the
 *    node's retryBindIfNeeded()) — so a normal handover recovers fast, e.g.
 *    a file load, where the new graph's node binds while the old graph
 *    (different node ids, so no socket transfer) still holds the port for a
 *    moment;
 *  - only REPORT "in use" after the bind has kept failing for 1 s — so that
 *    handover never flashes red, while a real conflict still shows.
 *
 * failedPort/failedSince are written on the message thread and read there
 * too (activity collection); atomics only because nothing here is worth a
 * data race.
 */
struct PortBindState
{
    static constexpr juce::uint32 kRetryMs  = 500;
    static constexpr juce::uint32 kReportMs = 1000;

    /** Returns true when this is a NEW failure (worth one log line). */
    bool failed (int port)
    {
        const auto now = juce::Time::getMillisecondCounter();
        nextRetry = now + kRetryMs;
        if (failedPort.load (std::memory_order_relaxed) == port)
            return false;
        failedPort.store (port, std::memory_order_relaxed);
        failedSince.store (now, std::memory_order_relaxed);
        return true;
    }

    void clear() { failedPort.store (0, std::memory_order_relaxed); }

    bool shouldRetry() const
    {
        return failedPort.load (std::memory_order_relaxed) != 0
            && juce::Time::getMillisecondCounter() >= nextRetry;
    }

    /** The port to show as "in use", or 0. */
    int portInUseForUi() const
    {
        const int p = failedPort.load (std::memory_order_relaxed);
        if (p == 0) return 0;
        return juce::Time::getMillisecondCounter() - failedSince.load (std::memory_order_relaxed) >= kReportMs ? p : 0;
    }

private:
    std::atomic<int>          failedPort  { 0 };
    std::atomic<juce::uint32> failedSince { 0 };
    juce::uint32              nextRetry = 0;
};
