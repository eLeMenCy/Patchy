#pragma once
#include "PaxScanner.h"
#include "../Source/NodeProcessor.h"
#include "../Source/GraphModel.h"
#include <memory>
#include <unordered_map>
#include <vector>

/**
 * PaxRegistry
 *
 * Singleton (owned by PatchyProcessor).
 * Loaded at startup from scan results.
 *
 * Responsibilities:
 *   - Keep DynamicLibrary handles alive so function pointers remain valid
 *   - Provide a factory: createNode(paxName) → NodeProcessor*
 *   - Tell the UI which Pax names are available per nodeType
 */
/** Translates a PAX_VALUETYPE_* int (as returned by a Pax's
 *  PAX_getValueInputType/OutputType) into the internal PaxValueType enum.
 *  Centralised here rather than duplicated in WebBridge.cpp and
 *  PatchyProcessor.cpp, which both need it to build a PaxPortSpec from
 *  registry data. Unrecognised/out-of-range values default to Generic —
 *  same safe-default reasoning as everywhere else in this mechanism. */
PaxValueType paxValueTypeFromTag (int tag);

class PaxRegistry
{
public:
    PaxRegistry() = default;

    /** Populate from a completed scan. Call once on startup. */
    void load (const std::vector<PaxScanner::ScanResult>& scanResults);

    /** Create a NodeProcessor that wraps a loaded Pax.
     *  Returns nullptr if paxName is not registered. */
    std::unique_ptr<NodeProcessor> createNode (const juce::String& nodeId,
                                               const juce::String& paxName);

    /** All registered Pax names, optionally filtered by nodeType (0 = all). */
    std::vector<juce::String> getPaxNames (int nodeType = 0) const;

    /** Returns true if at least one Pax is registered. */
    bool hasPax() const { return ! entries.empty(); }

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
        int                             valueInputs  = 0;  // resolved once at load() time from
        int                             valueOutputs = 0;  // PAX_getValueInputCount/OutputCount, if exported
        std::vector<int>                valueInputTypes;   // PAX_VALUETYPE_* per port, resolved from
        std::vector<int>                valueOutputTypes;  // PAX_getValueInputType/OutputType, if exported
        int                             colourCategory = -1;  // PAX_COLOURCAT_AUTO (-1) = auto-detect
        juce::File                      file;
        std::shared_ptr<juce::DynamicLibrary> lib;

        // Resolved function pointers
        using CreateFn  = PAX_Instance*  (*)();
        using DestroyFn = void (*)(PAX_Instance*);
        using PrepareFn = void (*)(PAX_Instance*, double, int);
        using ProcessFn = void (*)(PAX_Instance*, const PAX_ProcessContext*);

        using GetParamCountFn = int   (*)(PAX_Instance*);
        using GetParamInfoFn  = void  (*)(PAX_Instance*, int, PAX_ParameterInfo*);
        using GetParamFn      = float (*)(PAX_Instance*, int);
        using SetParamFn          = void  (*)(PAX_Instance*, int, float);
        using GetAudioOutCountFn  = int   (*)(PAX_Instance*);

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
 * DynamicPaxProcessor
 *
 * A NodeProcessor that delegates process() to a loaded Pax instance.
 */
class DynamicPaxProcessor : public NodeProcessor
{
public:
    DynamicPaxProcessor (const juce::String&             nodeId,
                          const PaxRegistry::Entry& entry);
    ~DynamicPaxProcessor() override;

    void prepare (double sampleRate, int maxBlockSize) override;
    void process (int numSamples) override;

    const juce::String& getPaxName() const { return paxName; }
    juce::String customName;   // user-defined display name (for MidiMonitor NAME column)
    std::function<void()> onPortCountChanged;  // called when dynamic port count changes
    int audioInputCount  = 1;  // number of audio input ports
    int audioOutputCount = 1;  // number of audio output ports

    // Parameter access (called from message thread via WebBridge)
    int   getParameterCount() const;
    void  getParameterInfo  (int index, PAX_ParameterInfo& info) const;
    float getParameter      (int index) const;
    void  setParameter      (int index, float value);

    // Spectrum data access (for Spectrumyser Pax)
    struct SpectrumData
    {
        int           fftSize   = 0;
        const float*  mags      = nullptr;  // points into Pax memory — valid until next process()
        bool          valid     = false;
    };
    SpectrumData getSpectrumData() const
    {
        if (! lib) return {};
        using GetFFTSize = int         (*)(PAX_Instance*);
        using GetFFTMags = const float* (*)(PAX_Instance*);
        auto getSize = (GetFFTSize) lib->getFunction ("PAX_getFFTSize");
        auto getMags = (GetFFTMags) lib->getFunction ("PAX_getFFTMagnitudes");
        if (! getSize || ! getMags) return {};
        return { getSize (instance), getMags (instance), true };
    }

private:
    std::shared_ptr<juce::DynamicLibrary> lib;

    PaxRegistry::Entry::CreateFn        fnCreate        = nullptr;
    PaxRegistry::Entry::DestroyFn       fnDestroy       = nullptr;
    PaxRegistry::Entry::PrepareFn       fnPrepare       = nullptr;
    PaxRegistry::Entry::ProcessFn       fnProcess       = nullptr;
    PaxRegistry::Entry::GetParamCountFn fnGetParamCount = nullptr;
    PaxRegistry::Entry::GetParamInfoFn  fnGetParamInfo  = nullptr;
    PaxRegistry::Entry::GetParamFn      fnGetParam      = nullptr;
    PaxRegistry::Entry::SetParamFn          fnSetParam          = nullptr;
    PaxRegistry::Entry::GetAudioOutCountFn  fnGetAudioOutCount  = nullptr;

    PAX_Instance* instance   = nullptr;
    juce::String  paxName;

    static constexpr int kMaxMidiEvents = 256;
    PAX_MidiEvent midiInBuf  [kMaxMidiEvents];
    PAX_MidiEvent midiOutBuf [kMaxMidiEvents];
};
