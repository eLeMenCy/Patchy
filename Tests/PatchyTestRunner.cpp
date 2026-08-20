// Patchy — Test runner entry point

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

int main (int argc, char* argv[])
{
    juce::initialiseJuce_GUI();

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    // Run all registered tests (or a specific category)
    if (argc > 1)
        runner.runTestsInCategory (argv[1]);
    else
        runner.runAllTests();

    // juce::Thread::launch() (used by MqttConnectionOwnershipTests, and by
    // the real MqttSubscribeNode/PublishNode this validates) can't safely
    // delete its own LambdaThread wrapper from within the worker thread
    // itself — a Thread's destructor joins it, and a thread can't join
    // itself. JUCE defers that cleanup elsewhere, most likely to the
    // message thread — this runner never ran any kind of message loop at
    // all before now, so if that's the mechanism, that deferred cleanup
    // never got a chance to execute: the objects were still technically
    // alive (not truly leaked, just never reaped) by the time the process
    // exited, which is exactly what JUCE's leak detector reported after a
    // test run that used Thread::launch() for the first time in this
    // project (53 threads launched across the whole run — one test alone
    // launches 50 in a loop — all with zero cleanup opportunity the entire
    // time). A plain sleep here, rather than an explicit message-pump
    // call, gives whatever background cleanup mechanism JUCE actually uses
    // real wall-clock time to run on its own, without needing to know or
    // guess the exact API for it — a first attempt at this used
    // MessageManager::runDispatchLoopUntil(), which turned out not to
    // exist on this build; this avoids relying on getting that right.
    juce::Thread::sleep (1000);

    // Report results
    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        auto* result = runner.getResult (i);
        if (result->failures > 0)
        {
            ++failures;
            DBG ("FAIL: " << result->unitTestName << " — " << result->failures << " failure(s)");
            for (auto& msg : result->messages)
                DBG ("  " << msg);
        }
        else
        {
            DBG ("PASS: " << result->unitTestName);
        }
    }

    DBG ("\n" << (failures == 0 ? "All tests passed!" : juce::String (failures) + " test(s) FAILED"));
    juce::shutdownJuce_GUI();
    return failures > 0 ? 1 : 0;
}
