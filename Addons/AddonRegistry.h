#pragma once
#include "AddonScanner.h"
#include "../Source/NodeProcessor.h"
#include <memory>
#include <unordered_map>

/**
 * AddonRegistry
 *
 * Singleton (owned by PatchyProcessor).
 * Loaded at startup from scan results.
 *
 * Responsibilities:
 *   - Keep DynamicLibrary handles alive so function pointers remain valid
 *   - Provide a factory: createNode(addonName) → NodeProcessor*
 *   - Tell the UI which addon names are available per nodeType
 */
class AddonRegistry
{
public:
    AddonRegistry() = default;

    /** Populate from a completed scan. Call once on startup. */
    void load (const std::vector<AddonScanner::ScanResult>& scanResults);

    /** Create a NodeProcessor that wraps the named addon.
     *  Returns nullptr if addonName is not registered. */
    std::unique_ptr<NodeProcessor> createNode (const juce::String& nodeId,
                                               const juce::String& addonName);

    /** All registered addon names, optionally filtered by nodeType (0 = all). */
    std::vector<juce::String> getAddonNames (int nodeType = 0) const;

    /** Returns true if at least one addon is registered. */
    bool hasAddons() const { return ! entries.empty(); }

    /** Full descriptor info for UI display. */
    struct Entry
    {
        juce::String                    name;
        juce::String                    vendor;
        juce::String                    version;
        int                             nodeType     = 0;
        int                             audioInputs  = 0;  // 0 = use nodeType default
        int                             audioOutputs = 0;
        int                             midiInputs   = 0;
        int                             midiOutputs  = 0;
        juce::File                      file;
        std::shared_ptr<juce::DynamicLibrary> lib;

        // Resolved function pointers
        using CreateFn  = NGA_Instance*  (*)();
        using DestroyFn = void (*)(NGA_Instance*);
        using PrepareFn = void (*)(NGA_Instance*, double, int);
        using ProcessFn = void (*)(NGA_Instance*, float**, float**, int, int,
                                   const NGA_MidiEvent*, int,
                                   NGA_MidiEvent*, int*, int);

        using GetParamCountFn = int   (*)(NGA_Instance*);
        using GetParamInfoFn  = void  (*)(NGA_Instance*, int, NGA_ParameterInfo*);
        using GetParamFn      = float (*)(NGA_Instance*, int);
        using SetParamFn          = void  (*)(NGA_Instance*, int, float);
        using GetAudioOutCountFn  = int   (*)(NGA_Instance*);

        CreateFn           create           = nullptr;
        DestroyFn          destroy          = nullptr;
        PrepareFn          prepare          = nullptr;
        ProcessFn          process          = nullptr;
        GetParamCountFn    getParamCount    = nullptr;
        GetParamInfoFn     getParamInfo     = nullptr;
        GetParamFn         getParam         = nullptr;
        SetParamFn         setParam         = nullptr;
        GetAudioOutCountFn getAudioOutCount = nullptr;
    };

    const std::vector<Entry>& getEntries() const { return entries; }

private:
    std::vector<Entry> entries;

    // Keep libs alive by name for quick lookup
    std::unordered_map<juce::String, int> nameToIndex;
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * DynamicNodeProcessor
 *
 * A NodeProcessor that delegates process() to a loaded addon instance.
 */
class DynamicNodeProcessor : public NodeProcessor
{
public:
    DynamicNodeProcessor (const juce::String&             nodeId,
                          const AddonRegistry::Entry& entry);
    ~DynamicNodeProcessor() override;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (int numSamples) override;

    const juce::String& getAddonName() const { return addonName; }
    juce::String customName;   // user-defined display name (for MidiMonitor NAME column)
    std::function<void()> onPortCountChanged;  // called when dynamic port count changes
    int audioInputCount  = 1;  // number of audio input ports
    int audioOutputCount = 1;  // number of audio output ports

    // Parameter access (called from message thread via WebBridge)
    int   getParameterCount() const;
    void  getParameterInfo  (int index, NGA_ParameterInfo& info) const;
    float getParameter      (int index) const;
    void  setParameter      (int index, float value);

    // Spectrum data access (for Spectrumyser addon)
    struct SpectrumData
    {
        int           fftSize   = 0;
        const float*  mags      = nullptr;  // points into addon memory — valid until next process()
        bool          valid     = false;
    };
    SpectrumData getSpectrumData() const
    {
        if (! lib) return {};
        using GetFFTSize = int         (*)(NGA_Instance*);
        using GetFFTMags = const float* (*)(NGA_Instance*);
        auto getSize = (GetFFTSize) lib->getFunction ("NGA_getFFTSize");
        auto getMags = (GetFFTMags) lib->getFunction ("NGA_getFFTMagnitudes");
        if (! getSize || ! getMags) return {};
        return { getSize (instance), getMags (instance), true };
    }

private:
    std::shared_ptr<juce::DynamicLibrary> lib;

    AddonRegistry::Entry::CreateFn        fnCreate        = nullptr;
    AddonRegistry::Entry::DestroyFn       fnDestroy       = nullptr;
    AddonRegistry::Entry::PrepareFn       fnPrepare       = nullptr;
    AddonRegistry::Entry::ProcessFn       fnProcess       = nullptr;
    AddonRegistry::Entry::GetParamCountFn fnGetParamCount = nullptr;
    AddonRegistry::Entry::GetParamInfoFn  fnGetParamInfo  = nullptr;
    AddonRegistry::Entry::GetParamFn      fnGetParam      = nullptr;
    AddonRegistry::Entry::SetParamFn          fnSetParam          = nullptr;
    AddonRegistry::Entry::GetAudioOutCountFn  fnGetAudioOutCount  = nullptr;

    NGA_Instance* instance   = nullptr;
    juce::String  addonName;

    static constexpr int kMaxMidiEvents = 256;
    NGA_MidiEvent midiInBuf  [kMaxMidiEvents];
    NGA_MidiEvent midiOutBuf [kMaxMidiEvents];
};
