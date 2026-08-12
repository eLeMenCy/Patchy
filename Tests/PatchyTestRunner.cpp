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
