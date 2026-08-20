// Patchy — WaitableEvent CPU-spin regression tests (JUCE UnitTest framework)
//
// Covers the bug fixed 2026-08-13 in DmxDeviceNodes.h (originally),
// OscDeviceNodes.h, UdpDeviceNodes.h, and ArtNetDeviceNodes.h: each
// protocol's Out-device send queue uses a juce::WaitableEvent to let its
// background send thread sleep when the queue is empty, rather than
// busy-poll. Constructed with manualReset=true and never explicitly
// reset(), the event stayed permanently signalled after the very first
// value was ever sent — every subsequent wait() on an empty queue then
// returned immediately instead of blocking, degenerating into a CPU spin
// for the rest of that node's lifetime. Fixed by using manualReset=false
// (auto-reset) instead, matching MqttPublishNode's queue, which had it
// right from the start.
//
// Two complementary tests below: one behavioural, testing the actual
// fifo+event pattern in isolation (a faithful copy, not the real
// production classes — those are private nested structs inside node
// classes with real socket/JUCE-networking dependencies, not something a
// lightweight unit test should need to pull in); one a direct textual
// check against the real production files themselves, since that's the
// only way to verify the actual fix rather than just a copy of its shape.
// The second is more fragile (a rename or reformat could make it produce
// a false result without the underlying bug changing) but has the
// advantage the first doesn't: it actually looks at the real files.

#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Faithful copy of the fifo+WaitableEvent pattern used by
// OscOutDeviceNode/UdpOutDeviceNode/ArtNetOutDeviceNode/MqttPublishNode's
// send queues (all four are structurally identical, differing only in
// what they carry) — deliberately not touching juce_core's networking
// classes or the real node headers at all, just the same
// AbstractFifo+WaitableEvent shape.
struct FakeSendQueue
{
    void push (int value)
    {
        int s1, n1, s2, n2;
        fifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) values[static_cast<size_t> (s1)] = value;
        fifo.finishedWrite (n1 + n2);
    }
    bool pop (int& value)
    {
        int s1, n1, s2, n2;
        fifo.prepareToRead (1, s1, n1, s2, n2);
        if (n1 == 0) return false;
        value = values[static_cast<size_t> (s1)];
        fifo.finishedRead (n1 + n2);
        return true;
    }
    void signalDataAvailable() { event.signal(); }
    bool waitForData (int timeoutMs) { return event.wait (timeoutMs); }

    juce::AbstractFifo    fifo { 16 };
    std::array<int, 16>   values {};
    juce::WaitableEvent   event { false };   // auto-reset — the fix itself
};

class WaitableEventCpuSpinTests : public juce::UnitTest
{
public:
    WaitableEventCpuSpinTests() : juce::UnitTest ("WaitableEventCpuSpin", "Patchy") {}

    void runTest() override
    {
        testAutoResetEventBlocksOnSubsequentEmptyWait();
        testNoOutDeviceSendQueueUsesManualResetEvent();
    }

private:
    void testAutoResetEventBlocksOnSubsequentEmptyWait()
    {
        beginTest ("Auto-reset WaitableEvent blocks on wait() after its one signal has already been consumed, rather than spinning");

        FakeSendQueue queue;

        // Mirrors "the first value is ever sent" — one push, one signal,
        // one successful pop+wait, matching a real send thread's first
        // iteration.
        queue.push (42);
        queue.signalDataAvailable();

        int v = 0;
        bool gotValue = queue.pop (v);
        expect (gotValue, "should have popped the one pushed value");
        expectEquals (v, 42);

        auto waited = queue.waitForData (50);   // should return quickly (already signalled)
        expect (waited, "the initial wait should still succeed — it's consuming the signal from push above");

        // This is the actual bug: with the old manualReset=true and no
        // reset(), the event would still read as signalled here even
        // though nothing new was pushed — wait() would return instantly,
        // over and over, forever, rather than genuinely blocking. Time
        // the call: with the fix, it should take close to the full
        // timeout, not return in a handful of milliseconds.
        // Uses std::chrono rather than a JUCE-specific timing API — plain
        // standard C++, so there's no risk of a specific JUCE method name
        // being wrong here (the earlier PatchyTestRunner.cpp mistake in
        // this same session was exactly that kind of error).
        auto before = std::chrono::steady_clock::now();
        auto timedOut = ! queue.waitForData (200);
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>
                          (std::chrono::steady_clock::now() - before).count();

        expect (timedOut, "wait() on a genuinely empty, unsignalled queue should time out, not return true");
        expect (elapsedMs > 100,
                "wait() should have actually blocked for close to its 200ms timeout (took "
                + juce::String (elapsedMs) + "ms) — a near-zero time here is exactly the CPU-spin bug: "
                "the event staying permanently signalled after being triggered once");
    }

    void testNoOutDeviceSendQueueUsesManualResetEvent()
    {
        beginTest ("None of the protocol Out-device send queues use a manualReset(true) WaitableEvent");

        // Direct check against the real files, not the FakeSendQueue copy
        // above — deliberately fragile to a rename/reformat, but it's the
        // only one of these two tests that actually looks at the real fix
        // rather than a faithful copy of its shape.
        // Deliberately built from only getParentDirectory()/getChildFile()
        // — both have real precedent elsewhere in this codebase (unlike
        // getSiblingFile(), which doesn't), preferring the more certain
        // choice given how easy it is to get a specific JUCE API detail
        // wrong without being able to compile-check it here.
        juce::File thisFile (__FILE__);
        juce::File sourceDir = thisFile.getParentDirectory().getParentDirectory().getChildFile ("Source");

        const char* filesToCheck[] = {
            "DmxDeviceNodes.h", "OscDeviceNodes.h", "UdpDeviceNodes.h",
            "ArtNetDeviceNodes.h", "MqttDeviceNodes.h"
        };

        for (auto* filename : filesToCheck)
        {
            auto file = sourceDir.getChildFile (filename);
            expect (file.existsAsFile(), juce::String ("expected to find ") + filename
                    + " at " + file.getFullPathName());
            if (! file.existsAsFile()) continue;

            auto contents = file.loadFileAsString();
            bool hasManualResetTrue = contents.contains ("event { true }");

            expect (! hasManualResetTrue,
                    juce::String (filename) + " appears to declare a WaitableEvent with manualReset=true "
                    "(\"event { true }\") — this is the exact CPU-spin bug fixed 2026-08-13. If this is a "
                    "genuine new WaitableEvent unrelated to a send queue, this check is a false positive and "
                    "needs updating to be more specific; if it's a real regression, it needs the same fix "
                    "DmxDeviceNodes.h/OscDeviceNodes.h/UdpDeviceNodes.h/ArtNetDeviceNodes.h/MqttDeviceNodes.h "
                    "already have (manualReset=false, i.e. \"event { false }\").");
        }
    }
};

// Register the test suite
static WaitableEventCpuSpinTests waitableEventCpuSpinTests;
