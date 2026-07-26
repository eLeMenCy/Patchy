#include "GraphModel.h"
#include <unordered_set>
#include <algorithm>

/** Translates a Pax-declared per-port value type into the internal
 *  PortType plus the label prefix that makes the frontend's own
 *  (label-text-based) connection/edge classifiers recognise it
 *  correctly. See PaxValueType's comment in GraphModel.h for why
 *  ArtNet and DMX share PortType::DMX but need different label text. */
void portTypeAndLabelFor (PaxValueType t, PortType& outType, juce::String& outLabelPrefix)
{
    switch (t)
    {
        case PaxValueType::Mqtt:    outType = PortType::MQTT;  outLabelPrefix = "MQTT";   break;
        case PaxValueType::Osc:     outType = PortType::OSC;   outLabelPrefix = "OSC";    break;
        case PaxValueType::Dmx:     outType = PortType::DMX;   outLabelPrefix = "DMX";    break;
        case PaxValueType::Udp:     outType = PortType::UDP;   outLabelPrefix = "UDP";    break;
        case PaxValueType::ArtNet:  outType = PortType::DMX;   outLabelPrefix = "ArtDMX"; break;
        case PaxValueType::Midi:    outType = PortType::Midi;  outLabelPrefix = "MIDI";   break;
        case PaxValueType::Generic:
        default:                   outType = PortType::Value; outLabelPrefix = "Value";  break;
    }
}

PaxValueType paxValueTypeFromPort (const Port& p)
{
    if (p.type == PortType::MQTT) return PaxValueType::Mqtt;
    if (p.type == PortType::OSC)  return PaxValueType::Osc;
    if (p.type == PortType::UDP)  return PaxValueType::Udp;
    if (p.type == PortType::DMX)  return p.label.startsWith ("ArtDMX") ? PaxValueType::ArtNet : PaxValueType::Dmx;
    if (p.type == PortType::Midi) return PaxValueType::Midi;
    return PaxValueType::Generic;
}

juce::String tagForValueType (PaxValueType t)
{
    switch (t)
    {
        case PaxValueType::Mqtt:   return "mqtt";
        case PaxValueType::Osc:    return "osc";
        case PaxValueType::Dmx:    return "dmx";
        case PaxValueType::Udp:    return "udp";
        case PaxValueType::ArtNet: return "artnet";
        case PaxValueType::Midi:   return "midi";
        case PaxValueType::Generic:
        default:                  return "generic";
    }
}

/** Parses a value-type tag string (as written by toVar()'s tagForValueType)
 *  back into a PaxValueType — used by restoreSnapshot() to reconstruct
 *  exactly what a Pax's per-port types were at the point a snapshot was
 *  taken, rather than re-querying the (possibly since-changed) registry. */
PaxValueType parseValueTypeTag (const juce::String& tag)
{
    if (tag == "mqtt")   return PaxValueType::Mqtt;
    if (tag == "osc")    return PaxValueType::Osc;
    if (tag == "dmx")    return PaxValueType::Dmx;
    if (tag == "udp")    return PaxValueType::Udp;
    if (tag == "artnet") return PaxValueType::ArtNet;
    if (tag == "midi")   return PaxValueType::Midi;
    return PaxValueType::Generic;
}

