#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "GraphModel.h"
#include "ProcessingGraph.h"
#include "MidiDeviceNodes.h"
#include "MidiMonitorNode.h"
#include "AudioMonitorNode.h"
#include "MidiKeyboardNode.h"
#include <unordered_map>
#include "AudioDeviceNodes.h"
#include "WebBridge.h"
#include "../Addons/AddonRegistry.h"
#include "../Addons/AddonScanner.h"

/**
 * PatchyProcessor
 *
 * Owns the GraphModel (UI data) and ProcessingGraph (audio thread).
 * When the graph changes the UI calls rebuildProcessingGraph() which
 * snapshots the model and atomically swaps in a new ProcessingGraph.
 *
 * Thread safety:
 *   GraphModel   — message thread only
 *   ProcessingGraph — rebuilt on message thread, processed on audio thread.
 *                     A simple flag + swap ensures no concurrent access.
 */
class PatchyProcessor : public juce::AudioProcessor
{
public:
    PatchyProcessor();
    ~PatchyProcessor() override = default;

    //─── AudioProcessor ────────────────────────────────────────────────────
    void prepareToPlay  (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock   (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool                        hasEditor()    const override { return true; }

    const juce::String getName()               const override { return "Node Graph"; }
    bool   acceptsMidi()                       const override { return true; }
    bool   producesMidi()                      const override { return true; }
    bool   isMidiEffect()                      const override { return false; }
    double getTailLengthSeconds()              const override { return 0.0; }

    int    getNumPrograms()                    override { return 1; }
    int    getCurrentProgram()                 override { return 0; }
    void   setCurrentProgram (int)             override {}
    const juce::String getProgramName (int)    override { return "Default"; }
    void   changeProgramName (int, const juce::String&) override {}

    void getStateInformation  (juce::MemoryBlock&)        override;
    void setStateInformation  (const void*, int)           override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    //─── Graph API (called from editor / WebBridge on message thread) ──────
    GraphModel&  getGraphModel()  { return graphModel; }

    /** Call after any structural change to the graph model. */
    void rebuildProcessingGraph();
    void forceDawDeviceSelection (ProcessingGraph& graph);
    bool isStandalone = false;  // true only in standalone app, false in DAW/plugin mode

    /** Access the registry (for UI sidebar population). */
    AddonRegistry& getRegistry()       { return registry; }

    /** Open a MIDI device for the given node (called from WebBridge via PatchyEditor). */
    void setMidiDevice (const juce::String& nodeId, const juce::String& deviceId)
    {
        // Always store in the manager so applyDeviceSelections() can replay it
        // after the next graph rebuild.
        midiDeviceManager.storeSelection (nodeId, deviceId);

        // Try the current audio graph first
        bool found = midiDeviceManager.applyToGraph (nodeId, deviceId, processingGraph);

        // If not found (node is in the pending graph, not yet swapped in),
        // try the pending graph directly.
        if (! found && pendingGraph != nullptr)
            midiDeviceManager.applyToGraph (nodeId, deviceId, *pendingGraph);

        // Persist in the model for save/restore
        if (auto* node = graphModel.findNode (nodeId))
            node->selectedDeviceId = deviceId;
    }

    void setAudioDevice (const juce::String& nodeId, const juce::String& deviceName)
    {
        audioDeviceManager.storeSelection (nodeId, deviceName);
        bool found = audioDeviceManager.applyToGraph (nodeId, deviceName, processingGraph);
        if (! found && pendingGraph != nullptr)
            audioDeviceManager.applyToGraph (nodeId, deviceName, *pendingGraph);
        if (auto* node = graphModel.findNode (nodeId))
            node->selectedDeviceId = deviceName;
    }

    AudioDeviceManager& getAudioDeviceManager() { return audioDeviceManager; }

    void setAddonParameter (const juce::String& nodeId, int index, float value)
    {
        for (auto& node : processingGraph.getNodes())
            if (node->id == nodeId)
                if (auto* dyn = dynamic_cast<DynamicNodeProcessor*> (node.get()))
                    { dyn->setParameter (index, value); break; }
    }

    void newGraph()
    {
        graphModel.suspendNotifications();
        graphModel.clear();
        graphModel.resumeNotifications();  // fires onChange → rebuildProcessingGraph
    }

    void loadGraphFromJson (const juce::String& json)
    {
        // setStateInformation already handles suspend/resume and triggers onChange
        graphModel.clear();
        juce::MemoryBlock mb (json.toRawUTF8(), static_cast<size_t>(json.getNumBytesAsUTF8()));
        setStateInformation (mb.getData(), (int) mb.getSize());
    }

    void setNodeCustomName (const juce::String& nodeId, const juce::String& name)
    {
        for (auto& node : processingGraph.getNodes())
            if (node->id == nodeId)
            {
                if (auto* kbd = dynamic_cast<MidiKeyboardNode*> (node.get()))
                    { kbd->customName = name; break; }
                if (auto* dyn = dynamic_cast<DynamicNodeProcessor*> (node.get()))
                    { dyn->customName = name; break; }
            }
    }

    void pushMidiKeyEvent (const juce::String& nodeId,
                           uint8_t status, uint8_t data1, uint8_t data2)
    {
        for (auto& node : processingGraph.getNodes())
        {
            if (node->id == nodeId)
            {
                if (auto* kbd = dynamic_cast<MidiKeyboardNode*> (node.get()))
                    kbd->pushUIEvent (status, data1, data2);
                break;
            }
        }
    }

    /** Collect per-node port activity for the 30fps visualisation push. */
    std::vector<PortActivity> getPortActivity()
    {
        std::vector<PortActivity> result;

        // Collect MIDI + Audio activity from all nodes in the processing graph
        // using the lightweight atomic counters on NodeProcessor base class.
        for (auto& node : processingGraph.getNodes())
        {
            if (node == nullptr) continue;

            PortActivity a;
            a.nodeId        = node->id;
            a.midiOutEvents = node->drainMidiActivity();

            // Audio RMS from AudioMonitorBuffer (for AudioMonitorNode)
            auto it = audioMonitorBuffers.find (node->id);
            if (it != audioMonitorBuffers.end() && it->second != nullptr)
            {
                int count = std::min ((int) (lastSampleRate > 0 ? lastSampleRate * 0.02 : 960),
                                      AudioMonitorBuffer::kRingSize);
                std::vector<float> lv, rv;
                it->second->snapshot (count, lv, rv);
                float sl = 0.f, sr = 0.f;
                for (size_t i = 0; i < lv.size(); ++i)
                    { sl += lv[i]*lv[i]; sr += rv[i]*rv[i]; }
                a.audioRmsL = lv.empty() ? 0.f : std::sqrt (sl / static_cast<float>(lv.size()));
                a.audioRmsR = rv.empty() ? 0.f : std::sqrt (sr / static_cast<float>(rv.size()));
            }
            else
            {
                // For AudioIn/Out device nodes — RMS from their audio buffers
                float sl = 0.f, sr = 0.f;
                int   count = 0;

                if (! node->outputAudioBuffers.empty())
                {
                    // Multi-port addon node: report per-port RMS
                    for (auto& portBuf : node->outputAudioBuffers)
                    {
                        float ps = 0.f;
                        int   pc = portBuf.getNumSamples();
                        if (pc > 0)
                        {
                            for (int i = 0; i < pc; ++i) ps += portBuf.getSample(0,i) * portBuf.getSample(0,i);
                            a.portRms.push_back (std::sqrt (ps / (float) pc));
                        }
                        else a.portRms.push_back (0.f);
                    }
                    // Also set audioRmsL from port 0 for backwards compat
                    if (! a.portRms.empty()) { a.audioRmsL = a.portRms[0]; a.audioRmsR = a.portRms[0]; }
                }
                else
                {
                    const auto& buf = node->outputAudio.getNumChannels() > 0
                                      ? node->outputAudio : node->inputAudio;
                    count = buf.getNumSamples();
                    if (count > 0)
                    {
                        for (int i = 0; i < count; ++i) sl += buf.getSample(0,i) * buf.getSample(0,i);
                        a.audioRmsL = std::sqrt (sl / (float) count);
                        if (buf.getNumChannels() > 1)
                        {
                            for (int i = 0; i < count; ++i) sr += buf.getSample(1,i) * buf.getSample(1,i);
                            a.audioRmsR = std::sqrt (sr / (float) count);
                        }
                        else a.audioRmsR = a.audioRmsL;
                    }
                }
            }

            // For keyboard nodes — drain from dedicated buffer (not monitorBuffers)
            if (dynamic_cast<MidiKeyboardNode*> (node.get()) != nullptr)
            {
                auto kbIt = keyboardMonitorBuffers.find (node->id);
                if (kbIt != keyboardMonitorBuffers.end() && kbIt->second != nullptr)
                {
                    auto events = kbIt->second->drain();
                    for (auto& ev : events)
                    {
                        uint8_t st   = ev.statusByte;
                        uint8_t type = st & 0xF0;
                        if (type == 0x90 || type == 0x80)
                            a.incomingNotes.push_back ({ st, ev.data1 });
                    }
                }
            }

            if (a.midiOutEvents > 0 || a.audioRmsL > 0.f || a.audioRmsR > 0.f
                || ! a.incomingNotes.empty())
                result.push_back (a);
        }

        return result;
    }

    /** Clear the old graph trash bin — call periodically on the message thread. */
    void clearGraphTrash()
    {
        if (graphTrashPending.load())
        {
            graphTrashPending.store (false);
            graphTrash.reset();
        }
    }

    /** Called by WebBridge 30fps timer — snapshot all audio monitor buffers. */
    std::vector<AudioSnapshot> getAudioSnapshots()
    {
        if (audioSnapshotBusy) return {};
        audioSnapshotBusy = true;
        struct Guard { bool& b; ~Guard() { b = false; } } g { audioSnapshotBusy };

        std::vector<AudioSnapshot> result;
        // Snapshot buffer keys first to avoid iterator invalidation
        std::vector<std::pair<juce::String, AudioMonitorBuffer*>> entries;
        for (auto& [k, v] : audioMonitorBuffers)
            if (v) entries.push_back ({ k, v.get() });

        for (auto& [nodeId, buf] : entries)
        {
            AudioSnapshot snap;
            snap.nodeId     = nodeId;
            snap.sampleRate = lastSampleRate > 0.0 ? lastSampleRate : 48000.0;
            int count = std::min ((int) (snap.sampleRate * 0.2),
                                  AudioMonitorBuffer::kRingSize);
            buf->snapshot (count, snap.left, snap.right);
            result.push_back (std::move (snap));
        }
        return result;
    }

    /** Called by WebBridge 30fps timer — drains all monitor buffers. */
    std::vector<MidiMonitorBatch> drainAllMidiMonitorEvents()
    {
        std::vector<MidiMonitorBatch> result;
        for (auto& [nodeId, buf] : monitorBuffers)
        {
            auto events = buf->drain();
            if (! events.empty())
                result.push_back ({ nodeId, std::move (events) });
        }
        return result;
    }

    /** Called by ProcessingGraph when creating a MidiMonitorNode — returns shared buffer. */
    // Template helper — gets or creates a monitor buffer in any of the three maps
    template <typename BufferType>
    BufferType* getOrCreateBuffer (
        std::unordered_map<juce::String, std::unique_ptr<BufferType>>& map,
        const juce::String& nodeId)
    {
        auto it = map.find (nodeId);
        if (it == map.end())
        {
            map[nodeId] = std::make_unique<BufferType>();
            return map[nodeId].get();
        }
        return it->second.get();
    }

    MidiMonitorBuffer*  getOrCreateMidiMonitorBuffer     (const juce::String& id) { return getOrCreateBuffer (monitorBuffers,          id); }
    MidiMonitorBuffer*  getOrCreateKeyboardMonitorBuffer (const juce::String& id) { return getOrCreateBuffer (keyboardMonitorBuffers,  id); }
    AudioMonitorBuffer* getOrCreateAudioMonitorBuffer    (const juce::String& id) { return getOrCreateBuffer (audioMonitorBuffers,     id); }

    void removeMonitorBuffer (const juce::String& nodeId)
    {
        monitorBuffers.erase (nodeId);
    }
    ProcessingGraph&    getProcessingGraph()     { return processingGraph; }

    /** For telemetry: always returns the freshest graph.
     *  Before audio starts: pendingGraph has the latest rebuild.
     *  After audio starts: processingGraph holds the swapped-in graph. */
    ProcessingGraph* getLatestGraph()
    {
        if (pendingGraph != nullptr) return pendingGraph.get();
        return &processingGraph;
    }

private:
    GraphModel          graphModel;
    ProcessingGraph     processingGraph;
    AddonRegistry  registry;
    MidiDeviceManager   midiDeviceManager;
    AudioDeviceManager  audioDeviceManager;
    std::unordered_map<juce::String, std::unique_ptr<MidiMonitorBuffer>>  monitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<MidiMonitorBuffer>>  keyboardMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<AudioMonitorBuffer>> audioMonitorBuffers;
    bool audioSnapshotBusy = false;

    // Pending graph to swap in at the start of the next processBlock
    std::unique_ptr<ProcessingGraph> pendingGraph;
    std::atomic<bool>                graphPending { false };

    // Holds the old graph after an audio-thread swap until the message thread
    // can safely destroy it (avoids destructor running on the audio thread).
    std::unique_ptr<ProcessingGraph> graphTrash;
    std::atomic<bool>                graphTrashPending { false };

    double lastSampleRate  = 44100.0;
    int    lastBlockSize   = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchyProcessor)
};
