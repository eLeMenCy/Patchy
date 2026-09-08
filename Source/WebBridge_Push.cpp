#include "WebBridge.h"
#include "MidiDeviceNodes.h"
#include "AudioDeviceNodes.h"
#include "ProcessingGraph.h"
#include "MidiMonitorNode.h"
#include "SerialPort.h"
#include <unordered_map>

#if HAS_BUNDLED_UI
  #include "BinaryData.h"
#endif

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushUndoState()
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("canUndo", graph.canUndo());
    obj->setProperty ("canRedo", graph.canRedo());
    pushToUI ("onUndoState", juce::JSON::toString (juce::var (obj.release()), false));
}

// ─────────────────────────────────────────────────────────────────────────────

void WebBridge::pushPaxList()
{
    if (! connected || registry == nullptr) return;

    // Build JSON: { paxItems: [ {name, vendor, version, nodeType}, ... ] }
    juce::Array<juce::var> arr;
    for (const auto& e : registry->getEntries())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name",        e.name);
        obj->setProperty ("vendor",      e.vendor);
        obj->setProperty ("version",     e.version);
        obj->setProperty ("nodeType",    e.nodeType);
        // Effective port counts (0 = default for nodeType)
        int paxType = e.nodeType;
        obj->setProperty ("audioInputs",  e.audioInputs  > 0 ? e.audioInputs  : (paxType == 2 || paxType == 3 ? 1 : 0));
        obj->setProperty ("audioOutputs", e.audioOutputs > 0 ? e.audioOutputs : (paxType == 2 || paxType == 3 ? 1 : 0));
        obj->setProperty ("midiInputs",   e.midiInputs   > 0 ? e.midiInputs   : (paxType == 1 || paxType == 3 ? 1 : 0));
        obj->setProperty ("midiOutputs",  e.midiOutputs  > 0 ? e.midiOutputs  : (paxType == 1 || paxType == 3 ? 1 : 0));
        // Value ports have no nodeType-implied default (see GraphModel.cpp's
        // portsForType) — purely whatever the Pax exported via
        // PAX_getValueInputCount/PAX_getValueOutputCount.
        obj->setProperty ("valueInputs",  e.valueInputs);
        obj->setProperty ("valueOutputs", e.valueOutputs);
        // Per-port types + colour category override, threaded through so
        // the sidebar can show each Pax's actual auto-detected colour
        // (Hybrid/Converter/native-type) rather than a generic per-paxType
        // bucket colour — keeps sidebar and placed-node colour consistent.
        {
            juce::Array<juce::var> inTags, outTags;
            for (int tag : e.valueInputTypes)  inTags.add  (tagForValueType (paxValueTypeFromTag (tag)));
            for (int tag : e.valueOutputTypes) outTags.add (tagForValueType (paxValueTypeFromTag (tag)));
            obj->setProperty ("valueInputTypes",  inTags);
            obj->setProperty ("valueOutputTypes", outTags);
        }
        obj->setProperty ("colourCategory", e.colourCategory);

        // Include parameter descriptors so UI can render sliders
        juce::Array<juce::var> params;
        if (e.create && e.getParamCount && e.getParamInfo)
        {
            auto* tmp = e.create();
            int count = e.getParamCount (tmp);
            for (int p = 0; p < count; ++p)
            {
                PAX_ParameterInfo info {};
                e.getParamInfo (tmp, p, &info);
                auto* po = new juce::DynamicObject();
                po->setProperty ("index",        p);
                po->setProperty ("name",         juce::String (info.name));
                po->setProperty ("min",          info.minValue);
                po->setProperty ("max",          info.maxValue);
                po->setProperty ("defaultValue", info.defaultValue);
                po->setProperty ("step",         info.step);
                // Optional — see PaxAPI.h's PAX_isParameterReadOnly. Not
                // exported by a Pax means every one of its parameters is
                // a normal editable control (safe default).
                po->setProperty ("readOnly",     e.isParamReadOnly ? (e.isParamReadOnly (tmp, p) != 0) : false);
                params.add (po);
            }
            e.destroy (tmp);
        }
        obj->setProperty ("params", params);
        arr.add (obj);
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("paxItems", arr);

    auto json = juce::JSON::toString (root, true);
    json = json.replace ("\\", "\\\\").replace ("`", "\\`");

    pushToUI ("onPaxList", json);
}

