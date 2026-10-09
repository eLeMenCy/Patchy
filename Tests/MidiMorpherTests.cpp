// Patchy — MIDI Morpher engine tests (JUCE UnitTest framework), v0.0.925
//
// Drives MidiMorpherNode::process() with real MidiBuffers: default rule,
// matching, out-of-range ignore, pass/block, scaling (incl. inverted and
// fixed), Pull swaps, pitch bend 14-bit handling, channel remap, system
// messages, disabled pass-through, and settingsJson parsing.

#include <juce_core/juce_core.h>
#include "MidiMorpherNode.h"

class MidiMorpherTests : public juce::UnitTest
{
public:
    MidiMorpherTests() : juce::UnitTest ("MIDI Morpher", "MidiMorpher") {}

    using M    = MidiMorpherNode;
    using Rule = MidiMorpherNode::Rule;

    // Run one message through a node with the given rule; returns the output.
    static std::vector<juce::MidiMessage> run (const Rule& r, const juce::MidiMessage& in, bool disabled = false)
    {
        MidiMorpherNode node ("test");
        node.prepare (48000.0, 64);
        node.setRule (r);
        node.disabled = disabled;
        node.inputMidi.addEvent (in, 0);
        node.process (64);
        std::vector<juce::MidiMessage> out;
        for (const auto meta : node.outputMidi)
            out.push_back (meta.getMessage());
        return out;
    }

    void expectOne (const std::vector<juce::MidiMessage>& out, const juce::MidiMessage& expected, const juce::String& what)
    {
        expectEquals ((int) out.size(), 1, what + " (count)");
        if (out.size() == 1)
            expect (out[0].getRawDataSize() == expected.getRawDataSize()
                    && std::memcmp (out[0].getRawData(), expected.getRawData(), (size_t) expected.getRawDataSize()) == 0,
                    what + ": got " + out[0].getDescription() + ", expected " + expected.getDescription());
    }

    static Rule fromJson (const char* json) { return M::parseRule (juce::JSON::parse (juce::String (json))); }

    void runTest() override
    {
        beginTest ("Default rule copies everything unchanged");
        {
            Rule r;
            expectOne (run (r, juce::MidiMessage::noteOn (3, 60, (juce::uint8) 100)), juce::MidiMessage::noteOn (3, 60, (juce::uint8) 100), "note on");
            expectOne (run (r, juce::MidiMessage::controllerEvent (16, 7, 127)),        juce::MidiMessage::controllerEvent (16, 7, 127), "cc");
            expectOne (run (r, juce::MidiMessage::pitchWheel (1, 12345)),               juce::MidiMessage::pitchWheel (1, 12345), "pitch");
            expectOne (run (r, juce::MidiMessage::programChange (2, 42)),               juce::MidiMessage::programChange (2, 42), "program");
            expectOne (run (r, juce::MidiMessage::channelPressureChange (5, 90)),       juce::MidiMessage::channelPressureChange (5, 90), "channel AT");
            expectOne (run (r, juce::MidiMessage::noteOff (1, 64, (juce::uint8) 0)),    juce::MidiMessage::noteOff (1, 64, (juce::uint8) 0), "note off");
        }

        beginTest ("User's MidiDash example: ch 3 CC 7 -> CC 110");
        {
            const Rule r = fromJson (R"({"inCh":3,"inMsg":4,"inD1":[7,7],"outD1":[110,110]})");
            expectOne (run (r, juce::MidiMessage::controllerEvent (3, 7, 64)), juce::MidiMessage::controllerEvent (3, 110, 64), "remapped");
            expectOne (run (r, juce::MidiMessage::controllerEvent (3, 8, 64)), juce::MidiMessage::controllerEvent (3, 8, 64),   "other CC passes");
            expectOne (run (r, juce::MidiMessage::controllerEvent (4, 7, 64)), juce::MidiMessage::controllerEvent (4, 7, 64),   "other channel passes");
        }

        beginTest ("Unmatched: block");
        {
            const Rule r = fromJson (R"({"inMsg":4,"inD1":[7,7],"unmatched":"block"})");
            expectEquals ((int) run (r, juce::MidiMessage::controllerEvent (1, 8, 64)).size(), 0, "other CC blocked");
            expectEquals ((int) run (r, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 1)).size(), 0, "note blocked");
            expectEquals ((int) run (r, juce::MidiMessage::controllerEvent (1, 7, 64)).size(), 1, "match kept");
        }