std::vector<Port> GraphModel::portsForType (int t, const juce::String& nid,
                                               const PaxPortSpec& portSpec)
{
    std::vector<Port> p;
    auto mk = [&](const char* lbl, PortType pt, PortDirection dir)
    {
        Port port;
        port.id        = nid + "_" + lbl + "_" + (dir == PortDirection::Input ? "in" : "out");
        port.label     = lbl;
        port.type      = pt;
        port.direction = dir;
        p.push_back (port);
    };

    if (t == 1)
    {
        // MidiInDeviceNode: 1 MIDI output (feeds downstream graph nodes)
        mk ("MIDI Out", PortType::Midi,  PortDirection::Output);
    }
    else if (t == 2)
    {
        // MidiOutDeviceNode: 1 MIDI input (receives from upstream graph nodes)
        mk ("MIDI In",  PortType::Midi,  PortDirection::Input);
    }
    else if (t == 3)
    {
        // AudioInDeviceNode: 1 stereo Audio output
        mk ("Audio Out", PortType::Audio, PortDirection::Output);
    }
    else if (t == 4)
    {
        // AudioOutDeviceNode: 1 stereo Audio input
        mk ("Audio In",  PortType::Audio, PortDirection::Input);
    }
    else if (t == 5)
    {
        mk ("MIDI In",   PortType::Midi,  PortDirection::Input);
        mk ("MIDI Out",  PortType::Midi,  PortDirection::Output);
    }
    else if (t == 6)
    {
        mk ("Audio In",  PortType::Audio, PortDirection::Input);
        mk ("Audio Out", PortType::Audio, PortDirection::Output);
    }
    else if (t == 7)
    {
        // MidiKeyboardNode: MIDI In (optional) + MIDI Out
        mk ("MIDI In",  PortType::Midi, PortDirection::Input);
        mk ("MIDI Out", PortType::Midi, PortDirection::Output);
    }
    else if (t == 8)
    {
        // UdpInDeviceNode: 1 UDP output (feeds downstream graph nodes)
        mk ("UDP Out", PortType::UDP, PortDirection::Output);
    }
    else if (t == 9)
    {
        // UdpOutDeviceNode: 1 UDP input (receives from upstream graph nodes)
        mk ("UDP In",  PortType::UDP, PortDirection::Input);
    }
    else if (t == 10)
    {
        // OscInDeviceNode: 1 OSC output (parsed OSC messages as PAX_Value)
        mk ("OSC Out", PortType::OSC, PortDirection::Output);
    }
    else if (t == 11)
    {
        // OscOutDeviceNode: 1 OSC input (serialises PAX_Value back to OSC)
        mk ("OSC In",  PortType::OSC, PortDirection::Input);
    }
    else if (t == 12)
    {
        // ArtNetInDeviceNode: 1 DMX output (parsed ArtDmx universe as PAX_Value blob)
        mk ("ArtDMX Out", PortType::DMX, PortDirection::Output);
    }
    else if (t == 13)
    {
        // ArtNetOutDeviceNode: 1 DMX input (sends PAX_Value blob as ArtDmx)
        mk ("ArtDMX In",  PortType::DMX, PortDirection::Input);
    }
    else if (t == 14)
    {
        // DmxInDeviceNode: 1 DMX output (received universe from Enttec Pro)
        mk ("DMX Out", PortType::DMX, PortDirection::Output);
    }
    else if (t == 15)
    {
        // DmxOutDeviceNode: 1 DMX input (sends PAX_Value blob to Enttec Pro)
        mk ("DMX In",  PortType::DMX, PortDirection::Input);
    }
    else if (t == 16)
    {
        // DmxMonitorNode: DMX In + DMX Out (pass-through, display only)
        mk ("DMX In",  PortType::DMX, PortDirection::Input);
        mk ("DMX Out", PortType::DMX, PortDirection::Output);
    }
    else if (t == 17)
    {
        // DmxConsoleNode: DMX Out only — Console is a source, not a processor
        mk ("DMX Out", PortType::DMX, PortDirection::Output);
    }
    else if (t == 18)
    {
        // ArtNetMonitorNode: ArtDMX In + ArtDMX Out (pass-through, display only)
        mk ("ArtDMX In",  PortType::DMX, PortDirection::Input);
        mk ("ArtDMX Out", PortType::DMX, PortDirection::Output);
    }
    else if (t == 19)
    {
        // ArtNetConsoleNode: ArtDMX Out only — Console is a source, not a processor
        mk ("ArtDMX Out", PortType::DMX, PortDirection::Output);
    }
    else if (t == 20)
    {
        // OscMonitorNode: OSC In + OSC Out (pass-through, display only)
        mk ("OSC In",  PortType::OSC, PortDirection::Input);
        mk ("OSC Out", PortType::OSC, PortDirection::Output);
    }
    else if (t == 21)
    {
        // UdpMonitorNode: UDP In + UDP Out (pass-through, display only)
        mk ("UDP In",  PortType::UDP, PortDirection::Input);
        mk ("UDP Out", PortType::UDP, PortDirection::Output);
    }
    else if (t == 22)
    {
        // MqttSubscribeNode: MQTT Out only — subscribes to a broker topic,
        // no input (source node, same shape as a Console)
        mk ("MQTT Out", PortType::MQTT, PortDirection::Output);
    }
    else if (t == 23)
    {
        // MqttPublishNode: MQTT In only — publishes to a broker topic,
        // no output (sink node)
        mk ("MQTT In", PortType::MQTT, PortDirection::Input);
    }
    else if (t == 24)
    {
        // MqttMonitorNode: MQTT In + MQTT Out (pass-through, display only)
        mk ("MQTT In",  PortType::MQTT, PortDirection::Input);
        mk ("MQTT Out", PortType::MQTT, PortDirection::Output);
    }
    else if (t == 25)
    {
        // MqttConsoleNode: MQTT Out only — manual topic+payload composer,
        // source node, same shape as DmxConsoleNode/ArtNetConsoleNode
        mk ("MQTT Out", PortType::MQTT, PortDirection::Output);
    }
    else if (t >= 100)
    {
        // Dynamic addon node — ports based on NGA nodeType (t - 100)
        // with optional override counts from the descriptor.
        int ngaType = t - 100;

        // Determine effective port counts (descriptor overrides nodeType defaults)
        int effMidiIn   = portSpec.midiIn   > 0 ? portSpec.midiIn   : ((ngaType == 1 || ngaType == 3) ? 1 : 0);
        int effMidiOut  = portSpec.midiOut  > 0 ? portSpec.midiOut  : ((ngaType == 1 || ngaType == 3) ? 1 : 0);
        int effAudioIn  = portSpec.audioIn  > 0 ? portSpec.audioIn  : ((ngaType == 2 || ngaType == 3) ? 1 : 0);
        int effAudioOut = portSpec.audioOut > 0 ? portSpec.audioOut : ((ngaType == 2 || ngaType == 3) ? 1 : 0);
        // Value ports have no nodeType-implied default (unlike audio/midi
        // above) — nodeType 4 (Value only) carries no audio/MIDI default
        // either, so a Value-only Pax needs valueInputs/valueOutputs set
        // via PAX_getValueInputCount/PAX_getValueOutputCount to get any
        // ports at all. Purely override-driven, 0 is a valid "no value
        // ports" default for any nodeType.
        int effValueIn  = portSpec.valueIn;
        int effValueOut = portSpec.valueOut;

        for (int i = 0; i < effMidiIn;   ++i)
            mk (effMidiIn  == 1 ? "MIDI In"  : ("MIDI In "  + juce::String(i+1)).toRawUTF8(),
                PortType::Midi,  PortDirection::Input);
        for (int i = 0; i < effMidiOut;  ++i)
            mk (effMidiOut == 1 ? "MIDI Out" : ("MIDI Out " + juce::String(i+1)).toRawUTF8(),
                PortType::Midi,  PortDirection::Output);
        for (int i = 0; i < effAudioIn;  ++i)
            mk (effAudioIn  == 1 ? "Audio In"  : ("Audio In "  + juce::String(i+1)).toRawUTF8(),
                PortType::Audio, PortDirection::Input);
        for (int i = 0; i < effAudioOut; ++i)
            mk (effAudioOut == 1 ? "Audio Out" : ("Audio Out " + juce::String(i+1)).toRawUTF8(),
                PortType::Audio, PortDirection::Output);

        // Value ports — each one individually typed via portSpec, not a
        // single blanket "Value In"/"Value Out" like before per-port
        // typing existed. A Pax that only declares counts (no per-port
        // types) still gets plain generic "Value In"/"Value Out" ports,
        // identical to the original behaviour.
        for (int i = 0; i < effValueIn;  ++i)
        {
            PortType pt; juce::String prefix;
            portTypeAndLabelFor (portSpec.valueInTypeAt (i), pt, prefix);
            juce::String label = prefix + " In";
            // Count same-typed ports so far to number duplicates correctly
            // (e.g. two MQTT-typed inputs → "MQTT In", "MQTT In 2").
            int sameTypeCount = 0;
            for (int j = 0; j <= i; ++j)
            {
                PortType pt2; juce::String prefix2;
                portTypeAndLabelFor (portSpec.valueInTypeAt (j), pt2, prefix2);
                if (prefix2 == prefix) ++sameTypeCount;
            }
            if (sameTypeCount > 1) label << " " << sameTypeCount;
            mk (label.toRawUTF8(), pt, PortDirection::Input);
        }
        for (int i = 0; i < effValueOut; ++i)
        {
            PortType pt; juce::String prefix;
            portTypeAndLabelFor (portSpec.valueOutTypeAt (i), pt, prefix);
            juce::String label = prefix + " Out";
            int sameTypeCount = 0;
            for (int j = 0; j <= i; ++j)
            {
                PortType pt2; juce::String prefix2;
                portTypeAndLabelFor (portSpec.valueOutTypeAt (j), pt2, prefix2);
                if (prefix2 == prefix) ++sameTypeCount;
            }
            if (sameTypeCount > 1) label << " " << sameTypeCount;
            mk (label.toRawUTF8(), pt, PortDirection::Output);
        }
    }
    return p;
}