void WebBridge::pushMidiDevices()
{
    pushToUI ("onMidiDevices", juce::JSON::toString (MidiDeviceManager::getAvailableDevicesVar(), true));
}

void WebBridge::pushAudioDevices()
{
    pushToUI ("onAudioDevices", juce::JSON::toString (AudioDeviceManager::getAvailableDevicesVar (isStandalone), true));
}

void WebBridge::pushSerialPorts()
{
    auto ports = SerialPort::listPorts();
    juce::Array<juce::var> arr;
    for (const auto& p : ports)
        arr.add (juce::String (p));
    pushToUI ("onSerialPorts", juce::JSON::toString (juce::var (arr), true));
}

void WebBridge::pushDmxSnapshots()
{
    if (! connected || ! drainDmxSnapshots || webView == nullptr) return;

    auto snaps = drainDmxSnapshots();
    if (snaps.empty()) return;

    // Encode channel data as standard base64 (RFC 4648, compatible with JS atob())
    // 512 bytes → 684 chars, vs JSON integer array ~1500 chars — 2x smaller payload
    static constexpr auto Q = "\"";
    static const char* kB64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    juce::MemoryOutputStream json;
    json << "[";
    for (size_t si = 0; si < snaps.size(); ++si)
    {
        const auto& snap = snaps[si];
        if (si > 0) json << ",";

        // Standard base64 encode 512 bytes
        const uint8_t* src = snap.channels.data();
        const int srcLen   = 512;
        juce::String b64;
        b64.preallocateBytes (684 + 4);
        for (int i = 0; i < srcLen; i += 3)
        {
            uint32_t b  = (uint32_t) src[i] << 16;
            if (i + 1 < srcLen) b |= (uint32_t) src[i + 1] << 8;
            if (i + 2 < srcLen) b |= (uint32_t) src[i + 2];

            b64 += kB64Chars[(b >> 18) & 0x3F];
            b64 += kB64Chars[(b >> 12) & 0x3F];
            b64 += (i + 1 < srcLen) ? kB64Chars[(b >>  6) & 0x3F] : '=';
            b64 += (i + 2 < srcLen) ? kB64Chars[(b >>  0) & 0x3F] : '=';
        }

        json << "{" << Q << "id"  << Q << ":" << Q << snap.nodeId << Q << ","
             << Q << "b64" << Q << ":" << Q << b64 << Q << "}";
    }
    json << "]";
    pushToUI ("onDmxSnapshot", json.toString());
}

void WebBridge::pushArtNetSnapshots()
{
    if (! connected || ! drainArtNetSnapshots || webView == nullptr) return;

    auto snaps = drainArtNetSnapshots();
    if (snaps.empty()) return;

    static constexpr auto Q = "\"";
    static const char* kB64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    juce::MemoryOutputStream json;
    json << "[";
    for (size_t si = 0; si < snaps.size(); ++si)
    {
        const auto& snap = snaps[si];
        if (si > 0) json << ",";

        const uint8_t* src = snap.channels.data();
        const int srcLen   = 512;
        juce::String b64;
        b64.preallocateBytes (684 + 4);
        for (int i = 0; i < srcLen; i += 3)
        {
            uint32_t b  = (uint32_t) src[i] << 16;
            if (i + 1 < srcLen) b |= (uint32_t) src[i + 1] << 8;
            if (i + 2 < srcLen) b |= (uint32_t) src[i + 2];
            b64 += kB64Chars[(b >> 18) & 0x3F];
            b64 += kB64Chars[(b >> 12) & 0x3F];
            b64 += (i + 1 < srcLen) ? kB64Chars[(b >>  6) & 0x3F] : '=';
            b64 += (i + 2 < srcLen) ? kB64Chars[(b >>  0) & 0x3F] : '=';
        }

        json << "{" << Q << "id"       << Q << ":" << Q << snap.nodeId  << Q << ","
             << Q << "b64"     << Q << ":" << Q << b64          << Q << ","
             << Q << "universe" << Q << ":" << snap.universe << "}";
    }
    json << "]";
    pushToUI ("onArtNetSnapshot", json.toString());
}

