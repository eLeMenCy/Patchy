#include <cstring>
#include <atomic>
#include "PaxScanner.h"

// ─────────────────────────────────────────────────────────────────────────────

juce::String PaxScanner::getPaxExtension()
{
#if JUCE_MAC
    return ".dylib";
#elif JUCE_WINDOWS
    return ".dll";
#else
    return ".so";
#endif
}

// ─────────────────────────────────────────────────────────────────────────────

std::vector<juce::File> PaxScanner::getPaxFolders()
{
    std::vector<juce::File> folders;

#if JUCE_MAC
    folders.push_back (
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("Patchy/Pax"));
#elif JUCE_WINDOWS
    folders.push_back (
        juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("Patchy/Pax"));
#else // Linux
    folders.push_back (
        juce::File::getSpecialLocation (juce::File::userHomeDirectory)
            .getChildFile (".patchy/pax"));
#endif

    // Also scan next to the binary — convenient for development
    folders.push_back (
        juce::File::getSpecialLocation (juce::File::currentExecutableFile)
            .getParentDirectory()
            .getChildFile ("PatchyPax"));

    return folders;
}

// ─────────────────────────────────────────────────────────────────────────────

std::vector<PaxScanner::ScanResult> PaxScanner::scan()
{
    std::vector<ScanResult> results;
    const auto ext = getPaxExtension();

    for (const auto& folder : getPaxFolders())
    {
        if (! folder.isDirectory())
            continue;

        juce::Logger::writeToLog ("PaxScanner: scanning " + folder.getFullPathName());

        for (const auto& file : folder.findChildFiles (
                 juce::File::findFiles, false, "*" + ext))
        {
            auto result = tryLoad (file);
            juce::Logger::writeToLog (
                juce::String ("  ") + file.getFileName() + " → "
                + (result.valid ? ("OK: " + result.name) : ("FAIL: " + result.errorMsg)));
            results.push_back (std::move (result));
        }
    }

    return results;
}

// ─────────────────────────────────────────────────────────────────────────────

PaxScanner::ScanResult PaxScanner::tryLoad (const juce::File& file)
{
    ScanResult result;
    result.file       = file;
    result.loadedFrom = makeShadowCopy (file);   // v0.0.959

    auto lib = std::make_unique<juce::DynamicLibrary>();
    if (! lib->open (result.loadedFrom.getFullPathName()))
    {
        result.errorMsg = "Could not open library";
        lib.reset();
        releaseShadowCopy (result.loadedFrom, file);
        return result;
    }
    // a failed check below drops the copy too
    struct DropCopy { std::unique_ptr<juce::DynamicLibrary>& l; ScanResult& r;
                      ~DropCopy() { if (! r.valid) { l.reset(); PaxScanner::releaseShadowCopy (r.loadedFrom, r.file); } } }
        dropCopy { lib, result };

    // Check all required symbols are present
    using GetDescFn = const PAX_Descriptor* (*)();
    using CreateFn  = PAX_Instance*          (*)();
    using DestroyFn = void (*)(PAX_Instance*);
    using PrepareFn = void (*)(PAX_Instance*, double, int);
    using ProcessFn = void (*)(PAX_Instance*, float**, float**, int, int,
                               const PAX_MidiEvent*, int,
                               PAX_MidiEvent*, int*, int);

    auto getDesc = (GetDescFn)  lib->getFunction ("PAX_getDescriptor");
    auto create  = (CreateFn)   lib->getFunction ("PAX_create");
    auto destroy = (DestroyFn)  lib->getFunction ("PAX_destroy");
    auto prepare = (PrepareFn)  lib->getFunction ("PAX_prepare");
    auto process = (ProcessFn)  lib->getFunction ("PAX_process");

    if (! getDesc || ! create || ! destroy || ! prepare || ! process)
    {
        result.errorMsg = "Missing required PAX exports";
        return result;
    }

    const PAX_Descriptor* desc = getDesc();
    if (desc == nullptr)
    {
        result.errorMsg = "PAX_getDescriptor returned NULL";
        return result;
    }

    if (desc->apiVersion != PAX_API_VERSION)
    {
        result.errorMsg = "API version mismatch (pax="
                          + juce::String (desc->apiVersion)
                          + " host=" + juce::String (PAX_API_VERSION) + ")";
        return result;
    }

    if (desc->nodeType < 1 || desc->nodeType > 4)
    {
        result.errorMsg = "Invalid nodeType " + juce::String (desc->nodeType);
        return result;
    }

    result.valid    = true;
    result.name         = desc->name    ? desc->name    : "(unnamed)";
    result.vendor       = desc->vendor  ? desc->vendor  : "";
    result.version      = desc->version ? desc->version : "";
    result.nodeType     = desc->nodeType;
    result.audioInputs  = desc->audioInputs;
    result.audioOutputs = desc->audioOutputs;
    result.midiInputs   = desc->midiInputs;
    result.midiOutputs  = desc->midiOutputs;

    // lib goes out of scope here — we re-open it when creating instances
    // (DynamicLibrary is cheap to reopen; the OS keeps the .so in memory)
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  v0.0.959 — private copies (hot-reload)
// ─────────────────────────────────────────────────────────────────────────────

static juce::File shadowRoot()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("PatchyPax");
}

static juce::File sessionShadowDir()
{
    static const juce::String session = juce::Uuid().toString().substring (0, 12);
    return shadowRoot().getChildFile (session);
}

juce::File PaxScanner::makeShadowCopy (const juce::File& original)
{
    static std::atomic<int> counter { 0 };
    auto dir = sessionShadowDir();
    if (! dir.createDirectory()) return original;
    auto copy = dir.getChildFile (original.getFileNameWithoutExtension()
                                  + "-" + juce::String (++counter) + original.getFileExtension());
    if (! original.copyFileTo (copy)) return original;
    return copy;
}

void PaxScanner::releaseShadowCopy (const juce::File& copy, const juce::File& original)
{
#if ! JUCE_WINDOWS
    if (copy != original && copy.isAChildOf (shadowRoot()))
        copy.deleteFile();
#else
    juce::ignoreUnused (copy, original);
#endif
}

void PaxScanner::cleanStaleShadowCopies()
{
    const auto root = shadowRoot();
    if (! root.isDirectory()) return;
    const auto mine = sessionShadowDir();
    const auto cutoff = juce::Time::getCurrentTime() - juce::RelativeTime::minutes (1);
    for (const auto& d : root.findChildFiles (juce::File::findDirectories, false))
    {
        if (d == mine || d.getLastModificationTime() > cutoff) continue;   // another Patchy may be loading right now
        for (const auto& f : d.findChildFiles (juce::File::findFiles, false))
            f.deleteFile();          // a .dll still loaded by a running Patchy stays (locked)
        d.deleteFile();              // only goes once empty
    }
}