// ── Label helper — maps nodeType + paxName to a display label ──────────────
static juce::String labelForType (int t, const juce::String& paxName)
{
    if (paxName.isNotEmpty()) return paxName;
    switch (t)
    {
        case 1:  return "MIDI In Device";
        case 2:  return "MIDI Out Device";
        case 3:  return "Audio In Device";
        case 4:  return "Audio Out Device";
        case 5:  return "MIDI Monitor";
        case 6:  return "Audio Monitor";
        case 7:  return "MIDI Keyboard";
        case 8:  return "UDP In";
        case 9:  return "UDP Out";
        case 10: return "OSC In";
        case 11: return "OSC Out";
        case 12: return "ArtNet In";
        case 13: return "ArtNet Out";
        case 14: return "DMX In";
        case 15: return "DMX Out";
        case 16: return "DMX Monitor";
        case 17: return "DMX Console";
        case 18: return "ArtNet Monitor";
        case 19: return "ArtNet Console";
        case 20: return "OSC Monitor";
        case 21: return "UDP Monitor";
        case 22: return "MQTT Subscribe";
        case 23: return "MQTT Publish";
        case 24: return "MQTT Monitor";
        case 25: return "MQTT Console";
        default: return "Addon Node";
    }
}

// ── Change notification helper ───────────────────────────────────────────────
void GraphModel::notifyChange()
{
    if (onChange && !notificationsSuspended) onChange();
}