void WebBridge::timerCallback()
{
    // Clear old graph trash on message thread before any snapshot/drain
    if (clearGraphTrash) clearGraphTrash();
    if (connected)
    {
        pushMidiMonitorEvents();
        pushAudioSnapshots();
        pushSpectrumSnapshots();
        pushPortActivity();
        pushDmxSnapshots();
        pushArtNetSnapshots();
        pushOscMonitorEvents();
        pushUdpMonitorEvents();
        pushMqttMonitorEvents();
        pushAudioPlayerStatus();
        pushPendingAudioPlayerFileLoads();
    }
}

void WebBridge::pushMidiMonitorEvents()
{
    if (! connected || ! drainMonitor || webView == nullptr) return;

    auto batches = drainMonitor();

    if (batches.empty()) return;

    // Build JSON using explicit quote character to avoid escape confusion.
    const juce::juce_wchar Q = '"';

    juce::String json;
    json << "[";

    bool firstBatch = true;
    for (const auto& batch : batches)
    {
        if (! firstBatch) json << ",";
        firstBatch = false;

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << batch.nodeId << Q << ","
             << Q << "events" << Q << ":[";

        bool firstEv = true;
        for (const auto& ev : batch.events)
        {
            if (! firstEv) json << ",";
            firstEv = false;

            // Escape backslashes and quotes in string fields
            juce::String sn = ev.sourceNode.replace  ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String sd = ev.sourceDevice.replace ("\\", "\\\\").replace ("\"", "\\\"");

            json << "{"
                 << Q << "ts" << Q << ":" << ev.timestampMs  << ","
                 << Q << "sn" << Q << ":" << Q << sn << Q    << ","
                 << Q << "sd" << Q << ":" << Q << sd << Q    << ","
                 << Q << "st" << Q << ":" << (int) ev.statusByte << ","
                 << Q << "d1" << Q << ":" << (int) ev.data1      << ","
                 << Q << "d2" << Q << ":" << (int) ev.data2
                 << "}";
        }
        json << "]}";
    }
    json << "]";

    pushToUI ("onMidiMonitorEvents", json);
}

void WebBridge::pushOscMonitorEvents()
{
    if (! connected || ! drainOscMonitor || webView == nullptr) return;

    auto batches = drainOscMonitor();

    if (batches.empty()) return;

    const juce::juce_wchar Q = '"';

    juce::String json;
    json << "[";

    bool firstBatch = true;
    for (const auto& batch : batches)
    {
        if (! firstBatch) json << ",";
        firstBatch = false;

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << batch.nodeId << Q << ","
             << Q << "events" << Q << ":[";

        bool firstEv = true;
        for (const auto& ev : batch.events)
        {
            if (! firstEv) json << ",";
            firstEv = false;

            // Escape backslashes and quotes in string fields
            juce::String ad = ev.address.replace    ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String tt = ev.typeTags.replace    ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String ar = ev.argsDisplay.replace ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String sn = ev.sourceNode.replace   ("\\", "\\\\").replace ("\"", "\\\"");

            json << "{"
                 << Q << "ts" << Q << ":" << ev.timestampMs << ","
                 << Q << "sn" << Q << ":" << Q << sn << Q << ","
                 << Q << "ad" << Q << ":" << Q << ad << Q << ","
                 << Q << "tt" << Q << ":" << Q << tt << Q << ","
                 << Q << "ar" << Q << ":" << Q << ar << Q << ","
                 << Q << "by" << Q << ":" << ev.byteCount
                 << "}";
        }
        json << "]}";
    }
    json << "]";

    pushToUI ("onOscMonitorEvents", json);
}

