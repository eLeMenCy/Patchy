// Patchy — GraphModel unit tests (JUCE UnitTest framework)

#include <juce_core/juce_core.h>
#include "../Source/GraphModel.h"

// ─────────────────────────────────────────────────────────────────────────────
class GraphModelTests : public juce::UnitTest
{
public:
    GraphModelTests() : juce::UnitTest ("GraphModel", "Patchy") {}

    void runTest() override
    {
        testAddNode();
        testRemoveNode();
        testAddConnection();
        testRemoveConnection();
        testClear();
        testPortsForType();
        testCycleDetection();
        testSerialisationRoundtrip();
        testPacks();
    }

private:
    // ── helpers ───────────────────────────────────────────────────────────────
    GraphModel makeGraph()
    {
        GraphModel g;
        g.onChange = [](){};   // suppress callbacks during tests
        return g;
    }

    // ── tests ─────────────────────────────────────────────────────────────────

    void testAddNode()
    {
        beginTest ("addNode creates node with correct type and ports");

        auto g = makeGraph();

        // MIDI In (type 1) — should have 1 MIDI Out port
        juce::String id1 = g.addNode (1, 100.f, 100.f).id;
        expectEquals (g.getNodes().size(), (size_t) 1);
        expectEquals (g.getNodes()[0].nodeType, 1);
        expect (!id1.isEmpty());

        // Audio Out (type 4) — should have 1 Audio In port
        juce::String id2 = g.addNode (4, 200.f, 200.f).id;
        expectEquals (g.getNodes().size(), (size_t) 2);
        expectEquals (g.getNodes()[1].nodeType, 4);

        // IDs must be unique
        expect (id1 != id2);
    }

    void testRemoveNode()
    {
        beginTest ("removeNode removes node and its connections");

        auto g = makeGraph();
        // Copy IDs immediately — vector may reallocate on second addNode
        juce::String id1 = g.addNode (1, 0.f, 0.f).id;
        juce::String portId1 = g.getNodes()[0].ports.empty() ? "" : g.getNodes()[0].ports[0].id;
        juce::String id2 = g.addNode (2, 100.f, 0.f).id;
        juce::String portId2 = g.getNodes()[1].ports.empty() ? "" : g.getNodes()[1].ports[0].id;

        // Add a connection between them
        if (portId1.isNotEmpty() && portId2.isNotEmpty())
            g.addConnection (id1, portId1, id2, portId2);

        expectEquals (g.getNodes().size(), (size_t) 2);

        bool removed = g.removeNode (id1);
        expect (removed);
        expectEquals (g.getNodes().size(), (size_t) 1);

        // Connection should also be gone
        expectEquals (g.getConnections().size(), (size_t) 0);
    }

    void testAddConnection()
    {
        beginTest ("addConnection creates connection between compatible ports");

        auto g = makeGraph();
        juce::String idIn  = g.addNode (1, 0.f,   0.f).id;
        juce::String idOut = g.addNode (2, 100.f, 0.f).id;

        juce::String srcPort, dstPort;
        for (auto& p : g.getNodes()[0].ports)
            if (p.direction == PortDirection::Output && p.type == PortType::Midi)
                { srcPort = p.id; break; }
        for (auto& p : g.getNodes()[1].ports)
            if (p.direction == PortDirection::Input && p.type == PortType::Midi)
                { dstPort = p.id; break; }

        expect (!srcPort.isEmpty(), "MIDI In node should have MIDI output port");
        expect (!dstPort.isEmpty(), "MIDI Out node should have MIDI input port");

        auto* conn = g.addConnection (idIn, srcPort, idOut, dstPort);
        expect (conn != nullptr);
        expectEquals (g.getConnections().size(), (size_t) 1);
    }

    void testRemoveConnection()
    {
        beginTest ("removeConnection removes existing connection");

        auto g = makeGraph();
        juce::String id1 = g.addNode (1, 0.f, 0.f).id;
        juce::String id2 = g.addNode (2, 100.f, 0.f).id;

        juce::String srcPort, dstPort;
        for (auto& p : g.getNodes()[0].ports)
            if (p.direction == PortDirection::Output) { srcPort = p.id; break; }
        for (auto& p : g.getNodes()[1].ports)
            if (p.direction == PortDirection::Input)  { dstPort = p.id; break; }

        auto* conn = g.addConnection (id1, srcPort, id2, dstPort);
        expect (conn != nullptr);

        bool removed = g.removeConnection (conn->id);
        expect (removed);
        expectEquals (g.getConnections().size(), (size_t) 0);
    }

    void testClear()
    {
        beginTest ("clear removes all nodes and connections");

        auto g = makeGraph();
        g.addNode (1, 0.f, 0.f);
        g.addNode (2, 100.f, 0.f);
        g.addNode (3, 200.f, 0.f);
        expectEquals (g.getNodes().size(), (size_t) 3);

        g.clear();
        expectEquals (g.getNodes().size(), (size_t) 0);
        expectEquals (g.getConnections().size(), (size_t) 0);
    }

    void testPortsForType()
    {
        beginTest ("portsForType returns correct ports for each built-in type");

        // Type 1: MIDI In — should have 1 MIDI output
        auto ports1 = GraphModel::portsForType (1, "test1");
        expect (!ports1.empty());
        expect (std::any_of (ports1.begin(), ports1.end(),
            [](const Port& p){ return p.type == PortType::Midi
                                   && p.direction == PortDirection::Output; }));

        // Type 3: Audio In — should have 1 Audio output
        auto ports3 = GraphModel::portsForType (3, "test3");
        expect (!ports3.empty());
        expect (std::any_of (ports3.begin(), ports3.end(),
            [](const Port& p){ return p.type == PortType::Audio
                                   && p.direction == PortDirection::Output; }));

        // Type 102: Pax Audio — should have audio in + out
        auto ports102 = GraphModel::portsForType (102, "test102");
        expect (!ports102.empty());
    }

