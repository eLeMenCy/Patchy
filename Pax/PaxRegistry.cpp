#include "PaxRegistry.h"
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  PaxRegistry
// ─────────────────────────────────────────────────────────────────────────────

void PaxRegistry::load (const std::vector<PaxScanner::ScanResult>& results)
{
    entries.clear();
    nameToIndex.clear();

    for (const auto& r : results)
    {
        if (! r.valid) continue;

        // Open the library and resolve all function pointers
        auto lib = std::make_shared<juce::DynamicLibrary>();
        if (! lib->open (r.file.getFullPathName()))
        {
            juce::Logger::writeToLog ("Registry: failed to reopen " + r.file.getFileName());
            continue;
        }

        Entry e;
        e.name     = r.name;
        e.vendor   = r.vendor;
        e.version  = r.version;
        e.nodeType = r.nodeType;
        e.file     = r.file;
        e.lib      = lib;

        // Read optional port counts from descriptor
        if (auto* getDesc = (const PAX_Descriptor*(*)()) lib->getFunction ("PAX_getDescriptor"))
        {
            if (auto* desc = getDesc())
            {
                e.audioInputs  = desc->audioInputs;
                e.audioOutputs = desc->audioOutputs;
                e.midiInputs   = desc->midiInputs;
                e.midiOutputs  = desc->midiOutputs;
            }
        }

        e.create  = (Entry::CreateFn)  lib->getFunction ("PAX_create");
        e.destroy = (Entry::DestroyFn) lib->getFunction ("PAX_destroy");
        e.prepare       = (Entry::PrepareFn)       lib->getFunction ("PAX_prepare");
        e.process       = (Entry::ProcessFn)       lib->getFunction ("PAX_process");
        e.getParamCount = (Entry::GetParamCountFn) lib->getFunction ("PAX_getParameterCount");
        e.getParamInfo  = (Entry::GetParamInfoFn)  lib->getFunction ("PAX_getParameterInfo");
        e.getParam      = (Entry::GetParamFn)      lib->getFunction ("PAX_getParameter");
        e.setParam          = (Entry::SetParamFn)          lib->getFunction ("PAX_setParameter");
        e.getAudioOutCount  = (Entry::GetAudioOutCountFn) lib->getFunction ("PAX_getAudioOutputCount");

        if (! e.create || ! e.destroy || ! e.prepare || ! e.process)
        {
            juce::Logger::writeToLog ("Registry: symbol missing in " + r.file.getFileName());
            continue;
        }

        nameToIndex[e.name] = static_cast<int>(entries.size());
        entries.push_back (std::move (e));
        juce::Logger::writeToLog ("Registry: loaded Pax '" + r.name + "'");
    }
}

std::unique_ptr<NodeProcessor>
PaxRegistry::createNode (const juce::String& nodeId,
                                 const juce::String& paxName)
{
    auto it = nameToIndex.find (paxName);
    if (it == nameToIndex.end())
    {
        juce::Logger::writeToLog ("Registry: unknown Pax '" + paxName + "'");
        return nullptr;
    }
    return std::make_unique<DynamicPaxProcessor> (nodeId, entries[static_cast<size_t>(it->second)]);
}

std::vector<juce::String>
PaxRegistry::getPaxNames (int nodeType) const
{
    std::vector<juce::String> names;
    for (const auto& e : entries)
        if (nodeType == 0 || e.nodeType == nodeType)
            names.push_back (e.name);
    return names;
}

// ─────────────────────────────────────────────────────────────────────────────
//  DynamicPaxProcessor
// ─────────────────────────────────────────────────────────────────────────────

DynamicPaxProcessor::DynamicPaxProcessor (const juce::String&             nodeId,
                                             const PaxRegistry::Entry& e)
    : NodeProcessor (nodeId, static_cast<NodeProcessor::Type> (e.nodeType)),
      lib             (e.lib),
      fnCreate        (e.create),
      fnDestroy       (e.destroy),
      fnPrepare       (e.prepare),
      fnProcess       (e.process),
      fnGetParamCount (e.getParamCount),
      fnGetParamInfo  (e.getParamInfo),
      fnGetParam      (e.getParam),
      fnSetParam          (e.setParam),
      fnGetAudioOutCount  (e.getAudioOutCount),
      paxName       (e.name)
{
    jassert (fnCreate != nullptr);
    instance = fnCreate();
    jassert (instance != nullptr);

    // Set effective audio port counts
    int ngaType = e.nodeType;
    audioInputCount  = e.audioInputs  > 0 ? e.audioInputs  : (ngaType == 2 || ngaType == 3 ? 1 : 0);
    audioOutputCount = e.audioOutputs > 0 ? e.audioOutputs : (ngaType == 2 || ngaType == 3 ? 1 : 0);

    // Allocate per-port audio buffers (size set properly in prepare())
    allocatePortBuffers (audioInputCount, audioOutputCount, 512);
}

DynamicPaxProcessor::~DynamicPaxProcessor()
{
    if (instance != nullptr && fnDestroy != nullptr)
    {
        fnDestroy (instance);
        instance = nullptr;
    }
    // lib shared_ptr releases here — .dylib unloaded when last processor is gone
}