void WebBridge::pushUdpMonitorEvents()
{
    if (! connected || ! drainUdpMonitor || webView == nullptr) return;

    auto batches = drainUdpMonitor();

    if (batches.empty()) return;

    const juce::juce_wchar Q = '"';

    juce::String json;
    json << "[";

    bool firstBatch = true;
    for (const auto& batch : batches)
    {
        if (! firstBatch) json << ",";
        firstBatch = false;

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << batch.nodeId << Q << ","
             << Q << "events" << Q << ":[";

        bool firstEv = true;
        for (const auto& ev : batch.events)
        {
            if (! firstEv) json << ",";
            firstEv = false;

            juce::String sn = ev.sourceNode.replace ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String ip = ev.senderIp.replace   ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String hx = ev.hexPreview;   // hex digits only — no escaping needed

            json << "{"
                 << Q << "ts" << Q << ":" << ev.timestampMs << ","
                 << Q << "sn" << Q << ":" << Q << sn << Q << ","
                 << Q << "ip" << Q << ":" << Q << ip << Q << ","
                 << Q << "pt" << Q << ":" << ev.senderPort << ","
                 << Q << "by" << Q << ":" << ev.byteCount << ","
                 << Q << "hx" << Q << ":" << Q << hx << Q
                 << "}";
        }
        json << "]}";
    }
    json << "]";

    pushToUI ("onUdpMonitorEvents", json);
}

void WebBridge::pushMqttMonitorEvents()
{
    if (! connected || ! drainMqttMonitor || webView == nullptr) return;

    auto batches = drainMqttMonitor();

    if (batches.empty()) return;

    const juce::juce_wchar Q = '"';

    juce::String json;
    json << "[";

    bool firstBatch = true;
    for (const auto& batch : batches)
    {
        if (! firstBatch) json << ",";
        firstBatch = false;

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << batch.nodeId << Q << ","
             << Q << "events" << Q << ":[";

        bool firstEv = true;
        for (const auto& ev : batch.events)
        {
            if (! firstEv) json << ",";
            firstEv = false;

            juce::String tp = ev.topic.replace   ("\\", "\\\\").replace ("\"", "\\\"");
            juce::String pl = ev.payload.replace ("\\", "\\\\").replace ("\"", "\\\"");

            json << "{"
                 << Q << "ts" << Q << ":" << ev.timestampMs << ","
                 << Q << "tp" << Q << ":" << Q << tp << Q << ","
                 << Q << "pl" << Q << ":" << Q << pl << Q
                 << "}";
        }
        json << "]}";
    }
    json << "]";

    pushToUI ("onMqttMonitorEvents", json);
}

