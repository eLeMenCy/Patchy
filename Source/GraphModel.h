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

struct NodeData
{
    juce::String      id, label;
    int               nodeType        = 1;
    juce::String      paxName;        // empty = built-in, non-empty = dynamic addon
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
                              int audioIn=0, int audioOut=0,
                              int midiIn=0,  int midiOut=0);

    /** Restore a node with its original saved ID (used by setStateInformation).
     *  Does NOT fire onChange — caller must do that once all nodes+connections
     *  are restored. */
    NodeData&   restoreNode   (const juce::String& savedId,
                               int nodeType, float x, float y,
                               const juce::String& paxName = {},
                               int audioIn=0, int audioOut=0,
                               int midiIn=0,  int midiOut=0);

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
                                           int audioIn=0, int audioOut=0,
                                           int midiIn=0,  int midiOut=0);

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