NodeData& GraphModel::addNode (int t, float x, float y, const juce::String& paxName,
                               const PaxPortSpec& portSpec)
{
    NodeData n;
    n.id       = juce::Uuid().toString();
    n.nodeType = t;
    n.x = x; n.y = y;
    n.paxName = paxName;

    n.label = labelForType (t, paxName);
    n.ports    = portsForType (t, n.id, portSpec);
    nodes.push_back (std::move (n));
    notifyChange();
    return nodes.back();
}

bool GraphModel::removeNode (const juce::String& id)
{
    connections.erase (
        std::remove_if (connections.begin(), connections.end(),
            [&](const Connection& c){ return c.sourceNodeId == id || c.targetNodeId == id; }),
        connections.end());

    auto it = std::remove_if (nodes.begin(), nodes.end(),
        [&](const NodeData& n){ return n.id == id; });
    if (it == nodes.end()) return false;
    nodes.erase (it, nodes.end());
    notifyChange();
    return true;
}

NodeData* GraphModel::findNode (const juce::String& id)
{
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

Connection* GraphModel::addConnection (const juce::String& sn, const juce::String& sp,
                                        const juce::String& tn, const juce::String& tp)
{
    for (auto& c : connections)
        if (c.sourceNodeId == sn && c.sourcePortId == sp &&
            c.targetNodeId == tn && c.targetPortId == tp)
            return &c;

    Connection c;
    c.id           = juce::Uuid().toString();
    c.sourceNodeId = sn; c.sourcePortId = sp;
    c.targetNodeId = tn; c.targetPortId = tp;
    connections.push_back (c);
    notifyChange();
    return &connections.back();
}

bool GraphModel::removeConnection (const juce::String& id)
{
    auto it = std::remove_if (connections.begin(), connections.end(),
        [&](const Connection& c){ return c.id == id; });
    if (it == connections.end()) return false;
    connections.erase (it, connections.end());
    notifyChange();
    return true;
}

bool GraphModel::hasConnection (const juce::String& id) const
{
    for (const auto& c : connections)
        if (c.id == id) return true;
    return false;
}

juce::var GraphModel::toVar() const
{
    juce::Array<juce::var> nodesArr;
    for (const auto& n : nodes)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id",       n.id);
        obj->setProperty ("label",    n.label);
        obj->setProperty ("nodeType",   n.nodeType);
        obj->setProperty ("paxName",        n.paxName);
        obj->setProperty ("selectedDeviceId", n.selectedDeviceId);
        obj->setProperty ("settingsJson",     n.settingsJson);
        obj->setProperty ("x",        n.x);
        obj->setProperty ("y",        n.y);

        // Count ports by type/direction for restore. OSC/DMX/MQTT/UDP are
        // "value-ish" here alongside generic Value — a Pax's per-port-typed
        // value ports (new mechanism) use these same PortTypes, so they
        // need to be counted as value ports, not folded into "midi", or a
        // Pax with e.g. an OSC-typed value port would restore with a
        // phantom MIDI port instead. Harmless for built-in protocol nodes
        // (OSC/DMX/MQTT/UDP In/Out etc.) — their portsForType() branches
        // are hardcoded per-nodeType and never consult these aggregate
        // counts during restore, same as before this change.
        //
        // Deliberately NOT including PortType::Midi in "value-ish" here —
        // a MIDI-typed value port (PaxValueType::Midi) is indistinguishable
        // from a legacy MIDI port at this level, so it folds back into the
        // legacy midiIn/midiOut count instead. Harmless: portsForType()'s
        // t>=100 branch produces the identical resulting port list (same
        // count, same "MIDI In"/"MIDI In 2" labels) whichever path
        // reconstructs it — no existing Pax combines the two mechanisms
        // for MIDI specifically, so this never actually diverges in
        // practice. Same reasoning as updateNodeAudioOutputCount() above.
        int audioIn = 0, audioOut = 0, midiIn = 0, midiOut = 0, valueIn = 0, valueOut = 0;
        juce::Array<juce::var> valueInTypes, valueOutTypes;
        for (const auto& p : n.ports)
        {
            bool isOut = (p.direction == PortDirection::Output);
            bool isValueLike = (p.type == PortType::Value || p.type == PortType::OSC ||
                                 p.type == PortType::DMX   || p.type == PortType::MQTT ||
                                 p.type == PortType::UDP);
            if (p.type == PortType::Audio)
            {
                if (isOut) ++audioOut; else ++audioIn;
            }
            else if (isValueLike)
            {
                auto tag = tagForValueType (paxValueTypeFromPort (p));
                if (isOut) { ++valueOut; valueOutTypes.add (tag); }
                else       { ++valueIn;  valueInTypes.add  (tag); }
            }
            else { if (isOut) ++midiOut; else ++midiIn; }
        }
        obj->setProperty ("audioInputs",  audioIn);
        obj->setProperty ("audioOutputs", audioOut);
        obj->setProperty ("midiInputs",   midiIn);
        obj->setProperty ("midiOutputs",  midiOut);
        obj->setProperty ("valueInputs",  valueIn);
        obj->setProperty ("valueOutputs", valueOut);
        obj->setProperty ("valueInputTypes",  valueInTypes);
        obj->setProperty ("valueOutputTypes", valueOutTypes);

        juce::Array<juce::var> ports;
        for (const auto& p : n.ports)
        {
            auto* po = new juce::DynamicObject();
            po->setProperty ("id",        p.id);
            po->setProperty ("label",     p.label);
            po->setProperty ("type", [&]() -> juce::String {
                switch (p.type) {
                    case PortType::Midi:  return "midi";
                    case PortType::Audio: return "audio";
                    case PortType::OSC:   return "osc";
                    case PortType::DMX:   return "dmx";
                    case PortType::MQTT:  return "mqtt";
                    case PortType::UDP:   return "udp";
                    case PortType::Value: return "value";
                    default:              return "unknown";
                }
            }());
            po->setProperty ("direction", p.direction == PortDirection::Input ? "input" : "output");
            ports.add (po);
        }
        obj->setProperty ("ports", ports);
        nodesArr.add (obj);
    }

    juce::Array<juce::var> connsArr;
    for (const auto& c : connections)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id",           c.id);
        obj->setProperty ("sourceNodeId", c.sourceNodeId);
        obj->setProperty ("sourcePortId", c.sourcePortId);
        obj->setProperty ("targetNodeId", c.targetNodeId);
        obj->setProperty ("targetPortId", c.targetPortId);
        connsArr.add (obj);
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("nodes",         nodesArr);
    root->setProperty ("connections",   connsArr);
    root->setProperty ("viewportX",     viewportX);
    root->setProperty ("viewportY",     viewportY);
    root->setProperty ("viewportZoom",  viewportZoom);
    return root;
}