void WebBridge::pushAudioSnapshots()
{
    if (! connected || ! getAudioSnapshots || webView == nullptr) return;

    auto snapshots = getAudioSnapshots();

    if (snapshots.empty()) return;

    // Performance: downsample to kDisplaySamples points before encoding.
    // The canvas is ~280px wide so more samples are invisible.
    // Encode as integers (value * 1000, clamped ±1000) to avoid decimal points —
    // reduces payload by ~3x versus floating-point strings.
    static constexpr int kDisplaySamples = 512;

    const juce::juce_wchar Q = '"';
    juce::String json;
    json << "[";
    bool firstSnap = true;

    for (const auto& snap : snapshots)
    {
        if (! firstSnap) json << ",";
        firstSnap = false;

        const int srcCount = (int) snap.left.size();
        const int outCount = std::min (kDisplaySamples, srcCount);

        juce::String lStr, rStr;
        lStr.preallocateBytes (static_cast<size_t>(outCount) * 6);
        rStr.preallocateBytes (static_cast<size_t>(outCount) * 6);

        for (int i = 0; i < outCount; ++i)
        {
            // Downsample: pick evenly-spaced samples from the source buffer
            int idx = (srcCount > outCount)
                        ? (int) ((int64_t) i * srcCount / outCount)
                        : i;
            idx = juce::jlimit (0, srcCount - 1, idx);

            // Encode as integer (×1000), saves ~3× vs "0.1234"
            int lv = juce::jlimit (-1000, 1000, (int) (snap.left[static_cast<size_t>(idx)]  * 1000.0f));
            int rv = juce::jlimit (-1000, 1000, (int) (snap.right[static_cast<size_t>(idx)] * 1000.0f));

            if (i > 0) { lStr << ","; rStr << ","; }
            lStr << lv;
            rStr << rv;
        }

        json << "{"
             << Q << "nodeId" << Q << ":" << Q << snap.nodeId << Q << ","
             << Q << "sr"     << Q << ":" << (int) snap.sampleRate << ","
             << Q << "n"      << Q << ":" << outCount << ","
             << Q << "l"      << Q << ":" << Q << lStr << Q << ","
             << Q << "r"      << Q << ":" << Q << rStr << Q
             << "}";
    }
    json << "]";
    pushToUI ("onAudioSnapshot", json);
}

void WebBridge::pushPortActivity()
{
    if (! connected || ! getPortActivity || webView == nullptr) return;

    auto activities = getPortActivity();

    if (activities.empty()) return;

    const juce::juce_wchar Q = '"';
    juce::String json;
    json << "[";
    bool first = true;
    for (const auto& a : activities)
    {
        if (! first) json << ",";
        first = false;
        // Encode RMS as integer (×1000) for compactness
        int lv = juce::jlimit (0, 1000, (int) (a.audioRmsL * 1000.0f));
        int rv = juce::jlimit (0, 1000, (int) (a.audioRmsR * 1000.0f));
        // Current DMX channel level, same ×1000 integer encoding as RMS above
        int dv = juce::jlimit (0, 1000, (int) (a.dmxValue * 1000.0f));
        // Encode incoming notes as "s,n s,n ..." e.g. "144,60 128,60"
        juce::String notesStr;
        for (const auto& [st, n] : a.incomingNotes)
        {
            if (notesStr.isNotEmpty()) notesStr << " ";
            notesStr << (int) st << "," << (int) n;
        }

        // Build portRms array for multi-port nodes
        juce::String portRmsStr = "[";
        for (size_t pi = 0; pi < a.portRms.size(); ++pi)
        {
            if (pi > 0) portRmsStr << ",";
            portRmsStr << juce::jlimit (0, 1000, (int) (a.portRms[pi] * 1000.0f));
        }
        portRmsStr << "]";

        // Build paxReadOnly array — [{index, value}, ...] for this node's
        // read-only parameters, empty for every node except a Pax that has
        // at least one. Value sent as a plain float, not the ×1000-integer
        // compactness convention RMS/DMX use above — those are always
        // 0.0-1.0 by design, but a read-only parameter could legitimately
        // be any range (matching whatever min/max the Pax itself declared
        // for it), and this array is rare/small enough (empty for most
        // nodes) that precision matters more here than shaving bytes.
        juce::String paxReadOnlyStr = "[";
        for (size_t ri = 0; ri < a.paxReadOnlyValues.size(); ++ri)
        {
            if (ri > 0) paxReadOnlyStr << ",";
            paxReadOnlyStr << "{" << Q << "index" << Q << ":" << a.paxReadOnlyValues[ri].first
                           << "," << Q << "value" << Q << ":"
                           << juce::String (a.paxReadOnlyValues[ri].second, 4) << "}";
        }
        paxReadOnlyStr << "]";

        // Live-synced editable parameters (see PaxAPI.h's
        // PAX_isParameterLiveSynced, and PatchyProcessor.h's own matching
        // change-detection loop that sets this field) — deliberately NOT
        // folded into the onPortActivity JSON payload above the way
        // paxReadOnly is: this reuses the existing, already-proven
        // settingsJson push path instead (the same one undo/redo already
        // restores slider positions through), since the whole point here
        // is moving a still-editable control's on-screen position, which
        // is what that path is for — paxReadOnly's own array is for a
        // separate, display-only live mirror with no editable control to
        // move at all.
        if (a.liveSyncedSettingsJson.isNotEmpty())
            pushSettingsToUI (a.nodeId, a.liveSyncedSettingsJson);

        json << "{"
             << Q << "id"       << Q << ":" << Q << a.nodeId       << Q << ","
             << Q << "midi"     << Q << ":" << a.midiOutEvents             << ","
             << Q << "l"        << Q << ":" << lv                          << ","
             << Q << "r"        << Q << ":" << rv                          << ","
             << Q << "portRms"  << Q << ":" << portRmsStr                  << ","
             << Q << "paxReadOnly" << Q << ":" << paxReadOnlyStr            << ","
             << Q << "notes"    << Q << ":" << Q << notesStr        << Q   << ","
             << Q << "bytes"    << Q << ":" << a.udpBytes                  << ","
             << Q << "isMk2"    << Q << ":" << (a.dmxIsMk2 ? "true" : "false") << ","
             << Q << "dmxValue" << Q << ":" << dv
             << "}";
    }
    json << "]";
    pushToUI ("onPortActivity", json);
}

