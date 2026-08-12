#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>
#include <deque>

enum class PortType      { Midi, Audio, OSC, DMX, MQTT, UDP, Value };
enum class PortDirection { Input, Output };

struct Port
{
    juce::String  id, label;
    PortType      type      = PortType::Midi;
    PortDirection direction = PortDirection::Input;
};

/** Value port type tag for a Pax-declared port — deliberately its own
 *  enum, not PortType directly. ArtNet and plain DMX share the same
 *  PortType::DMX internally (see the built-in ArtNet/DMX nodes) and are
 *  only distinguished by label text ("ArtDMX" vs "DMX") — the frontend's
 *  own connection-type classifier works off that label text, not this
 *  backend enum, so PaxValueType needs to keep ArtNet and DMX distinct
 *  even though they'll resolve to the same PortType once a port is
 *  actually created. Mirrors PAX_VALUETYPE_* in PaxAPI.h 1:1. */
enum class PaxValueType { Generic, Mqtt, Osc, Dmx, Udp, ArtNet, Midi };

/** Translates a PaxValueType + port label into the internal PortType plus
 *  the label prefix that makes the frontend's own (label-text-based)
 *  connection/edge classifiers recognise it correctly. See PaxValueType's
 *  comment above for why ArtNet and DMX share PortType::DMX but need
 *  different label text. */
void portTypeAndLabelFor (PaxValueType t, PortType& outType, juce::String& outLabelPrefix);

/** Reverse direction — determines a Port's PaxValueType from its actual
 *  PortType + label (the ArtDMX-vs-DMX distinction again needs the label,
 *  not just PortType, to tell them apart). Used when serializing a node's
 *  ports (toVar()) back into per-port type tags for undo/redo and fragment
 *  export. */
PaxValueType paxValueTypeFromPort (const Port& p);

/** String tag <-> PaxValueType, used for JSON serialization (toVar()'s
 *  output, and reading it back in restoreSnapshot()/Fragment Import).
 *  Deliberately string tags rather than raw ints, for readability in
 *  saved/exported project JSON. */
juce::String tagForValueType   (PaxValueType t);
PaxValueType  parseValueTypeTag (const juce::String& tag);

/** Bundles a Pax node's full port configuration — counts plus, for Value
 *  ports specifically, each port's individual declared type. Replaces
 *  the long, ever-growing list of individual int parameters that
 *  addNode/restoreNode/portsForType used to take (audioIn, audioOut,
 *  midiIn, midiOut, then valueIn, valueOut added alongside them) — one
 *  more extension on top of that would have made an already-awkward
 *  signature worse, so this consolidates it while the per-port-typing
 *  work is already touching all three functions anyway.
 *
 *  valueInTypes/valueOutTypes: PaxValueType per Value port, in the same
 *  order as the count implies. Empty, or shorter than the count, is
 *  fine — missing entries default to PaxValueType::Generic, which is
 *  exactly the pre-per-port-typing behaviour for any Pax that doesn't
 *  declare per-port types (existing PAX_getValueInputCount/OutputCount-
 *  only Pax, or an index the author didn't populate). */
struct PaxPortSpec
{
    int audioIn = 0, audioOut = 0;
    int midiIn  = 0, midiOut  = 0;
    int valueIn = 0, valueOut = 0;
    std::vector<PaxValueType> valueInTypes;
    std::vector<PaxValueType> valueOutTypes;

    PaxValueType valueInTypeAt (int i) const
    {
        return (i >= 0 && i < (int) valueInTypes.size()) ? valueInTypes[(size_t) i] : PaxValueType::Generic;
    }
    PaxValueType valueOutTypeAt (int i) const
    {
        return (i >= 0 && i < (int) valueOutTypes.size()) ? valueOutTypes[(size_t) i] : PaxValueType::Generic;
    }
};

struct NodeData
{
    juce::String      id, label;
    int               nodeType        = 1;
    juce::String      paxName;        // empty = built-in, non-empty = dynamic Pax
    juce::String      selectedDeviceId;  // for nodeType 4 and 5
    juce::String      settingsJson;      // UI settings blob (JSON string)
    float             x = 100.f, y = 100.f;
    std::vector<Port> ports;
};