NodeData& GraphModel::restoreNode (const juce::String& savedId,
                                    int t, float x, float y,
                                    const juce::String& paxName,
                                    const PaxPortSpec& portSpec)
{
    NodeData n;
    n.id         = savedId;
    n.nodeType   = t;
    n.x = x; n.y = y;
    n.paxName = paxName;
    n.label = labelForType (t, paxName);
    n.ports      = portsForType (t, n.id, portSpec);

    nodes.push_back (std::move (n));
    // onChange intentionally not fired here — caller uses resumeNotifications()
    return nodes.back();
}

void GraphModel::renameNode (const juce::String& nodeId, const juce::String& newLabel)
{
    for (auto& n : nodes)
    {
        if (n.id == nodeId)
        {
            n.label = newLabel;
            notifyChange();
            return;
        }
    }
}

void GraphModel::setNodeSettings (const juce::String& nodeId, const juce::String& json)
{
    for (auto& n : nodes)
        if (n.id == nodeId) { n.settingsJson = json; return; }
}

void GraphModel::updateNodeAudioOutputCount (const juce::String& nodeId, int newAudioOut)
{
    for (auto& n : nodes)
    {
        if (n.id != nodeId) continue;

        // Collect valid port IDs after the update
        int audioIn = 0, midiIn = 0, midiOut = 0;
        PaxPortSpec spec;
        for (auto& p : n.ports)
        {
            if (p.type == PortType::Audio && p.direction == PortDirection::Input)  ++audioIn;
            if (p.type == PortType::Midi  && p.direction == PortDirection::Input)  ++midiIn;
            if (p.type == PortType::Midi  && p.direction == PortDirection::Output) ++midiOut;

            // Value ports: preserve each one's actual type, not just a count.
            // MIDI-typed value ports (via the newer per-port mechanism) are
            // a narrow exception — indistinguishable here from a legacy
            // MIDI port sharing the same PortType::Midi, so they fold back
            // into the legacy midiIn/midiOut count above instead. Only
            // matters for a Pax combining dynamic audio bands *and*
            // MIDI-typed value ports at once — no existing Pax does both.
            bool isValueLike = (p.type == PortType::Value || p.type == PortType::OSC ||
                                 p.type == PortType::DMX   || p.type == PortType::MQTT ||
                                 p.type == PortType::UDP);
            if (isValueLike && p.direction == PortDirection::Input)
            {
                spec.valueInTypes.push_back (paxValueTypeFromPort (p));
                ++spec.valueIn;
            }
            if (isValueLike && p.direction == PortDirection::Output)
            {
                spec.valueOutTypes.push_back (paxValueTypeFromPort (p));
                ++spec.valueOut;
            }
        }
        spec.audioIn = audioIn; spec.audioOut = newAudioOut;
        spec.midiIn  = midiIn;  spec.midiOut  = midiOut;

        // Build new port list so we know which port IDs will exist
        auto newPorts = portsForType (n.nodeType, n.id, spec);
        std::unordered_set<juce::String> validPortIds;
        for (auto& p : newPorts) validPortIds.insert (p.id);

        // Remove connections to/from ports that no longer exist
        connections.erase (
            std::remove_if (connections.begin(), connections.end(),
                [&] (const Connection& c)
                {
                    // Check if this connection involves our node
                    bool srcIsUs = (c.sourceNodeId == nodeId);
                    bool dstIsUs = (c.targetNodeId == nodeId);
                    if (! srcIsUs && ! dstIsUs) return false;
                    // Remove if the port no longer exists
                    if (srcIsUs && validPortIds.find (c.sourcePortId) == validPortIds.end()) return true;
                    if (dstIsUs && validPortIds.find (c.targetPortId) == validPortIds.end()) return true;
                    return false;
                }),
            connections.end()
        );

        n.ports = std::move (newPorts);
        if (! notificationsSuspended) notifyChange();
        return;
    }
}

