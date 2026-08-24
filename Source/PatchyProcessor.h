#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "GraphModel.h"
#include "ProcessingGraph.h"
#include "MidiDeviceNodes.h"
#include "MidiMonitorNode.h"
#include "DmxConsoleNode.h"
#include "ArtNetConsoleNode.h"
#include "AudioMonitorNode.h"
#include "MidiKeyboardNode.h"
#include <unordered_map>
#include "AudioDeviceNodes.h"
#include "UdpDeviceNodes.h"
#include "OscDeviceNodes.h"
#include "ArtNetDeviceNodes.h"
#include "DmxDeviceNodes.h"
#include "WebBridge.h"
#include "../Pax/PaxRegistry.h"
#include "../Pax/PaxScanner.h"

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
    void releaseResources() override { processingGraph.closeAllProtocolDeviceSockets(); }
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
    GraphModel&      getGraphModel()      { return graphModel; }

    /** Call after any structural change to the graph model. */
    void rebuildProcessingGraph();
    bool isStandalone = false;

    /** Prune ProcessingGraph edges for ports removed from a node. Call after GraphModel update. */
    void pruneProcessingGraphEdges (const juce::String& nodeId)
    {
        std::vector<juce::String> validPorts;
        for (auto& n : graphModel.getNodes())
            if (n.id == nodeId)
                for (auto& p : n.ports) validPorts.push_back (p.id);
        // Edge pruning only needed when reducing — no suspend needed (read-only on audio thread)
        processingGraph.pruneEdgesForNode (nodeId, validPorts);
    }

    // Returns the current audio output count for a dynamic Pax node (e.g. Spectrumyser)
    // Also resizes the live node's output buffers to avoid a full graph rebuild
    int getPaxAudioOutCount (const juce::String& nodeId)
    {
        for (auto& node : processingGraph.getNodes())
        {
            if (node->id != nodeId) continue;
            if (auto* dyn = dynamic_cast<DynamicPaxProcessor*> (node.get()))
            {
                if (dyn->audioOutputCount > 0)
                {
                    int newCount  = dyn->audioOutputCount;
                    int prevCount = (int) dyn->outputAudioBuffers.size();

                    if (newCount != prevCount)
                    {
                        // allocatePortBuffers uses resize() which is safe without suspend:
                        // - Growing: new buffers appended, existing untouched
                        // - Shrinking: existing buffers remain valid until next process() tick
                        dyn->allocatePortBuffers (dyn->audioInputCount, newCount, lastBlockSize);
                    }
                    return newCount;
                }
            }
        }
        return 0;
    }  // true only in standalone app, false in DAW/plugin mode

    /** Access the registry (for UI sidebar population). */
    PaxRegistry& getRegistry()       { return registry; }

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
        {
            node->selectedDeviceId = deviceName;
            // Store the real channel count in settingsJson so React always has it
            int chCount = 0;
            for (auto& n : processingGraph.getNodes())
            {
                if (n->id != nodeId) continue;
                if (auto* out = dynamic_cast<AudioOutDeviceNode*> (n.get()))
                    chCount = out->getDeviceChannelCount();
                else if (auto* in = dynamic_cast<AudioInDeviceNode*> (n.get()))
                    chCount = in->getDeviceChannelCount();
                break;
            }
            if (chCount > 0)
            {
                juce::var existing;
                try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
                if (existing.getDynamicObject() == nullptr)
                    existing = new juce::DynamicObject();
                existing.getDynamicObject()->setProperty ("deviceChannelCount", chCount);
                node->settingsJson = juce::JSON::toString (existing, true);
            }
        }
    }

    /** Apply a channel selection to a live audio node and persist it. */
    void setAudioDeviceChannels (const juce::String& nodeId,
                                  const std::vector<int>& channels)
    {
        audioDeviceManager.setAudioDeviceChannels (nodeId, channels, processingGraph);
        if (pendingGraph != nullptr)
            audioDeviceManager.applyChannelsToGraph (nodeId, channels, *pendingGraph);
    }

    AudioDeviceManager& getAudioDeviceManager() { return audioDeviceManager; }

    /** Apply UDP settings (port, mode, target/multicast) to a live node and persist them. */
    void setUdpSettings (const juce::String& nodeId,
                         int port, int mode,
                         const juce::String& targetHost,
                         const juce::String& multicastAddr)
    {
        UdpDeviceManager::Settings s;
        s.port          = port;
        s.mode          = static_cast<UdpMode> (mode);
        s.targetHost    = targetHost;
        s.multicastAddr = multicastAddr;

        udpDeviceManager.storeSettings (nodeId, s);
        udpDeviceManager.applyToGraph (nodeId, processingGraph);
        if (pendingGraph != nullptr)
            udpDeviceManager.applyToGraph (nodeId, *pendingGraph);

        // Persist in settingsJson for save/restore
        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();
            obj->setProperty ("udpPort",          port);
            obj->setProperty ("udpMode",           mode);
            obj->setProperty ("udpTargetHost",     targetHost);
            obj->setProperty ("udpMulticastAddr",  multicastAddr);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    /** Apply MQTT Subscribe settings and persist them. */
    void setMqttSubscribeSettings (const juce::String& nodeId,
                                   const juce::String& host, int port,
                                   const juce::String& topic, int qos,
                                   const juce::String& username, const juce::String& password)
    {
        MqttDeviceManager::Settings s;
        s.host     = host;
        s.port     = port;
        s.topic    = topic;
        s.qos      = qos;
        s.username = username;
        s.password = password;

        mqttDeviceManager.storeSettings (nodeId, s);
        mqttDeviceManager.applyToGraph (nodeId, processingGraph);
        if (pendingGraph != nullptr)
            mqttDeviceManager.applyToGraph (nodeId, *pendingGraph);

        // Persist in settingsJson for save/restore. Password is included —
        // same as usernames/hosts already stored in plain settingsJson
        // elsewhere in this file; no credential encryption exists yet
        // anywhere in Patchy's settings storage.
        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();
            obj->setProperty ("mqttHost",     host);
            obj->setProperty ("mqttPort",     port);
            obj->setProperty ("mqttTopic",    topic);
            obj->setProperty ("mqttQos",      qos);
            obj->setProperty ("mqttUsername", username);
            obj->setProperty ("mqttPassword", password);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    /** Apply MQTT Publish settings and persist them. */
    void setMqttPublishSettings (const juce::String& nodeId,
                                 const juce::String& host, int port,
                                 const juce::String& topic, int qos, bool retain,
                                 const juce::String& username, const juce::String& password)
    {
        MqttDeviceManager::Settings s;
        s.host     = host;
        s.port     = port;
        s.topic    = topic;
        s.qos      = qos;
        s.retain   = retain;
        s.username = username;
        s.password = password;

        mqttDeviceManager.storeSettings (nodeId, s);
        mqttDeviceManager.applyToGraph (nodeId, processingGraph);
        if (pendingGraph != nullptr)
            mqttDeviceManager.applyToGraph (nodeId, *pendingGraph);

        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();
            obj->setProperty ("mqttHost",     host);
            obj->setProperty ("mqttPort",     port);
            obj->setProperty ("mqttTopic",    topic);
            obj->setProperty ("mqttQos",      qos);
            obj->setProperty ("mqttRetain",   retain);
            obj->setProperty ("mqttUsername", username);
            obj->setProperty ("mqttPassword", password);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    /** Triggers an MqttConsoleNode's Send action — a discrete one-shot event,
     *  not a persisted setting, so this doesn't touch settingsJson at all
     *  (unlike setMqttXSettings above). Topic history is tracked entirely on
     *  the frontend, persisted via the node's ordinary settingsJson round-trip
     *  same as any other UI-only state.
     *
     *  Deliberately only targets the CURRENTLY ACTIVE processingGraph, unlike
     *  the settings-apply methods above which also apply to a pendingGraph if
     *  one exists. pendingGraph doesn't get process() calls until it's
     *  swapped in later — setting the pending-send flag there would fire an
     *  unexpected, stale send whenever that swap eventually happens, with no
     *  relation to this actual click. A persisted setting should apply
     *  consistently to whichever graph ends up active; a one-shot trigger
     *  should only ever affect what's live right now. */
    void mqttConsoleSend (const juce::String& nodeId, const juce::String& topic, float payload)
    {
        if (auto* node = processingGraph.findMqttConsoleNode (nodeId))
            node->sendNow (topic, payload);
    }

    /** Apply OSC settings (port, targetHost, oscAddress) to a live node and persist them. */
    void setOscSettings (const juce::String& nodeId,
                         int port,
                         const juce::String& targetHost,
                         const juce::String& oscAddress)
    {
        OscDeviceManager::Settings s;
        s.port       = port;
        s.targetHost = targetHost;
        s.oscAddress = oscAddress.isNotEmpty() ? oscAddress : "/patchy";

        oscDeviceManager.storeSettings (nodeId, s);
        oscDeviceManager.applyToGraph (nodeId, processingGraph);
        if (pendingGraph != nullptr)
            oscDeviceManager.applyToGraph (nodeId, *pendingGraph);

        // Persist in settingsJson for save/restore
        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();
            obj->setProperty ("oscPort",       port);
            obj->setProperty ("oscTargetHost", targetHost);
            obj->setProperty ("oscAddress",    oscAddress.isNotEmpty() ? oscAddress : "/patchy");
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    void setArtNetSettings (const juce::String& nodeId,
                            int universe,
                            const juce::String& targetHost)
    {
        ArtNetDeviceManager::Settings s;
        s.universe   = universe;
        s.targetHost = targetHost;

        artNetDeviceManager.storeSettings (nodeId, s);
        artNetDeviceManager.applyToGraph (nodeId, processingGraph);
        if (pendingGraph != nullptr)
            artNetDeviceManager.applyToGraph (nodeId, *pendingGraph);

        // Persist in settingsJson for save/restore
        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();
            obj->setProperty ("artNetUniverse",   universe);
            obj->setProperty ("artNetTargetHost",  targetHost);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    void setDmxSettings (const juce::String& nodeId,
                         const juce::String& devicePath,
                         int universe = 0)
    {
        DmxDeviceManager::Settings s;
        s.devicePath = devicePath;
        s.universe   = universe;

        dmxDeviceManager.storeSettings (nodeId, s);
        dmxDeviceManager.applyToGraph (nodeId, processingGraph);
        if (pendingGraph != nullptr)
            dmxDeviceManager.applyToGraph (nodeId, *pendingGraph);

        // Persist in settingsJson for save/restore
        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();
            obj->setProperty ("dmxDevicePath", devicePath);
            obj->setProperty ("dmxUniverse",   universe);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    /** Save all 512 DMX Console channel values into the node's settingsJson.
     *  Called on every fader change so settingsJson is always current for undo. */
    void saveDmxConsoleChannels (const juce::String& nodeId)
    {
        // Find the node in processingGraph or pendingGraph
        auto* console = processingGraph.findDmxConsoleNode (nodeId);
        if (console == nullptr && pendingGraph != nullptr)
            console = pendingGraph->findDmxConsoleNode (nodeId);
        if (console == nullptr) return;

        auto channels = console->getAllChannels();

        // Merge dmxChannels (base64) into existing settingsJson
        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();

            // Standard base64 encode 512 bytes
            static const char* kB64 =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            juce::String b64;
            b64.preallocateBytes (688);
            for (int i = 0; i < 512; i += 3)
            {
                uint32_t n  = (uint32_t) channels[static_cast<size_t>(i)] << 16;
                if (i + 1 < 512) n |= (uint32_t) channels[static_cast<size_t>(i + 1)] << 8;
                if (i + 2 < 512) n |= (uint32_t) channels[static_cast<size_t>(i + 2)];
                b64 += kB64[(n >> 18) & 0x3F];
                b64 += kB64[(n >> 12) & 0x3F];
                b64 += (i + 1 < 512) ? kB64[(n >> 6) & 0x3F] : '=';
                b64 += (i + 2 < 512) ? kB64[(n >> 0) & 0x3F] : '=';
            }
            obj->setProperty ("dmxChannels", b64);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    /** Restore DMX Console channel values from a settingsJson string.
     *  Called from pushSettingsToUI on undo/redo so C++ audio state stays in sync. */
    void restoreDmxConsoleChannels (const juce::String& nodeId, const juce::String& settingsJson)
    {
        restoreDmxConsoleChannels (nodeId, settingsJson, nullptr);
    }

    void restoreDmxConsoleChannels (const juce::String& nodeId, const juce::String& settingsJson,
                                    ProcessingGraph* graph)
    {
        DmxConsoleNode* console = nullptr;
        if (graph != nullptr)
            console = graph->findDmxConsoleNode (nodeId);
        if (console == nullptr)
            console = processingGraph.findDmxConsoleNode (nodeId);
        if (console == nullptr && pendingGraph != nullptr)
            console = pendingGraph->findDmxConsoleNode (nodeId);
        if (console == nullptr) return;

        try
        {
            auto parsed = juce::JSON::parse (settingsJson);
            juce::String b64 = parsed["dmxChannels"].toString();
            if (b64.isEmpty()) return;

            static const int8_t kDec[256] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
            };
            std::array<uint8_t, 512> channels {};
            int out = 0;
            for (int i = 0; i + 3 < b64.length() && out < 512; i += 4)
            {
                int a = kDec[(uint8_t)b64[i]],  b2 = kDec[(uint8_t)b64[i+1]];
                int c = kDec[(uint8_t)b64[i+2]], d = kDec[(uint8_t)b64[i+3]];
                if (a < 0 || b2 < 0) break;
                if (out < 512) channels[static_cast<size_t>(out++)] = (uint8_t)((a << 2) | (b2 >> 4));
                if (c >= 0 && out < 512) channels[static_cast<size_t>(out++)] = (uint8_t)((b2 << 4) | (c >> 2));
                if (d >= 0 && out < 512) channels[static_cast<size_t>(out++)] = (uint8_t)((c << 6) | d);
            }
            console->restoreChannels (channels);
            // Also restore blackout
            auto blackoutVar = parsed["blackout"];
            if (! blackoutVar.isVoid() && ! blackoutVar.isUndefined())
                console->restoreBlackout ((bool) blackoutVar);
        }
        catch (...) {}
    }

    void setPaxParameter (const juce::String& nodeId, int index, float value)
    {
        for (auto& node : processingGraph.getNodes())
            if (node->id == nodeId)
                if (auto* dyn = dynamic_cast<DynamicPaxProcessor*> (node.get()))
                    { dyn->setParameter (index, value); break; }
    }

    // ── ArtNet Console channel save/restore (mirrors DMX Console pattern) ────

    void saveArtNetConsoleChannels (const juce::String& nodeId)
    {
        auto* console = processingGraph.findArtNetConsoleNode (nodeId);
        if (console == nullptr && pendingGraph != nullptr)
            console = pendingGraph->findArtNetConsoleNode (nodeId);
        if (console == nullptr) return;

        auto channels = console->getAllChannels();

        if (auto* node = graphModel.findNode (nodeId))
        {
            juce::var existing;
            try { existing = juce::JSON::parse (node->settingsJson); } catch (...) {}
            if (existing.getDynamicObject() == nullptr)
                existing = new juce::DynamicObject();
            auto* obj = existing.getDynamicObject();

            static const char* kB64 =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            juce::String b64;
            b64.preallocateBytes (688);
            for (int i = 0; i < 512; i += 3)
            {
                uint32_t n  = (uint32_t) channels[static_cast<size_t>(i)] << 16;
                if (i + 1 < 512) n |= (uint32_t) channels[static_cast<size_t>(i + 1)] << 8;
                if (i + 2 < 512) n |= (uint32_t) channels[static_cast<size_t>(i + 2)];
                b64 += kB64[(n >> 18) & 0x3F];
                b64 += kB64[(n >> 12) & 0x3F];
                b64 += (i + 1 < 512) ? kB64[(n >> 6) & 0x3F] : '=';
                b64 += (i + 2 < 512) ? kB64[(n >> 0) & 0x3F] : '=';
            }
            obj->setProperty ("artNetChannels", b64);
            node->settingsJson = juce::JSON::toString (existing, true);
        }
    }

    void restoreArtNetConsoleChannels (const juce::String& nodeId, const juce::String& settingsJson)
    {
        restoreArtNetConsoleChannels (nodeId, settingsJson, nullptr);
    }

    void restoreArtNetConsoleChannels (const juce::String& nodeId, const juce::String& settingsJson,
                                       ProcessingGraph* graph)
    {
        ArtNetConsoleNode* console = nullptr;
        if (graph != nullptr)
            console = graph->findArtNetConsoleNode (nodeId);
        if (console == nullptr)
            console = processingGraph.findArtNetConsoleNode (nodeId);
        if (console == nullptr && pendingGraph != nullptr)
            console = pendingGraph->findArtNetConsoleNode (nodeId);
        if (console == nullptr) return;

        try
        {
            auto parsed = juce::JSON::parse (settingsJson);
            juce::String b64 = parsed["artNetChannels"].toString();
            if (b64.isEmpty()) return;

            static const int8_t kDec[256] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
                52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
                15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
                41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
            };
            std::array<uint8_t, 512> channels {};
            int out = 0;
            for (int i = 0; i + 3 < b64.length() && out < 512; i += 4)
            {
                int a = kDec[(uint8_t)b64[i]],  b2 = kDec[(uint8_t)b64[i+1]];
                int c = kDec[(uint8_t)b64[i+2]], d = kDec[(uint8_t)b64[i+3]];
                if (a < 0 || b2 < 0) break;
                if (out < 512) channels[static_cast<size_t>(out++)] = (uint8_t)((a << 2) | (b2 >> 4));
                if (c >= 0 && out < 512) channels[static_cast<size_t>(out++)] = (uint8_t)((b2 << 4) | (c >> 2));
                if (d >= 0 && out < 512) channels[static_cast<size_t>(out++)] = (uint8_t)((c << 6) | d);
            }
            console->restoreChannels (channels);

            // Also restore universe and blackout
            int uni = (int) parsed["universe"];
            console->setUniverse (uni);
            auto blackoutVar = parsed["blackout"];
            if (! blackoutVar.isVoid() && ! blackoutVar.isUndefined())
                console->restoreBlackout ((bool) blackoutVar);
        }
        catch (...) {}
    }

    void newGraph()
    {
        graphModel.suspendNotifications();
        graphModel.clear();
        graphModel.clearHistory();
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
                if (auto* dyn = dynamic_cast<DynamicPaxProcessor*> (node.get()))
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
    std::vector<SpectrumSnapshot> getSpectrumSnapshots()
    {
        std::vector<SpectrumSnapshot> result;
        for (auto& node : processingGraph.getNodes())
        {
            auto* dyn = dynamic_cast<DynamicPaxProcessor*> (node.get());
            if (! dyn || dyn->getPaxName() != "Spectrumyser") continue;

            auto sd = dyn->getSpectrumData();
            if (! sd.valid || sd.fftSize <= 0 || ! sd.mags) continue;

            SpectrumSnapshot snap;
            snap.nodeId     = node->id;
            snap.sampleRate = lastSampleRate;
            snap.magnitudes.assign (sd.mags, sd.mags + sd.fftSize);

            int bands = (int) dyn->getParameter (0);
            snap.bandCount = bands;
            for (int b = 0; b < bands; ++b)
            {
                snap.bandLow.push_back  (dyn->getParameter (1 + b * 2));
                snap.bandHigh.push_back (dyn->getParameter (1 + b * 2 + 1));
            }
            result.push_back (std::move (snap));
        }
        return result;
    }

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

            // Current DMX channel level, for gradual intensity rendering —
            // works for any node whose lightweight Value mirror is DMX-typed
            // (built-in DmxIn/DmxConsole, or a Pax like AudioFreqToDmxPax),
            // not just a hardcoded list of classes. Read unconditionally
            // (not gated on outputValueCount>0) since built-in nodes only
            // touch outputValues[0] on an actual change and leave it holding
            // the last real value otherwise — exactly the "current level"
            // this is meant to report, not "did it just change".
            if (node->outputValues[0].type == PAX_TYPE_DMX)
                a.dmxValue = node->outputValues[0].value;

            // Live values for any read-only parameters (see PaxAPI.h's
            // PAX_isParameterReadOnly) — checked every poll rather than
            // resolved once, since which indices are read-only doesn't
            // change per-instance and the check itself is a single cheap
            // function-pointer call, not worth caching separately.
            if (auto* dynRO = dynamic_cast<DynamicPaxProcessor*> (node.get()))
            {
                int paramCount = dynRO->getParameterCount();
                static std::unordered_map<juce::String, float> lastReadOnlyValue;
                for (int p = 0; p < paramCount; ++p)
                {
                    if (! dynRO->isParameterReadOnly (p)) continue;
                    float current = dynRO->getParameter (p);
                    juce::String key = node->id + "_" + juce::String (p);
                    auto it = lastReadOnlyValue.find (key);
                    if (it != lastReadOnlyValue.end() && it->second == current)
                        continue;   // unchanged since last poll — nothing to persist
                    lastReadOnlyValue[key] = current;

                    // FIX (2026-08-18): persist the change directly into
                    // graphModel's own settingsJson, here, unconditionally
                    // — not gated on any UI state. The original fix for
                    // this (GenericNode.tsx syncing paramValues into
                    // settingsJson whenever a live value arrived) only
                    // ran while that specific node's settings panel
                    // happened to be open, which real testing showed
                    // doesn't reliably keep the persisted value current —
                    // a rebuild (add/delete a node, undo/redo, anywhere
                    // in the graph) can happen at any moment, almost
                    // certainly while nobody has this node's panel open
                    // at all, so the "only sync while watching" approach
                    // defeated the whole point of the fix. This runs on
                    // every 30fps poll regardless, on the message thread
                    // — same thread WebBridge::handleSetNodeSettings
                    // already safely mutates graphModel from, so no new
                    // thread-safety concern here.
                    juce::Array<juce::var> settingsArr;
                    for (int i = 0; i < paramCount; ++i)
                        settingsArr.add (dynRO->getParameter (i));
                    graphModel.setNodeSettings (node->id, juce::JSON::toString (juce::var (settingsArr), true));
                }

                // Live-synced editable parameters (see PaxAPI.h's
                // PAX_isParameterLiveSynced) — same change-detection and
                // persist-to-settingsJson pattern as the read-only loop
                // above, but for a genuinely different situation: this
                // parameter stays a normal, editable slider — the point
                // here is a backend-side change (not a user edit) moving
                // that slider's on-screen position live, e.g.
                // UdpValueToMidiCCPax's "MIDI CC" following an incoming
                // 5-byte packet's own CC-number override. Deliberately a
                // separate loop from the read-only one above rather than
                // merged into it, even though the persist logic is
                // identical — keeps the two situations (display-only vs
                // still-editable-but-pushed) clearly distinct in the code,
                // matching how differently WebBridge.h's own two fields
                // for this (paxReadOnlyValues vs liveSyncedSettingsJson)
                // are documented and used downstream. Sets
                // a.liveSyncedSettingsJson (see that field's own comment
                // for why this is deliberately opt-in and narrowly scoped
                // rather than watching every parameter generically).
                static std::unordered_map<juce::String, float> lastLiveSyncedValue;
                for (int p = 0; p < paramCount; ++p)
                {
                    if (! dynRO->isParameterLiveSynced (p)) continue;
                    float current = dynRO->getParameter (p);
                    juce::String key = node->id + "_" + juce::String (p);
                    auto it = lastLiveSyncedValue.find (key);
                    if (it != lastLiveSyncedValue.end() && it->second == current)
                        continue;   // unchanged since last poll — nothing to push

                    lastLiveSyncedValue[key] = current;

                    juce::Array<juce::var> liveArr;
                    for (int i = 0; i < paramCount; ++i)
                        liveArr.add (dynRO->getParameter (i));
                    juce::String liveJson = juce::JSON::toString (juce::var (liveArr), true);
                    graphModel.setNodeSettings (node->id, liveJson);
                    a.liveSyncedSettingsJson = liveJson;
                }

                for (int p = 0; p < paramCount; ++p)
                    if (dynRO->isParameterReadOnly (p))
                        a.paxReadOnlyValues.emplace_back (p, dynRO->getParameter (p));
            }

            // Byte-rate for UDP In nodes
            if (auto* udpIn = dynamic_cast<UdpInDeviceNode*> (node.get()))
                a.udpBytes = udpIn->drainByteActivity();

            // Byte-rate for OSC In nodes (reuses udpBytes field — same UI display)
            if (auto* oscIn = dynamic_cast<OscInDeviceNode*> (node.get()))
                a.udpBytes = oscIn->drainByteActivity();

            // Byte-rate for ArtNet In nodes (reuses udpBytes field — same UI display)
            if (auto* artIn = dynamic_cast<ArtNetInDeviceNode*> (node.get()))
                a.udpBytes = artIn->drainByteActivity();

            // Byte-rate for DMX In nodes (reuses udpBytes field — same UI display)
            if (auto* dmxIn = dynamic_cast<DmxInDeviceNode*> (node.get()))
            {
                a.udpBytes   = dmxIn->drainByteActivity();
                a.dmxIsMk2   = dmxIn->isMk2.load (std::memory_order_relaxed);
            }
            if (auto* dmxOut = dynamic_cast<DmxOutDeviceNode*> (node.get()))
                a.dmxIsMk2 = dmxOut->isMk2.load (std::memory_order_relaxed);

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
                    // Multi-port Pax node: report per-port RMS
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

            // FIX (2026-08-18): this gate predates the paxReadOnlyValues
            // field entirely, and never accounted for it — a node whose
            // only "activity" in a given poll is a read-only parameter
            // change (no MIDI/audio/notes at all, which is exactly
            // ValueToDMXPax/OscToValuePax's situation) was silently never
            // added to result at all, meaning it never reached
            // pushPortActivity()/onPortActivity(), ever — regardless of
            // how many times the value actually changed. The backend's
            // own detection+persistence (the loop above) was never
            // affected by this, since it runs before this gate and reads
            // the Pax's live state directly — which is exactly why the
            // rebuild-restoration fix worked while this live-display
            // channel stayed silent the whole time.
            if (a.midiOutEvents > 0 || a.audioRmsL > 0.f || a.audioRmsR > 0.f
                || ! a.incomingNotes.empty() || ! a.paxReadOnlyValues.empty())
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

    /**
     * Shared drain-all-buffers-into-batches pattern, used by the four
     * drainAllXxxMonitorEvents() functions below. Each protocol's map
     * differs only in its buffer type — the drain logic itself was
     * identical, copy-pasted four times before this consolidation
     * (2026-08-12). BatchType is explicit at the call site (can't be
     * deduced from the map alone); MapType is deduced from whichever
     * map is passed in.
     */
    template <typename BatchType, typename MapType>
    static std::vector<BatchType> drainMonitorMap (MapType& buffers)
    {
        std::vector<BatchType> result;
        for (auto& [nodeId, buf] : buffers)
        {
            auto events = buf->drain();
            if (! events.empty())
                result.push_back ({ nodeId, std::move (events) });
        }
        return result;
    }

    /** Called by WebBridge 30fps timer — drains all monitor buffers. */
    std::vector<MidiMonitorBatch> drainAllMidiMonitorEvents()
    {
        return drainMonitorMap<MidiMonitorBatch> (monitorBuffers);
    }

    /** Called by WebBridge 30fps timer — drains all OSC monitor buffers. */
    std::vector<OscMonitorBatch> drainAllOscMonitorEvents()
    {
        return drainMonitorMap<OscMonitorBatch> (oscMonitorBuffers);
    }

    /** Called by WebBridge 30fps timer — drains all UDP monitor buffers. */
    std::vector<UdpMonitorBatch> drainAllUdpMonitorEvents()
    {
        return drainMonitorMap<UdpMonitorBatch> (udpMonitorBuffers);
    }

    /** Called by WebBridge 30fps timer — drains all MQTT monitor buffers. */
    std::vector<MqttMonitorBatch> drainAllMqttMonitorEvents()
    {
        return drainMonitorMap<MqttMonitorBatch> (mqttMonitorBuffers);
    }

    /** Called by WebBridge 30fps timer — drains DMX monitor + console snapshots. */
    std::vector<ArtNetSnapshot> drainAllArtNetSnapshots()
    {
        std::vector<ArtNetSnapshot> result;
        for (auto& [nodeId, buf] : artNetMonitorBuffers)
        {
            ArtNetSnapshot snap;
            snap.nodeId = nodeId;
            if (buf->drain (snap.channels, snap.universe))
            {
                snap.hasData = true;
                result.push_back (std::move (snap));
            }
        }
        for (auto& [nodeId, buf] : artNetConsoleBuffers)
        {
            ArtNetSnapshot snap;
            snap.nodeId = nodeId;
            if (buf->drain (snap.channels, snap.universe))
            {
                snap.hasData = true;
                result.push_back (std::move (snap));
            }
        }
        return result;
    }

    std::vector<DmxSnapshot> drainAllDmxSnapshots()
    {
        std::vector<DmxSnapshot> result;
        for (auto& [nodeId, buf] : dmxMonitorBuffers)
        {
            DmxSnapshot snap;
            snap.nodeId = nodeId;
            if (buf->drain (snap.channels))
            {
                snap.hasData = true;
                result.push_back (std::move (snap));
            }
        }
        for (auto& [nodeId, buf] : dmxConsoleBuffers)
        {
            DmxSnapshot snap;
            snap.nodeId = nodeId;
            if (buf->drain (snap.channels))
            {
                snap.hasData = true;
                result.push_back (std::move (snap));
            }
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
    DmxMonitorBuffer*      getOrCreateDmxMonitorBuffer      (const juce::String& id) { return getOrCreateBuffer (dmxMonitorBuffers,       id); }
    DmxMonitorBuffer*      getOrCreateDmxConsoleBuffer      (const juce::String& id) { return getOrCreateBuffer (dmxConsoleBuffers,       id); }
    ArtNetMonitorBuffer*   getOrCreateArtNetMonitorBuffer   (const juce::String& id) { return getOrCreateBuffer (artNetMonitorBuffers,    id); }
    ArtNetMonitorBuffer*   getOrCreateArtNetConsoleBuffer   (const juce::String& id) { return getOrCreateBuffer (artNetConsoleBuffers,    id); }
    OscMonitorBuffer*      getOrCreateOscMonitorBuffer      (const juce::String& id) { return getOrCreateBuffer (oscMonitorBuffers,       id); }
    UdpMonitorBuffer*      getOrCreateUdpMonitorBuffer      (const juce::String& id) { return getOrCreateBuffer (udpMonitorBuffers,       id); }
    MqttMonitorBuffer*     getOrCreateMqttMonitorBuffer     (const juce::String& id) { return getOrCreateBuffer (mqttMonitorBuffers,      id); }

    void removeMonitorBuffer (const juce::String& nodeId)
    {
        monitorBuffers.erase (nodeId);
    }
    ProcessingGraph&    getProcessingGraph()     { return processingGraph; }
    ProcessingGraph*    getPendingGraph()         { return pendingGraph.get(); }

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
    PaxRegistry  registry;
    MidiDeviceManager   midiDeviceManager;
    AudioDeviceManager  audioDeviceManager;
    UdpDeviceManager    udpDeviceManager;
    OscDeviceManager    oscDeviceManager;
    MqttDeviceManager   mqttDeviceManager;
    ArtNetDeviceManager artNetDeviceManager;
    DmxDeviceManager    dmxDeviceManager;
    std::unordered_map<juce::String, std::unique_ptr<MidiMonitorBuffer>>  monitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<MidiMonitorBuffer>>  keyboardMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<AudioMonitorBuffer>> audioMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<DmxMonitorBuffer>>      dmxMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<DmxMonitorBuffer>>      dmxConsoleBuffers;
    std::unordered_map<juce::String, std::unique_ptr<ArtNetMonitorBuffer>>   artNetMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<ArtNetMonitorBuffer>>   artNetConsoleBuffers;
    std::unordered_map<juce::String, std::unique_ptr<OscMonitorBuffer>>      oscMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<UdpMonitorBuffer>>      udpMonitorBuffers;
    std::unordered_map<juce::String, std::unique_ptr<MqttMonitorBuffer>>     mqttMonitorBuffers;
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
