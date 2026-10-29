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
        juce::File   loadedFrom;   // v0.0.959 — the private copy actually opened (see makeShadowCopy)
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

    /** v0.0.958 — check one file (the autoscan uses it for new files). */
    static ScanResult tryLoad (const juce::File& file);

    /** v0.0.959 — hot-reload: every Pax is opened from a private copy in
     *  <temp>/PatchyPax/<session>/, never from the Pax folder itself, so a
     *  rebuilt Pax is really new code (macOS / Linux hand back the library
     *  already loaded for the same path) and the original can be overwritten
     *  at any time (Windows locks a loaded .dll; macOS can crash when a mapped
     *  .dylib is written over). Returns the original if the copy fails. */
    static juce::File makeShadowCopy (const juce::File& original);
    /** The copy is no longer needed once the library is open (macOS / Linux
     *  keep it mapped); Windows can't delete a loaded .dll — left for the
     *  next session's cleanup. */
    static void releaseShadowCopy (const juce::File& copy, const juce::File& original);
    /** Startup: removes copies left by earlier sessions (not touched for a minute). */
    static void cleanStaleShadowCopies();
};