void DynamicPaxProcessor::prepare (double sampleRate, int maxBlockSize)
{
    NodeProcessor::prepare (sampleRate, maxBlockSize);
    allocatePortBuffers (audioInputCount, audioOutputCount, maxBlockSize);
    if (instance != nullptr && fnPrepare != nullptr)
        fnPrepare (instance, sampleRate, maxBlockSize);
}

void DynamicPaxProcessor::process (int numSamples)
{
    if (instance == nullptr) return;

    // ── Convert juce::MidiBuffer → PAX_MidiEvent array ───────────────────
    int inCount = 0;
    for (const auto& meta : inputMidi)
    {
        if (inCount >= kMaxMidiEvents) break;
        const auto& msg = meta.getMessage();
        auto& ev = midiInBuf[inCount++];
        ev.sampleOffset = meta.samplePosition;
        ev.byteCount    = (uint8_t) std::min (msg.getRawDataSize(), 3);
        std::memcpy (ev.bytes, msg.getRawData(), ev.byteCount);
    }

    // ── Set up audio channel pointers (per-port, 2ch each) ───────────────
    const int maxPorts = 8;
    float* audioInPtrs [maxPorts * 2] = {};
    float* audioOutPtrs[maxPorts * 2] = {};

    if (nodeType != NodeProcessor::Type::Midi)
    {
        // Use per-port buffers if available, otherwise fall back to single buffer
        if (!inputAudioBuffers.empty())
        {
            for (int p = 0; p < static_cast<int>(inputAudioBuffers.size()) && p < maxPorts; ++p)
                for (int ch = 0; ch < 2; ++ch)
                    audioInPtrs[p * 2 + ch] = inputAudioBuffers[static_cast<size_t>(p)].getWritePointer (ch);
        }
        else if (audioInputCount > 0)
        {
            for (int ch = 0; ch < std::min(2, static_cast<int>(inputAudio.getNumChannels())); ++ch)
                audioInPtrs[ch] = inputAudio.getWritePointer (ch);
        }

        if (!outputAudioBuffers.empty())
        {
            for (int p = 0; p < static_cast<int>(outputAudioBuffers.size()) && p < maxPorts; ++p)
                for (int ch = 0; ch < 2; ++ch)
                    audioOutPtrs[p * 2 + ch] = outputAudioBuffers[static_cast<size_t>(p)].getWritePointer (ch);
        }
        else if (audioOutputCount > 0)
        {
            for (int ch = 0; ch < std::min(2, static_cast<int>(outputAudio.getNumChannels())); ++ch)
                audioOutPtrs[ch] = outputAudio.getWritePointer (ch);
        }
    }

    // ── Call into the Pax ──────────────────────────────────────────────
    int outCount = 0;
    PAX_ProcessContext ctx {};
    ctx.audioIn       = audioInPtrs;
    ctx.audioOut      = audioOutPtrs;
    ctx.numChannels   = 2;
    ctx.numSamples    = numSamples;
    ctx.midiIn        = midiInBuf;
    ctx.midiInCount   = inCount;
    ctx.midiOut       = midiOutBuf;
    ctx.midiOutCount  = &outCount;
    ctx.midiMaxCount  = kMaxMidiEvents;
    // Value fields — NULL/0 until value ports are implemented
    ctx.valuesIn      = nullptr;
    ctx.valueInCount  = 0;
    ctx.valuesOut     = nullptr;
    ctx.valueOutCount = nullptr;
    ctx.valueMaxCount = 0;

    fnProcess (instance, &ctx);

    // ── Convert PAX_MidiEvent array → juce::MidiBuffer ───────────────────
    outputMidi.clear();
    for (int i = 0; i < outCount; ++i)
    {
        const auto& ev = midiOutBuf[i];

        // Guard: skip empty or obviously invalid events
        if (ev.byteCount == 0 || ev.bytes[0] == 0)
            continue;

        const int clampedLen = std::max (1, std::min ((int) ev.byteCount, 3));

        // Use createRaw so JUCE builds the message from the exact bytes
        // without the 3-byte constructor asserting on 1/2-byte messages
        outputMidi.addEvent (
            juce::MidiMessage (ev.bytes, clampedLen, 0.0),
            ev.sampleOffset);
    }
    if (outCount > 0)
        recordMidiActivity (outCount);

}

int DynamicPaxProcessor::getParameterCount() const
{
    return (fnGetParamCount && instance) ? fnGetParamCount (instance) : 0;
}

void DynamicPaxProcessor::getParameterInfo (int index, PAX_ParameterInfo& info) const
{
    if (fnGetParamInfo && instance)
        fnGetParamInfo (instance, index, &info);
}

float DynamicPaxProcessor::getParameter (int index) const
{
    return (fnGetParam && instance) ? fnGetParam (instance, index) : 0.f;
}

void DynamicPaxProcessor::setParameter (int index, float value)
{
    if (fnSetParam && instance)
        fnSetParam (instance, index, value);

    // Check if audio output count changed (e.g. band count in Spectrumyser)
    if (fnGetAudioOutCount && instance)
    {
        int newCount = fnGetAudioOutCount (instance);
        if (newCount > 0 && newCount != audioOutputCount)
        {
            audioOutputCount = newCount;
            if (onPortCountChanged) onPortCountChanged();
        }
    }
}