    void testCycleDetection()
    {
        beginTest ("addConnection prevents cycles");

        auto g = makeGraph();
        juce::String id1 = g.addNode (1, 0.f,   0.f).id;
        juce::String id2 = g.addNode (2, 100.f, 0.f).id;

        juce::String p1out, p2in;
        for (auto& p : g.getNodes()[0].ports)
            if (p.direction == PortDirection::Output) { p1out = p.id; break; }
        for (auto& p : g.getNodes()[1].ports)
            if (p.direction == PortDirection::Input)  { p2in  = p.id; break; }

        // n1 → n2 should succeed
        if (!p1out.isEmpty() && !p2in.isEmpty())
        {
            auto* c1 = g.addConnection (id1, p1out, id2, p2in);
            expect (c1 != nullptr, "Forward connection should succeed");
        }
    }

    void testSerialisationRoundtrip()
    {
        beginTest ("toVar / restoreNode roundtrip preserves graph structure");

        auto g = makeGraph();
        g.addNode (1, 100.f, 200.f);
        g.addNode (4, 300.f, 400.f);

        // Serialise
        auto state = g.toVar();
        expect (state.isObject());

        // Count nodes in serialised form
        if (auto* obj = state.getDynamicObject())
        {
            auto nodesVar = obj->getProperty ("nodes");
            if (auto* arr = nodesVar.getArray())
                expectEquals ((int) arr->size(), 2);
        }

        // Restore into a fresh graph
        GraphModel g2;
        g2.onChange = [](){};

        if (auto* obj = state.getDynamicObject())
        {
            auto nodesArr = obj->getProperty ("nodes");
            if (auto* arr = nodesArr.getArray())
            {
                for (auto& nv : *arr)
                {
                    if (auto* nd = nv.getDynamicObject())
                    {
                        g2.restoreNode (
                            nd->getProperty ("id").toString(),
                            (int)   nd->getProperty ("nodeType"),
                            (float) nd->getProperty ("x"),
                            (float) nd->getProperty ("y"),
                            nd->getProperty ("paxName").toString());
                    }
                }
            }
        }

        expectEquals (g2.getNodes().size(), (size_t) 2);
        expectEquals (g2.getNodes()[0].nodeType, g.getNodes()[0].nodeType);
        expectEquals (g2.getNodes()[1].nodeType, g.getNodes()[1].nodeType);
    }

    // v0.0.930 — packs: UI/project state kept in GraphModel
    void testPacks()
    {
        beginTest ("packs: add, one-level membership, node removal, undo snapshot round trip");

        auto g = makeGraph();
        const auto a = g.addNode (5, 0.f, 0.f).id;     // MIDI Monitor
        const auto b = g.addNode (28, 200.f, 0.f).id;  // MIDI Morpher
        const auto c = g.addNode (5, 400.f, 0.f).id;

        auto& p1 = g.addPack ("FIRST", juce::StringArray { a, b }, juce::StringArray { "x_MIDI In_in" }, 10.f, 20.f);
        const auto p1Id = p1.id;
        expectEquals ((int) g.getPacks().size(), 1);
        expectEquals (g.findPack (p1Id)->nodeIds.size(), 2);

        // A node joining another pack leaves the first one (one level, one pack per node)
        const auto p2Id = g.addPack ("SECOND", juce::StringArray { b, c }, {}, 0.f, 0.f).id;
        expectEquals (g.findPack (p1Id)->nodeIds.size(), 1, "b moved out of FIRST");
        expectEquals (g.findPack (p2Id)->nodeIds.size(), 2);

        // Undo snapshot round trip keeps packs (name, members, ports, position, open)
        g.findPack (p1Id)->open = true;
        g.pushSnapshot();
        g.removePack (p2Id);
        expectEquals ((int) g.getPacks().size(), 1);
        expect (g.undo());
        expectEquals ((int) g.getPacks().size(), 2, "undo brings the pack back");
        auto* r1 = g.findPack (p1Id);
        expect (r1 != nullptr);
        if (r1 != nullptr)
        {
            expectEquals (r1->name, juce::String ("FIRST"));
            expectEquals (r1->ports[0], juce::String ("x_MIDI In_in"));
            expectEquals ((int) r1->x, 10);
            expect (r1->open);
        }

        // Removing the last member removes the pack
        g.removeNode (a);
        expect (g.findPack (p1Id) == nullptr, "empty pack removed");
        expectEquals ((int) g.getPacks().size(), 1);

        // toVar carries packs; restorePacks drops members that no longer exist
        auto v = g.toVar();
        expect (v.getProperty ("packs", {}).isArray());
        g.removeNode (c);
        g.restorePacks (v.getProperty ("packs", {}));
        expectEquals ((int) g.getPacks().size(), 1);
        expectEquals (g.getPacks()[0].nodeIds.size(), 1, "c dropped, b kept");

        // Older snapshots without "packs" → none
        g.restorePacks (juce::var());
        expectEquals ((int) g.getPacks().size(), 0);
    }
};

// Register the test suite
static GraphModelTests graphModelTests;