struct Connection
{
    juce::String id;
    juce::String sourceNodeId, sourcePortId;
    juce::String targetNodeId, targetPortId;
};

class GraphModel
{
public:
    std::function<void()> onChange;
    /** Called after undo/redo restores a snapshot — allows the processor to
     *  resync device managers from the restored GraphModel state. */
    std::function<void()> onAfterRestore;

    // Viewport state (pan + zoom) — saved/restored with the graph
    float viewportX    = 0.0f;
    float viewportY    = 0.0f;
    float viewportZoom = 1.0f;

    NodeData&   addNode       (int nodeType, float x, float y,
                              const juce::String& paxName = {},
                              const PaxPortSpec& portSpec = {});

    /** Restore a node with its original saved ID (used by setStateInformation).
     *  Does NOT fire onChange — caller must do that once all nodes+connections
     *  are restored. */
    NodeData&   restoreNode   (const juce::String& savedId,
                               int nodeType, float x, float y,
                               const juce::String& paxName = {},
                               const PaxPortSpec& portSpec = {});

    /** Temporarily disable onChange notifications (for batch restores). */
    void clear()
    {
        nodes.clear();
        connections.clear();
        viewportX = viewportY = 0.0f;
        viewportZoom = 1.0f;
    }

    void        suspendNotifications()       { notificationsSuspended = true; }
    void        resumeNotifications()            { notificationsSuspended = false; if (onChange) onChange(); }
    void        resumeNotificationsQuiet()       { notificationsSuspended = false; }  // resumes without firing onChange
    bool        removeNode    (const juce::String& nodeId);
    NodeData*   findNode      (const juce::String& nodeId);

    Connection* addConnection (const juce::String& srcNode, const juce::String& srcPort,
                                const juce::String& dstNode, const juce::String& dstPort);
    bool        removeConnection (const juce::String& connId);
    bool        hasConnection   (const juce::String& connId) const;
    void        renameNode       (const juce::String& nodeId, const juce::String& newLabel);
    void        updateNodeAudioOutputCount (const juce::String& nodeId, int newAudioOut);
    void        setNodeSettings  (const juce::String& nodeId, const juce::String& json);

    juce::var   toVar() const;

    // ── Undo / Redo ───────────────────────────────────────────────────────────
    static constexpr int kMaxUndoSteps = 50;

    /** Call this BEFORE any qualifying mutation to capture the current state. */
    void pushSnapshot();

    /** Push a pre-captured snapshot onto the undo stack directly.
     *  Used by commitNodeSettings to push the pre-drag state. */
    void pushExistingSnapshot (juce::var snapshot)
    {
        undoStack.push_back (std::move (snapshot));
        while ((int) undoStack.size() > kMaxUndoSteps)
            undoStack.pop_front();
        redoStack.clear();
    }

    /** Undo the last action. Returns true if successful. */
    bool undo();

    /** Redo the last undone action. Returns true if successful. */
    bool redo();

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    /** Clear both undo and redo stacks — called on file load. */
    void clearHistory() { undoStack.clear(); redoStack.clear(); }

    // ── Test / read-only accessors ────────────────────────────────────────────
    const std::vector<NodeData>&   getNodes()       const { return nodes; }
    const std::vector<Connection>& getConnections() const { return connections; }
    int getNodeCount()       const { return static_cast<int>(nodes.size()); }
    int getConnectionCount() const { return static_cast<int>(connections.size()); }

    static std::vector<Port> portsForType (int t, const juce::String& nodeId,
                                           const PaxPortSpec& portSpec = {});

private:
    void notifyChange();

    std::vector<NodeData>   nodes;
    std::vector<Connection> connections;

    bool notificationsSuspended  = false;

    // ── History ───────────────────────────────────────────────────────────────
    std::deque<juce::var> undoStack;
    std::deque<juce::var> redoStack;

    void restoreSnapshot (const juce::var& snapshot);

};
