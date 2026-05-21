#pragma once
#include <juce_core/juce_core.h>
#include "../Addons/AddonAPI.h"
#include <functional>
#include <vector>

/**
 * AddonScanner
 *
 * Finds all shared libraries in the standard platform addon folders
 * (plus next to the binary for dev/portable use), attempts to load each
 * one, verifies it exports the required NGA symbols and has a matching
 * API version, then reports the result via a callback.
 *
 * Loading is done on the MESSAGE thread at startup — never on the audio thread.
 */
class AddonScanner
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

    /** Scan all standard addon folders and return results. */
    static std::vector<ScanResult> scan();

    /** Return all folders that will be searched. */
    static std::vector<juce::File> getAddonFolders();

    /** Platform extension: ".dylib" / ".so" / ".dll" */
    static juce::String getAddonExtension();

private:
    static ScanResult tryLoad (const juce::File& file);
};