void WebBridge::pushGraphToUI()
{
    pushToUI ("onGraphUpdate", juce::JSON::toString (graph.toVar(), true));
}

void WebBridge::pushSettingsToUI (const juce::String& nodeId, const juce::String& settingsJson)
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("nodeId",       nodeId);
    obj->setProperty ("settingsJson", settingsJson);
    pushToUI ("onNodeSettings", juce::JSON::toString (juce::var (obj.release()), false));

    // If this settingsJson contains dmxChannels, restore C++ fader state (undo/redo)
    if (onRestoreDmxConsoleChannels && settingsJson.contains ("dmxChannels"))
        onRestoreDmxConsoleChannels (nodeId, settingsJson);

    // If this settingsJson contains blackout, restore C++ blackout state silently (no flash)
    if (onRestoreDmxBlackout && settingsJson.contains ("blackout"))
    {
        try
        {
            auto parsed = juce::JSON::parse (settingsJson);
            bool active = (bool) parsed["blackout"];
            onRestoreDmxBlackout (nodeId, active);
        }
        catch (...) {}
    }

    // ArtNet Console restore (mirrors DMX Console)
    if (onRestoreArtNetConsoleChannels && settingsJson.contains ("artNetChannels"))
        onRestoreArtNetConsoleChannels (nodeId, settingsJson);

    if (onRestoreArtNetBlackout && settingsJson.contains ("blackout"))
    {
        try
        {
            auto parsed = juce::JSON::parse (settingsJson);
            bool active = (bool) parsed["blackout"];
            onRestoreArtNetBlackout (nodeId, active);
        }
        catch (...) {}
    }
}

