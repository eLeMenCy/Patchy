#include "GraphModel.h"
#include <unordered_set>
#include <algorithm>

std::vector<Port> GraphModel::portsForType (int t, const juce::String& nid,
                                               int audioIn, int audioOut,
                                               int midiIn,  int midiOut)
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
    else if (t >= 100)
    {
        // Dynamic addon node — ports based on NGA nodeType (t - 100)
        // with optional override counts from the descriptor.
        int ngaType = t - 100;

        // Determine effective port counts (descriptor overrides nodeType defaults)
        int effMidiIn   = midiIn   > 0 ? midiIn   : ((ngaType == 1 || ngaType == 3) ? 1 : 0);
        int effMidiOut  = midiOut  > 0 ? midiOut  : ((ngaType == 1 || ngaType == 3) ? 1 : 0);
        int effAudioIn  = audioIn  > 0 ? audioIn  : ((ngaType == 2 || ngaType == 3) ? 1 : 0);
        int effAudioOut = audioOut > 0 ? audioOut : ((ngaType == 2 || ngaType == 3) ? 1 : 0);

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
        default: return "Addon Node";
    }
}

// ── Change notification helper ───────────────────────────────────────────────
void GraphModel::notifyChange()
{
    if (onChange && !notificationsSuspended) onChange();
}

NodeData& GraphModel::addNode (int t, float x, float y, const juce::String& paxName,
                               int audioIn, int audioOut, int midiIn, int midiOut)
{
    NodeData n;
    n.id       = juce::Uuid().toString();
    n.nodeType = t;
    n.x = x; n.y = y;
    n.paxName = paxName;

    n.label = labelForType (t, paxName);
    n.ports    = portsForType (t, n.id, audioIn, audioOut, midiIn, midiOut);
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

        // Count ports by type/direction for restore
        int audioIn = 0, audioOut = 0, midiIn = 0, midiOut = 0;
        for (const auto& p : n.ports)
        {
            bool isAudio = (p.type == PortType::Audio);
            bool isOut   = (p.direction == PortDirection::Output);
            if  (isAudio &&  isOut) ++audioOut;
            else if (isAudio && !isOut) ++audioIn;
            else if (!isAudio &&  isOut) ++midiOut;
            else ++midiIn;
        }
        obj->setProperty ("audioInputs",  audioIn);
        obj->setProperty ("audioOutputs", audioOut);
        obj->setProperty ("midiInputs",   midiIn);
        obj->setProperty ("midiOutputs",  midiOut);

        juce::Array<juce::var> ports;
        for (const auto& p : n.ports)
        {
            auto* po = new juce::DynamicObject();
            po->setProperty ("id",        p.id);
            po->setProperty ("label",     p.label);
            po->setProperty ("type", [&]() -> juce::String {
                switch (p.type) {
                    case PortType::Audio: return "audio";
                    case PortType::OSC:   return "osc";
                    case PortType::DMX:   return "dmx";
                    case PortType::MQTT:  return "mqtt";
                    case PortType::UDP:   return "udp";
                    case PortType::Value: return "value";
                    default:              return "midi";
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
                                    int audioIn, int audioOut, int midiIn, int midiOut)
{
    NodeData n;
    n.id         = savedId;
    n.nodeType   = t;
    n.x = x; n.y = y;
    n.paxName = paxName;
    n.label = labelForType (t, paxName);
    n.ports      = portsForType (t, n.id, audioIn, audioOut, midiIn, midiOut);

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
        for (auto& p : n.ports)
        {
            if (p.type == PortType::Audio && p.direction == PortDirection::Input)  ++audioIn;
            if (p.type == PortType::Midi  && p.direction == PortDirection::Input)  ++midiIn;
            if (p.type == PortType::Midi  && p.direction == PortDirection::Output) ++midiOut;
        }

        // Build new port list so we know which port IDs will exist
        auto newPorts = portsForType (n.nodeType, n.id, audioIn, newAudioOut, midiIn, midiOut);
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

            int audioIn  = (int) nObj->getProperty ("audioInputs");
            int audioOut = (int) nObj->getProperty ("audioOutputs");
            int midiIn   = (int) nObj->getProperty ("midiInputs");
            int midiOut  = (int) nObj->getProperty ("midiOutputs");

            auto& nd = restoreNode (
                nObj->getProperty ("id").toString(),
                (int) nObj->getProperty ("nodeType"),
                (float)(double) nObj->getProperty ("x"),
                (float)(double) nObj->getProperty ("y"),
                nObj->getProperty ("paxName").toString(),
                audioIn, audioOut, midiIn, midiOut);

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

    // Notify processor to resync device managers from restored model
    for (const auto& n : nodes)

    // Update device manager selections BEFORE rebuild so applyDeviceSelections
    // inside rebuildProcessingGraph picks up the correct restored values.
    if (onAfterRestore) onAfterRestore();

    resumeNotifications();  // fires onChange → rebuild + pushGraphToUI
}