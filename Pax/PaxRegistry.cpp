#include "PaxRegistry.h"
#include <algorithm>

PaxValueType paxValueTypeFromTag (int tag)
{
    switch (tag)
    {
        case PAX_VALUETYPE_MQTT:   return PaxValueType::Mqtt;
        case PAX_VALUETYPE_OSC:    return PaxValueType::Osc;
        case PAX_VALUETYPE_DMX:    return PaxValueType::Dmx;
        case PAX_VALUETYPE_UDP:    return PaxValueType::Udp;
        case PAX_VALUETYPE_ARTNET: return PaxValueType::ArtNet;
        case PAX_VALUETYPE_MIDI:   return PaxValueType::Midi;
        default:                   return PaxValueType::Generic;
    }
}

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

        // Value port counts — separate optional exports, not descriptor fields
        // (avoids an ABI-breaking struct change for already-compiled Pax
        // binaries; see PaxAPI.h's PAX_Descriptor nodeType=4 comment).
        // Static, no instance needed — called once here at scan time.
        if (auto* getValIn = (int(*)()) lib->getFunction ("PAX_getValueInputCount"))
            e.valueInputs = getValIn();
        if (auto* getValOut = (int(*)()) lib->getFunction ("PAX_getValueOutputCount"))
            e.valueOutputs = getValOut();

        // Per-port value types — one more optional export beyond the counts
        // above, indexed by port position. Not exporting this (or an older
        // Pax that only has the count functions) means every value port
        // defaults to PAX_VALUETYPE_GENERIC (0), same as before per-port
        // typing existed.
        //
        // Always populate with exactly one entry per declared port, even
        // when the optional export isn't present at all — a Pax that
        // deliberately relies on the generic default (like MqttToValuePax's
        // output) must still get e.g. valueOutputTypes = [0], not an empty
        // array. An empty array here was a real bug: anything reconstructing
        // ports from this array (rather than from a live process() context,
        // which has its own separate in-code default) would see zero
        // output ports at all for such a Pax, not one generic one —
        // found via the sidebar showing MqttToValuePax as MQTT's own
        // colour instead of Converter/fuchsia, because with an empty
        // valueOutputTypes it looked like a pure-MQTT-source with no
        // output side to compare against at all.
        auto* getValInType  = (int(*)(int)) lib->getFunction ("PAX_getValueInputType");
        for (int i = 0; i < e.valueInputs; ++i)
            e.valueInputTypes.push_back (getValInType ? getValInType (i) : PAX_VALUETYPE_GENERIC);
        auto* getValOutType = (int(*)(int)) lib->getFunction ("PAX_getValueOutputType");
        for (int i = 0; i < e.valueOutputs; ++i)
            e.valueOutputTypes.push_back (getValOutType ? getValOutType (i) : PAX_VALUETYPE_GENERIC);

        // Colour category override — see PaxAPI.h's PAX_getColourCategory
        // doc for the full rule (only consulted for the one case the
        // auto-detection genuinely can't resolve: a pure source/sink Pax).
        if (auto* getColourCat = (int(*)()) lib->getFunction ("PAX_getColourCategory"))
            e.colourCategory = getColourCat();

        e.create  = (Entry::CreateFn)  lib->getFunction ("PAX_create");
        e.destroy = (Entry::DestroyFn) lib->getFunction ("PAX_destroy");
        e.prepare       = (Entry::PrepareFn)       lib->getFunction ("PAX_prepare");
        e.process       = (Entry::ProcessFn)       lib->getFunction ("PAX_process");
        e.getParamCount = (Entry::GetParamCountFn) lib->getFunction ("PAX_getParameterCount");
        e.getParamInfo  = (Entry::GetParamInfoFn)  lib->getFunction ("PAX_getParameterInfo");
        e.getParam      = (Entry::GetParamFn)      lib->getFunction ("PAX_getParameter");
        e.setParam          = (Entry::SetParamFn)          lib->getFunction ("PAX_setParameter");
        e.getAudioOutCount  = (Entry::GetAudioOutCountFn) lib->getFunction ("PAX_getAudioOutputCount");
        e.isParamReadOnly   = (Entry::IsParamReadOnlyFn)  lib->getFunction ("PAX_isParameterReadOnly");

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
      fnIsParamReadOnly   (e.isParamReadOnly),
      paxName       (e.name)
{
    jassert (fnCreate != nullptr);
    instance = fnCreate();
    jassert (instance != nullptr);

    // Set effective audio port counts
    int paxType = e.nodeType;
    audioInputCount  = e.audioInputs  > 0 ? e.audioInputs  : (paxType == 2 || paxType == 3 ? 1 : 0);
    audioOutputCount = e.audioOutputs > 0 ? e.audioOutputs : (paxType == 2 || paxType == 3 ? 1 : 0);

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
    int outCount      = 0;
    int valueOutCount = 0;
    PAX_ProcessContext ctx {};
    ctx.audioIn        = audioInPtrs;
    ctx.audioOut       = audioOutPtrs;
    ctx.numChannels    = 2;
    ctx.numSamples     = numSamples;
    ctx.midiIn         = midiInBuf;
    ctx.midiInCount    = inCount;
    ctx.midiOut        = midiOutBuf;
    ctx.midiOutCount   = &outCount;
    ctx.midiMaxCount   = kMaxMidiEvents;
    // Value buffers — now live
    // Reset before each block so an unset PAX_Value::portIndex (or any
    // other field a Pax doesn't explicitly write) always reads as a safe
    // zero, never a stale value left over from a previous block's write
    // to that same array slot. Matches the guarantee documented in
    // PaxAPI.h's PAX_Value comment.
    outputValues.fill (PAX_Value {});
    ctx.valuesIn       = inputValues.data();
    ctx.valueInCount   = inputValueCount;
    ctx.valuesOut      = outputValues.data();
    ctx.valueOutCount  = &valueOutCount;
    ctx.valueMaxCount  = kMaxValueEvents;

    // DMX universe (API v4) — separate wide-payload path from Values above.
    // Zero the output frame and clear the valid flag before each call, same
    // reasoning as outputValues.fill() just above: a Pax that only writes
    // one or two channels shouldn't need to zero the other 510 itself, and
    // an unset flag must never read as "holds real data" left over from a
    // previous block.
    outputDmxFrame.fill (0);
    outputDmxFrameValid = false;
    ctx.dmxFrameIn        = inputDmxFrameValid ? inputDmxFrame.data() : nullptr;
    ctx.dmxFrameInValid   = inputDmxFrameValid;
    ctx.dmxFrameOut       = outputDmxFrame.data();
    ctx.dmxFrameOutValid  = &outputDmxFrameValid;

    fnProcess (instance, &ctx);
    outputValueCount = valueOutCount;

    // Value events also count as activity for flash purposes — this was
    // missing until now, which meant a Pax that only ever writes value
    // events (e.g. MqttToValuePax) never registered any activity at all,
    // not even with the wrong colour. recordMidiActivity() is already the
    // de facto generic "this node had discrete-event activity" signal in
    // practice — every non-MIDI protocol node (DMX/ArtNet/OSC/UDP/MQTT)
    // already calls this exact same counter for its own activity, despite
    // none of them being MIDI either, so this is consistent with existing
    // precedent, not a special case.
    if (valueOutCount > 0)
        recordMidiActivity (valueOutCount);

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

bool DynamicPaxProcessor::isParameterReadOnly (int index) const
{
    return (fnIsParamReadOnly && instance) ? (fnIsParamReadOnly (instance, index) != 0) : false;
}
