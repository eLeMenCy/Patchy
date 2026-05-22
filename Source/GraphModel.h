#pragma once
#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

enum class PortType      { Midi, Audio };
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
    juce::String      addonName;        // empty = built-in, non-empty = dynamic addon
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

    // Viewport state (pan + zoom) — saved/restored with the graph
    float viewportX    = 0.0f;
    float viewportY    = 0.0f;
    float viewportZoom = 1.0f;

    NodeData&   addNode       (int nodeType, float x, float y,
                              const juce::String& addonName = {},
                              int audioIn=0, int audioOut=0,
                              int midiIn=0,  int midiOut=0);

    /** Restore a node with its original saved ID (used by setStateInformation).
     *  Does NOT fire onChange — caller must do that once all nodes+connections
     *  are restored. */
    NodeData&   restoreNode   (const juce::String& savedId,
                               int nodeType, float x, float y,
                               const juce::String& addonName = {},
                               int audioIn=0, int audioOut=0,
                               int midiIn=0,  int midiOut=0);

    /** Temporarily disable onChange notifications (for batch restores). */
    void clear()
    {
        nodes.clear();
        connections.clear();
        nodeCounter = 0;
        viewportX = viewportY = 0.0f;
        viewportZoom = 1.0f;
    }

    void        suspendNotifications()  { notificationsSuspended = true; }
    void        resumeNotifications()   { notificationsSuspended = false; if (onChange) onChange(); }
    bool        removeNode    (const juce::String& nodeId);
    NodeData*   findNode      (const juce::String& nodeId);

    Connection* addConnection (const juce::String& srcNode, const juce::String& srcPort,
                                const juce::String& dstNode, const juce::String& dstPort);
    bool        removeConnection (const juce::String& connId);
    void        renameNode       (const juce::String& nodeId, const juce::String& newLabel);
    void        setNodeSettings  (const juce::String& nodeId, const juce::String& json);

    juce::var   toVar() const;

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
    int  nodeCounter             = 0;
    int  connCounter             = 0;
    bool notificationsSuspended  = false;

};