// ── Undo / Redo ───────────────────────────────────────────────────────────────

void GraphModel::pushSnapshot()
{
    undoStack.push_back (toVar());

    // Trim to max steps
    while ((int) undoStack.size() > kMaxUndoSteps)
        undoStack.pop_front();

    // Any new action clears the redo stack
    redoStack.clear();
}

bool GraphModel::undo()
{
    if (undoStack.empty()) return false;

    // Push current state onto redo stack before restoring
    redoStack.push_back (toVar());

    auto snapshot = std::move (undoStack.back());
    undoStack.pop_back();

    restoreSnapshot (snapshot);
    return true;
}

bool GraphModel::redo()
{
    if (redoStack.empty()) return false;

    // Push current state onto undo stack before restoring
    undoStack.push_back (toVar());

    auto snapshot = std::move (redoStack.back());
    redoStack.pop_back();

    restoreSnapshot (snapshot);
    return true;
}

void GraphModel::restoreSnapshot (const juce::var& snapshot)
{
    auto* obj = snapshot.getDynamicObject();
    if (obj == nullptr) return;

    suspendNotifications();

    clear();

    viewportX    = (float)(double) obj->getProperty ("viewportX");
    viewportY    = (float)(double) obj->getProperty ("viewportY");
    viewportZoom = (float)(double) obj->getProperty ("viewportZoom");
    if (viewportZoom == 0.0f) viewportZoom = 1.0f;

    // Restore nodes
    auto* nodesArr = obj->getProperty ("nodes").getArray();
    if (nodesArr)
    {
        for (auto& n : *nodesArr)
        {
            auto* nObj = n.getDynamicObject();
            if (! nObj) continue;

            PaxPortSpec spec;
            spec.audioIn  = (int) nObj->getProperty ("audioInputs");
            spec.audioOut = (int) nObj->getProperty ("audioOutputs");
            spec.midiIn   = (int) nObj->getProperty ("midiInputs");
            spec.midiOut  = (int) nObj->getProperty ("midiOutputs");
            spec.valueIn  = (int) nObj->getProperty ("valueInputs");
            spec.valueOut = (int) nObj->getProperty ("valueOutputs");
            if (auto* arr = nObj->getProperty ("valueInputTypes").getArray())
                for (auto& v : *arr) spec.valueInTypes.push_back (parseValueTypeTag (v.toString()));
            if (auto* arr = nObj->getProperty ("valueOutputTypes").getArray())
                for (auto& v : *arr) spec.valueOutTypes.push_back (parseValueTypeTag (v.toString()));

            auto& nd = restoreNode (
                nObj->getProperty ("id").toString(),
                (int) nObj->getProperty ("nodeType"),
                (float)(double) nObj->getProperty ("x"),
                (float)(double) nObj->getProperty ("y"),
                nObj->getProperty ("paxName").toString(),
                spec);

            // Always restore selectedDeviceId — empty string means "no device"
            nd.selectedDeviceId = nObj->getProperty ("selectedDeviceId").toString();

            nd.settingsJson = nObj->getProperty ("settingsJson").toString();
            nd.label        = nObj->getProperty ("label").toString();
        }
    }

    // Restore connections
    auto* connsArr = obj->getProperty ("connections").getArray();
    if (connsArr)
    {
        for (auto& c : *connsArr)
        {
            auto* cObj = c.getDynamicObject();
            if (! cObj) continue;
            addConnection (
                cObj->getProperty ("sourceNodeId").toString(),
                cObj->getProperty ("sourcePortId").toString(),
                cObj->getProperty ("targetNodeId").toString(),
                cObj->getProperty ("targetPortId").toString());
        }
    }

    // Update device manager selections BEFORE rebuild so applyDeviceSelections
    // inside rebuildProcessingGraph picks up the correct restored values.
    if (onAfterRestore) onAfterRestore();

    resumeNotifications();  // fires onChange → rebuild + pushGraphToUI
}