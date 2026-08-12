#pragma once
#include <juce_core/juce_core.h>
#include "../Pax/PaxAPI.h"
#include <functional>
#include <vector>

/**
 * PaxScanner
 *
 * Finds all shared libraries in the standard platform Pax folders
 * (plus next to the binary for dev/portable use), attempts to load each
 * one, verifies it exports the required PAX symbols and has a matching
 * API version, then reports the result via a callback.
 *
 * Loading is done on the MESSAGE thread at startup — never on the audio thread.
 */
class PaxScanner
{
public:
    struct ScanResult
    {
        juce::File   file;
        bool         valid   = false;
        juce::String name;
        juce::String vendor;
        juce::String version;
        int          nodeType     = 0;
        int          audioInputs  = 0;
        int          audioOutputs = 0;
        int          midiInputs   = 0;
        int          midiOutputs  = 0;
        juce::String errorMsg;
    };

    /** Scan all standard Pax folders and return results. */
    static std::vector<ScanResult> scan();

    /** Return all folders that will be searched. */
    static std::vector<juce::File> getPaxFolders();

    /** Platform extension: ".dylib" / ".so" / ".dll" */
    static juce::String getPaxExtension();

private:
    static ScanResult tryLoad (const juce::File& file);
};
