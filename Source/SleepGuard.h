#pragma once

/**
 * SleepGuard.h — v0.0.915 (2026-09-27)
 *
 * Keeps the Mac from IDLE-sleeping while Patchy has at least one hardware
 * audio device open, the same way DAWs and QuickTime do during playback.
 *
 * Why: 2026-09-26 at the studio, the Mac idle-slept with Patchy (Standalone)
 * running. After wake the Behringer FCA1616 came back in a bad clock state:
 * its startup pop (constant jump 0.807, same signature as a fresh open)
 * repeated ~3x per second, and its AudioIn fifo starved every ~5.8 s from
 * clock drift. Patchy keeps the same device handle across sleep and never
 * reopens it. Preventing idle sleep while devices are open avoids the
 * situation entirely; full close-on-sleep / reopen-on-wake handling is
 * parked unless it happens again (see SessionLog 2026-09-27).
 *
 * Scope: only IDLE system sleep is blocked. The display can still sleep,
 * and closing the lid or choosing Apple menu > Sleep still sleeps.
 * Check while a device is open: `pmset -g assertions` lists
 * "Patchy (audio device open)" under PreventUserIdleSystemSleep.
 *
 * Ref-counted: AudioIn/AudioOutDeviceNode call acquire() on a successful
 * hardware open and release() in closeDevice(); a device transferred to a
 * rebuilt graph's node carries its hold with it (no release/acquire). The
 * assertion exists while the count is > 0. Message thread only in practice;
 * guarded by a mutex anyway. No-op on non-mac platforms.
 */
namespace SleepGuard
{
    void acquire();
    void release();
}