        beginTest ("Out of range is ignored, never clamped");
        {
            const Rule r = fromJson (R"({"inMsg":2,"inD2":[10,20],"outD2":[100,110]})");
            expectOne (run (r, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 30)), juce::MidiMessage::noteOn (1, 60, (juce::uint8) 30), "vel 30 untouched");
            expectOne (run (r, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 15)), juce::MidiMessage::noteOn (1, 60, (juce::uint8) 105), "vel 15 scaled");
        }

        beginTest ("OUT Min-Max keeps the value; an explicit OUT range scales");
        {
            const Rule filterOnly = fromJson (R"({"inMsg":2,"inD2":[64,-1]})");
            expectOne (run (filterOnly, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 80)), juce::MidiMessage::noteOn (1, 60, (juce::uint8) 80), "filter only: unchanged");

            const Rule stretch = fromJson (R"({"inMsg":2,"inD2":[64,127],"outD2":[0,127]})");
            expectOne (run (stretch, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 64)),  juce::MidiMessage::noteOn (1, 60, (juce::uint8) 0),   "64 -> 0");
            expectOne (run (stretch, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127)), juce::MidiMessage::noteOn (1, 60, (juce::uint8) 127), "127 -> 127");
        }

        beginTest ("Inverted range, fixed value, channel remap");
        {
            const Rule inv = fromJson (R"({"inMsg":4,"outD2":[127,0]})");
            expectOne (run (inv, juce::MidiMessage::controllerEvent (1, 1, 0)),   juce::MidiMessage::controllerEvent (1, 1, 127), "0 -> 127");
            expectOne (run (inv, juce::MidiMessage::controllerEvent (1, 1, 100)), juce::MidiMessage::controllerEvent (1, 1, 27),  "100 -> 27");

            const Rule fixed = fromJson (R"({"inMsg":2,"outD2":[90,90],"outCh":10})");
            expectOne (run (fixed, juce::MidiMessage::noteOn (4, 36, (juce::uint8) 12)), juce::MidiMessage::noteOn (10, 36, (juce::uint8) 90), "fixed velocity on ch 10");
        }

        beginTest ("Message conversion + Pull: note velocity -> CC 74 value");
        {
            const Rule r = fromJson (R"({"inMsg":2,"outMsg":4,"outD1":[74,74],"outD2Pull":false})");
            expectOne (run (r, juce::MidiMessage::noteOn (2, 60, (juce::uint8) 99)), juce::MidiMessage::controllerEvent (2, 74, 99), "vel -> CC 74");

            const Rule swap = fromJson (R"({"inMsg":4,"outD1Pull":true,"outD2Pull":true})");
            expectOne (run (swap, juce::MidiMessage::controllerEvent (1, 20, 5)), juce::MidiMessage::controllerEvent (1, 5, 20), "swap d1/d2");
        }

        beginTest ("Pitch bend 14-bit");
        {
            const Rule toCC = fromJson (R"({"inMsg":7,"outMsg":4,"outD1Pull":false,"outD2Pull":true})");
            // Pitch Data1 (0-16383, full range) pulled into CC Data2 (0-127), CC number from Data1 too:
            // Data1 of the CC = pitch value scaled to 0-127 as well (full ranges both sides).
            expectOne (run (toCC, juce::MidiMessage::pitchWheel (1, 16383)), juce::MidiMessage::controllerEvent (1, 127, 127), "max");
            expectOne (run (toCC, juce::MidiMessage::pitchWheel (1, 0)),     juce::MidiMessage::controllerEvent (1, 0, 0),     "min");
            expectOne (run (toCC, juce::MidiMessage::pitchWheel (1, 8192)),  juce::MidiMessage::controllerEvent (1, 64, 64),   "centre");

            const Rule fromCC = fromJson (R"({"inMsg":4,"inD1":[1,1],"outMsg":7,"outD1Pull":true})");
            expectOne (run (fromCC, juce::MidiMessage::controllerEvent (1, 1, 127)), juce::MidiMessage::pitchWheel (1, 16383), "mod wheel max -> bend max");
            expectOne (run (fromCC, juce::MidiMessage::controllerEvent (1, 1, 0)),   juce::MidiMessage::pitchWheel (1, 0),     "mod wheel 0 -> bend 0");

            const Rule upper = fromJson (R"({"inMsg":7,"inD1":[8192,-1],"unmatched":"block"})");
            expectEquals ((int) run (upper, juce::MidiMessage::pitchWheel (1, 100)).size(), 0, "lower half blocked");
            expectOne (run (upper, juce::MidiMessage::pitchWheel (1, 9000)), juce::MidiMessage::pitchWheel (1, 9000), "upper half kept");
        }

        beginTest ("Program change has no Data2: Data2 range not checked, absent source -> OUT low");
        {
            const Rule r = fromJson (R"({"inMsg":5,"inD2":[50,60],"outMsg":4,"outD1":[20,20],"outD2":[0,127]})");
            expectOne (run (r, juce::MidiMessage::programChange (1, 9)), juce::MidiMessage::controllerEvent (1, 20, 0), "program -> CC 20 value 0");
        }

        beginTest ("System messages always pass, even when blocking");
        {
            const Rule r = fromJson (R"({"unmatched":"block","inMsg":2})");
            expectEquals ((int) run (r, juce::MidiMessage::midiClock()).size(), 1, "clock passes");
        }

        beginTest ("Disabled = identity");
        {
            const Rule r = fromJson (R"({"inMsg":4,"outMsg":2,"unmatched":"block"})");
            expectOne (run (r, juce::MidiMessage::controllerEvent (1, 7, 7), true), juce::MidiMessage::controllerEvent (1, 7, 7), "disabled");
        }

        beginTest ("Live feedback: counts and last morph (v0.0.926)");
        {
            MidiMorpherNode node ("fb");
            node.prepare (48000.0, 64);
            node.setRule (fromJson (R"({"inCh":3,"inMsg":4,"inD1":[7,7],"outD1":[110,110],"unmatched":"block"})"));
            node.inputMidi.addEvent (juce::MidiMessage::controllerEvent (3, 7, 64), 0);   // match
            node.inputMidi.addEvent (juce::MidiMessage::controllerEvent (3, 8, 1),  1);   // blocked
            node.inputMidi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 9), 2);   // blocked
            node.process (64);
            auto f = node.drainFeedback();
            expectEquals (f.matched, 1);
            expectEquals (f.passed,  0);
            expectEquals (f.blocked, 2);
            expect (f.last == M::packMorph (M::kCC, 3, 7, 64, M::kCC, 3, 110, 64), "last morph packed");
            expect ((f.last >> 63) == 1, "valid flag");
            auto again = node.drainFeedback();
            expectEquals (again.matched + again.passed + again.blocked, 0, "counts reset after drain");
            expect (again.last == f.last, "last morph kept");

            const auto big = M::packMorph (M::kPitch, 16, 16383, 0, M::kCC, 1, 127, 127);
            expectEquals ((int) (big & 0x7), (int) M::kPitch);
            expectEquals ((int) ((big >> 3) & 0xF), 15);
            expectEquals ((int) ((big >> 7) & 0x3FFF), 16383);
            expectEquals ((int) ((big >> 28) & 0x7), (int) M::kCC);
            expectEquals ((int) ((big >> 49) & 0x7F), 127);
        }

        beginTest ("Learn (v0.0.929): skips releases, captures the first press, holds input back");
        {
            MidiMorpherNode node ("learn");
            node.prepare (48000.0, 64);
            node.armLearn (true);
            node.inputMidi.addEvent (juce::MidiMessage::noteOff (2, 60, (juce::uint8) 0), 0);   // release: skipped
            node.inputMidi.addEvent (juce::MidiMessage::noteOn  (2, 61, (juce::uint8) 0), 1);   // vel 0 = release: skipped
            node.inputMidi.addEvent (juce::MidiMessage::midiClock(), 2);                         // system: passes
            node.inputMidi.addEvent (juce::MidiMessage::noteOn  (5, 64, (juce::uint8) 99), 3);  // learned
            node.inputMidi.addEvent (juce::MidiMessage::controllerEvent (5, 7, 1), 4);          // after: still held this block
            node.process (64);
            int n = 0; bool onlyClock = true;
            for (const auto meta : node.outputMidi) { ++n; onlyClock = onlyClock && meta.data[0] == 0xF8; }
            expectEquals (n, 1, "only the clock went through");
            expect (onlyClock);
            expect (! node.learnArmed.load(), "disarmed after the capture");
            const auto w = node.drainLearned();
            expect ((w >> 63) == 1, "valid");
            expectEquals ((int) (w & 0xFF), 0x94, "status = Note On ch 5");
            expectEquals ((int) ((w >> 8) & 0x7F), 64);
            expectEquals ((int) ((w >> 15) & 0x7F), 99);
            expect (node.drainLearned() == 0, "drained once");

            // Disarmed again: normal processing is back
            node.resetBuffers (64);
            node.inputMidi.addEvent (juce::MidiMessage::controllerEvent (1, 7, 64), 0);
            node.process (64);
            expectEquals (node.outputMidi.getNumEvents(), 1, "rule applies again");

            // Pitch bend keeps both data bytes
            node.armLearn (true);
            node.resetBuffers (64);
            node.inputMidi.addEvent (juce::MidiMessage::pitchWheel (1, 12345), 0);
            node.process (64);
            const auto pw = node.drainLearned();
            expectEquals ((int) (((pw >> 8) & 0x7F) | (((pw >> 15) & 0x7F) << 7)), 12345);

            // Disarm before anything arrives: nothing learned, input flows
            node.armLearn (true);
            node.armLearn (false);
            node.resetBuffers (64);
            node.inputMidi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
            node.process (64);
            expectEquals (node.outputMidi.getNumEvents(), 1);
            expect (node.drainLearned() == 0);
        }

        beginTest ("parseRule clamps and defaults");
        {
            const Rule r = fromJson (R"({"inCh":99,"inMsg":-3,"inD1":[-5,99999],"outD1":"junk"})");
            expectEquals (r.inCh, 16);
            expectEquals (r.inMsg, 0);
            expectEquals (r.inD1.lo, 0);
            expectEquals (r.inD1.hi, 16383);
            expectEquals (r.outD1.hi, -1);
            expect (! r.blockUnmatched);
        }
    }
};

static MidiMorpherTests midiMorpherTests;