// ── Push spectrum snapshots (FFT magnitudes for Spectrumyser nodes) ───────────
void WebBridge::pushSpectrumSnapshots()
{
    if (! connected || ! getSpectrumSnapshots || webView == nullptr) return;

    auto snaps = getSpectrumSnapshots();
    if (snaps.empty()) return;

    const juce::String Q = "\"";
    juce::String json = "[";
    bool first = true;
    for (auto& s : snaps)
    {
        if (! first) json += ",";
        first = false;

        // Magnitude bins — compress to 64 log-spaced values for UI performance
        constexpr int UI_BINS = 64;
        juce::String mags = "[";
        int total = (int) s.magnitudes.size();
        for (int i = 0; i < UI_BINS; ++i)
        {
            float t   = (float) i / UI_BINS;
            int   idx = (int) (std::pow (10.f, t * std::log10 ((float) total)) - 1);
            idx = std::max (0, std::min (total - 1, idx));
            float mag = s.magnitudes[static_cast<size_t> (idx)];
            float db  = mag > 0.f ? 20.f * std::log10 (mag) : -80.f;
            float v   = std::max (0.f, std::min (1.f, (db + 80.f) / 80.f));
            if (i > 0) mags += ",";
            mags += juce::String ((int) (v * 1000.f));
        }
        mags += "]";

        // Band boundaries
        juce::String bands = "[";
        for (int b = 0; b < s.bandCount; ++b)
        {
            if (b > 0) bands += ",";
            juce::String loStr = juce::String ((int) s.bandLow  [static_cast<size_t> (b)]);
            juce::String hiStr = juce::String ((int) s.bandHigh [static_cast<size_t> (b)]);
            bands += "{" + Q + "lo" + Q + ":" + loStr
                  + "," + Q + "hi" + Q + ":" + hiStr + "}";
        }
        bands += "]";

        json += "{"   + Q + "id"    + Q + ":" + Q + s.nodeId + Q
              + ","   + Q + "sr"    + Q + ":" + juce::String ((int) s.sampleRate)
              + ","   + Q + "mags"  + Q + ":" + mags
              + ","   + Q + "bands" + Q + ":" + bands
              + "}";
    }
    json += "]";

    pushToUI ("onSpectrumSnapshots", json);
}

void WebBridge::pushAudioPlayerStatus()
{
    if (! connected || ! getAudioPlayerStatuses || webView == nullptr) return;

    auto statuses = getAudioPlayerStatuses();
    if (statuses.empty()) return;

    juce::String json;
    json << "[";
    bool first = true;
    for (const auto& s : statuses)
    {
        if (! first) json << ",";
        first = false;
        json << "{\"nodeId\":\"" << s.nodeId << "\","
             << "\"fraction\":" << juce::String (s.fraction, 4) << ","
             << "\"playing\":" << (s.playing ? "true" : "false") << "}";
    }
    json << "]";

    pushToUI ("onAudioPlayerStatus", json);
}

void WebBridge::pushPendingAudioPlayerFileLoads()
{
    if (! connected || ! getPendingAudioPlayerFileLoads || webView == nullptr) return;

    auto loads = getPendingAudioPlayerFileLoads();
    if (loads.empty()) return;

    // One message per load (a rare event, unlike the batched, per-frame
    // AudioPlayerStatus array) — matches onAudioPlayerLoadFile's own exact
    // JSON shape in PatchyEditor.cpp, reusing the same "onAudioPlayerFileLoaded"
    // event name so the frontend's existing handler needs no changes at all.
    // filePath is deliberately left out — the restore path that leads here
    // already came FROM the frontend's own known path in the first place,
    // so there's nothing new to report there.
    for (const auto& info : loads)
    {
        auto* result = new juce::DynamicObject();
        result->setProperty ("nodeId", info.nodeId);
        result->setProperty ("success", true);
        result->setProperty ("fileName", info.fileName);
        juce::Array<juce::var> peaksArr;
        for (auto& pk : info.peaks)
        {
            peaksArr.add (pk.first);
            peaksArr.add (pk.second);
        }
        result->setProperty ("peaks", peaksArr);
        result->setProperty ("numSamples", info.numSamples);
        result->setProperty ("sourceSampleRate", info.sourceSampleRate);

        pushToUI ("onAudioPlayerFileLoaded", juce::JSON::toString (juce::var (result), true));
    }
}
