#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
#include "ArtNetReceiver.h"
#include "../Pax/PaxAPI.h"
#include <algorithm>

// Forward declare to avoid circular include
class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal Art-Net DMX parser / serialiser — no external dependencies.
//
//  Art-Net packet layout (UDP, little-endian header, port 6454):
//
//    ID          8 bytes   "Art-Net\0"
//    OpCode      2 bytes   0x0050 (little-endian) = ArtDmx
//    ProtVer     2 bytes   0x000e (big-endian)     = protocol version 14
//    Sequence    1 byte    0x00 = disabled
//    Physical    1 byte    0x00
//    Universe    2 bytes   little-endian (0–32767)
//    Length      2 bytes   big-endian, number of DMX channels (2–512, even)
//    Data        N bytes   DMX channel values (1 byte each, 0–255)
//
//  The full universe now travels via the dedicated ArtNet frame path
//  (NodeProcessor.h's inputArtNetFrame/outputArtNetFrame, plus a paired
//  universe number) rather than PAX_Value::data — that's only 56 bytes,
//  which used to silently truncate every universe to its first 56 of 512
//  channels, the exact same bug DMX had before its own equivalent fix
//  (see Architecture.md). A lightweight PAX_Value mirror still travels
//  alongside (type=DMX, dataType=FLOAT, key=universe, value=channel[0]
//  normalised, no blob) purely so the existing port/edge-matching UI
//  machinery keeps working.
// ─────────────────────────────────────────────────────────────────────────────
namespace ArtNetCodec
{
    static constexpr uint16_t kOpDmx    = 0x5000u;
    static constexpr uint16_t kProtVer  = 14u;
    static constexpr int      kPort     = 6454;
    static constexpr int      kHdrSize  = 18;   // bytes before DMX data

    static const char* kArtNetId = "Art-Net";   // 7 chars + null = 8 bytes

    // ── Validate and parse an ArtDmx packet ──────────────────────────────────
    // Writes a full 512-channel frame into fullOut (zero-padded past
    // whatever the packet actually carried) — the real payload now, not
    // the old approach of cramming it into PAX_Value.data[] (56 bytes),
    // which silently truncated at the first 56 of 512 channels. `out` is
    // now a lightweight scalar mirror only (no blob), same convention
    // DMX's own migrated nodes already use, purely so the existing port/
    // edge-matching UI machinery keeps working.
    static bool parse (const uint8_t* buf, int len, PAX_Value& out, std::array<uint8_t, 512>& fullOut)
    {
        // Minimum: 18-byte header + 2 DMX bytes
        if (len < kHdrSize + 2) return false;

        // Check "Art-Net\0" ID
        if (std::memcmp (buf, kArtNetId, 8) != 0) return false;

        // OpCode — little-endian
        uint16_t opCode = (uint16_t) (buf[8] | (buf[9] << 8));
        if (opCode != kOpDmx) return false;

        // Universe — little-endian bytes 14–15
        uint16_t universe = (uint16_t) (buf[14] | (buf[15] << 8));

        // Length — big-endian bytes 16–17
        uint16_t dmxLen = (uint16_t) ((buf[16] << 8) | buf[17]);
        if (dmxLen < 2 || len < kHdrSize + dmxLen) return false;

        const uint8_t* dmx = buf + kHdrSize;

        out = PAX_Value{};
        out.type     = PAX_TYPE_DMX;
        out.dataType = PAX_DATA_FLOAT;
        out.key      = universe;
        // Max across the received frame (fixed 2026-08-30, as part of
        // migrating ArtNet from "flash" to DMX's own "continuous
        // intensity" treatment — see App.tsx and Architecture.md), not
        // just dmx[0] (channel 1) as this "convenient shortcut" did
        // before — the same fix already applied to ArtNetConsoleNode.h
        // and ArtNetMonitorNode.h earlier the same day, for the identical
        // reason: channel 1 alone meant this value never reflected any
        // other channel actually being active.
        out.value    = *std::max_element (dmx, dmx + dmxLen) / 255.f;

        fullOut.fill (0);
        const int copyLen = std::min ((int) fullOut.size(), (int) dmxLen);
        std::memcpy (fullOut.data(), dmx, (size_t) copyLen);

        return true;
    }

    // ── Serialise a full 512-channel universe into an ArtDmx packet ─────────
    /** Returns byte count, or 0 on failure. buf must be >= kHdrSize + 512. */
    static int serialise (uint16_t universe, const uint8_t* dmxData /* 512 bytes */,
                          uint8_t* buf, int bufCap)
    {
        if ((unsigned) bufCap < (unsigned) kHdrSize + 512u) return 0;

        // ID "Art-Net\0"
        std::memcpy (buf, kArtNetId, 8);

        // OpCode little-endian
        buf[8]  = (uint8_t) (kOpDmx & 0xFFu);
        buf[9]  = (uint8_t) (kOpDmx >> 8);

        // ProtVer big-endian
        buf[10] = 0x00;
        buf[11] = (uint8_t) kProtVer;

        // Sequence, Physical
        buf[12] = 0x00;
        buf[13] = 0x00;

        // Universe little-endian
        buf[14] = (uint8_t) (universe & 0xFFu);
        buf[15] = (uint8_t) (universe >> 8);

        // Length big-endian — always the full 512, same convention DMX's
        // own EnttecProCodec::buildOutputPacket already uses
        buf[16] = (uint8_t) (512u >> 8);
        buf[17] = (uint8_t) (512u & 0xFFu);

        std::memcpy (buf + kHdrSize, dmxData, 512);

        return (int) (kHdrSize + 512);
    }

} // namespace ArtNetCodec


// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetInDeviceNode  (nodeType 12)
 *
 * Listens on UDP port 6454 (Art-Net standard port). Parses incoming ArtDmx
 * packets and emits the full 512-channel universe every block via the
 * dedicated ArtNet frame path (NodeProcessor.h — outputArtNetFrame/
 * outputArtNetFrameValid/outputArtNetUniverse), plus a lightweight
 * PAX_Value mirror alongside it (type=DMX, dataType=FLOAT, key=universe,
 * value=channel[0]/255, no blob) for anything that only wants a plain
 * scalar. The frame is the real payload now — the old approach of
 * cramming the universe into PAX_Value.data[] silently truncated at 56 of
 * 512 channels, the exact same bug DMX had before its own fix.
 */
class ArtNetInDeviceNode : public NodeProcessor,
                           private ArtNetReceiver::Listener
{
public:
    explicit ArtNetInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi)
    {}

    ~ArtNetInDeviceNode() override { closeSocket(); }

    // v0.0.916 (2026-09-28) — shared receiver. This node no longer owns a
    // socket or a thread: every ArtNet In subscribes to the one process-wide
    // ArtNetReceiver (see ArtNetReceiver.h for why — two nodes, or two
    // Patchy instances, used to fight over port 6454 and whoever bound last
    // won). A universe change is now just a filter change, no rebind.
    // Method names are kept so existing callers stay unchanged.
    void configure (int universeToUse)
    {
        universe.store (universeToUse, std::memory_order_relaxed);
        openSocket();
    }

    // Kept for its callers (ArtNetDeviceManager). The old socket-transfer
    // dance (stop the old node's thread, move its socket — see SessionLog,
    // 2026-09-01/02, including the EXC_BAD_ACCESS it once caused) is gone:
    // the new node simply subscribes, and the shared socket stays bound as
    // long as at least one node (old or new) is subscribed, so a graph
    // rebuild never reopens it. Returns true when an old node existed.
    bool transferOrConfigure (int universeToUse, ArtNetInDeviceNode* oldNode)
    {
        configure (universeToUse);
        return oldNode != nullptr;
    }

    void openSocket()
    {
        if (subscribed) return;
        ArtNetReceiver::subscribe (this);
        subscribed = true;
        juce::Logger::writeToLog ("ArtNetInDeviceNode: subscribed to shared receiver, universe "
                                  + juce::String (universe.load (std::memory_order_relaxed)));
    }

    // v0.0.916 — "port 6454 in use" on this node: subscribed, but the
    // shared receiver can't bind (another application holds the port).
    int portInUseForUi() const
    {
        return subscribed && ArtNetReceiver::isPortInUse() ? 6454 : 0;
    }

    // Blocks until this node's callback can no longer run (see
    // ArtNetReceiver::unsubscribe()). Idempotent.
    void closeSocket()
    {
        if (! subscribed) return;
        ArtNetReceiver::unsubscribe (this);
        subscribed = false;
    }

    void process (int /*numSamples*/) override
    {
        // Real bug found 2026-09-15 (same root cause/fix as
        // MidiInDeviceNode's own — see that class's own process() comment
        // for the full story): the receiver callback below (was run()) keeps pushing into fifo
        // regardless of `disabled`, on its own thread, entirely
        // independent of whether process() runs. Always drained now
        // (never backlogging), but only actually forwarded to
        // outputValues/outputArtNetFrame when enabled — drained and
        // discarded while disabled, keeping this node correctly "cut".
        // Fix, v0.0.916 (2026-09-28) — a universe change must drop the last
        // known frame: it belonged to the OLD universe, and with a silent new
        // universe it was re-emitted below forever (downstream kept showing
        // old data; user saw the port "frozen" in its previous state).
        const int filterUniverse = universe.load (std::memory_order_relaxed);
        if (filterUniverse != emittedForUniverse)
        {
            emittedForUniverse = filterUniverse;
            hasReceived = false;
            lastReceived.fill (0);
        }

        ParsedPacket pkt;
        int count = 0;
        bool gotNew = false;
        while ((disabled || count < kMaxValueEvents) && fifo.pop (pkt))
        {
            if (disabled) continue;

            // Filter by universe if set (universe == -1 means accept all)
            if (filterUniverse >= 0 && (int) pkt.value.key != filterUniverse)
                continue;

            outputValues[static_cast<size_t>(count)] = pkt.value;
            ++count;

            // Last-known-frame state for the block-rate emission below —
            // in "accept all universes" mode (-1) this just tracks
            // whichever universe's packet arrived most recently, same
            // "shows the latest" convention ArtNetMonitorBuffer already
            // uses; it was never a genuine simultaneous multi-universe
            // view even before this fix.
            lastReceived = pkt.fullData;
            lastUniverse = (int) pkt.value.key;
            gotNew = true;
        }
        outputValueCount = count;

        // Always emit the last known frame at audio-block rate — same
        // convention DmxInDeviceNode already uses, gives downstream
        // Monitor/Out nodes a steady supply instead of bursty UDP packets.
        // v0.0.916 — nothing to hold yet (after a universe change, until the
        // new universe's first packet): tell downstream to drop our cached
        // frame. See NodeProcessor.h's outputArtNetWithdrawn.
        outputArtNetWithdrawn = ! (hasReceived || gotNew);

        if (! disabled && (hasReceived || gotNew))
        {
            hasReceived = true;
            outputArtNetFrame      = lastReceived;
            outputArtNetFrameValid = true;
            outputArtNetUniverse   = lastUniverse;
        }
    }

    bool passesThroughWhenDisabled() const override { return true; }

    // v0.0.916 — atomic: written on the message thread (configure()), read
    // on the receiver thread (packet filter) and the audio thread (process()).
    std::atomic<int> universe { 0 };

    std::atomic<int> bytesSinceLastPoll { 0 };
    int drainByteActivity() { return bytesSinceLastPoll.exchange (0, std::memory_order_relaxed); }

private:
    // v0.0.916 — was this node's own run() loop; now called by the shared
    // ArtNetReceiver's thread for every packet (receiver thread only, so
    // lastDmx and the fifo's producer side stay single-threaded as before).
    void artNetPacketReceived (const uint8_t* data, int bytesRead) override
    {
        PAX_Value v {};
        std::array<uint8_t, 512> fullFrame {};
        if (ArtNetCodec::parse (data, bytesRead, v, fullFrame))
        {
            // Fix, v0.0.916 (2026-09-28) — filter by universe HERE, so the
            // activity flash, the byte rate and the change detection only
            // react to this node's own universe (with the shared receiver
            // every node sees every universe; process() still filters too).
            const int u = universe.load (std::memory_order_relaxed);
            if (u >= 0 && (int) v.key != u)
                return;

            bytesSinceLastPoll.fetch_add (bytesRead, std::memory_order_relaxed);

            // Only flash on actual DMX value changes, not on every refresh packet
            if (std::memcmp (fullFrame.data(), lastDmx.data(), 512) != 0)
            {
                lastDmx = fullFrame;
                recordMidiActivity (1);
            }
            ParsedPacket pkt;
            pkt.value    = v;
            pkt.fullData = fullFrame;
            fifo.push (pkt);
        }
    }

    bool                     subscribed = false;   // message thread only
    std::array<uint8_t, 512> lastDmx {};           // receiver thread only — change detection

    struct ParsedPacket { PAX_Value value {}; std::array<uint8_t, 512> fullData {}; };

    static constexpr int kFifoSize = 64;
    struct PacketFifo
    {
        void push (const ParsedPacket& p)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) packets[static_cast<size_t> (s1)] = p;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (ParsedPacket& p)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            p = packets[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        juce::AbstractFifo                           fifo { kFifoSize };
        std::array<ParsedPacket, kFifoSize>          packets;
    } fifo;

    // Last-known-frame state — see process()'s comment for why this
    // persists between UDP packets rather than only existing per-block.
    std::array<uint8_t, 512> lastReceived {};
    int                      lastUniverse = 0;
    bool                     hasReceived  = false;
    int                      emittedForUniverse = 0;   // v0.0.916 — audio thread; see process()

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtNetInDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetOutDeviceNode  (nodeType 13)
 *
 * Receives full 512-channel universes via the dedicated ArtNet frame path
 * (NodeProcessor.h — inputArtNetFrame/inputArtNetFrameValid) and sends
 * them as ArtDmx packets to a configured target host on port 6454. Only
 * sends when data changes (memcmp vs last sent frame) — same convention
 * DmxOutDeviceNode already uses, added here because the frame path
 * persists and re-pushes every block once anything's connected (see the
 * multi-source merge cache in ProcessingGraph.cpp), which would otherwise
 * mean sending a UDP packet every single block regardless of whether
 * anything actually changed.
 *
 * Always transmits on this node's own configured universe (the "Universe"
 * field in its settings panel) — matches DmxOutDeviceNode's own precedent
 * of universe/hardware-port being a fixed per-instance property, and
 * matches what a user configuring a specific target universe would
 * reasonably expect, rather than having it silently overridden by
 * whatever universe number an upstream source happens to be tagged with.
 * No longer reads PAX_Value at all for the channel payload — the old
 * blob approach silently truncated at 56 of 512 channels.
 */
class ArtNetOutDeviceNode : public NodeProcessor,
                            private juce::Thread
{
public:
    explicit ArtNetOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("ArtNetOut_" + nodeId)
    {}

    ~ArtNetOutDeviceNode() override { closeSocket(); }

    void configure (const juce::String& targetHostToUse, int universeToUse)
    {
        closeSocket();
        targetHost = targetHostToUse;
        universe   = universeToUse;
        openSocket();
    }

    // Real fix, 2026-09-01 (2nd pass) — same reasoning and same fix as
    // ArtNetInDeviceNode's own (see that file's own comment for the full
    // story).
    bool transferOrConfigure (const juce::String& targetHostToUse, int universeToUse, ArtNetOutDeviceNode* oldNode)
    {
        if (oldNode != nullptr && oldNode->targetHost == targetHostToUse
            && oldNode->universe == universeToUse && oldNode->socket != nullptr)
        {
            // Real crash found and fixed 2026-09-02 — same reasoning and
            // same fix as ArtNetInDeviceNode's own (see that file's own
            // comment for the full story), but using this node's own
            // correct wake-up mechanism: its own thread blocks waiting on
            // sendQueue, not on a socket read, so that's what needs
            // signalling to let it notice threadShouldExit() promptly,
            // matching this class's own closeSocket(). Timeout reduced
            // from 1000ms to 200ms, 2026-09-02 — see DmxInDeviceNode's own
            // equivalent comment for the full reasoning.
            if (oldNode->isThreadRunning())
            {
                oldNode->signalThreadShouldExit();
                oldNode->sendQueue.signalDataAvailable();
                oldNode->stopThread (200);
            }

            socket     = std::move (oldNode->socket);
            targetHost = targetHostToUse;
            universe   = universeToUse;
            startThread (juce::Thread::Priority::normal);
            return true;
        }

        configure (targetHostToUse, universeToUse);
        return false;
    }

    void openSocket()
    {
        if (targetHost.isEmpty()) return;
        socket = std::make_unique<juce::DatagramSocket> (true);   // broadcast enabled
        startThread (juce::Thread::Priority::normal);
        juce::Logger::writeToLog ("ArtNetOutDeviceNode: ready -> "
                                  + targetHost + ":" + juce::String (ArtNetCodec::kPort)
                                  + " universe " + juce::String (universe));
    }

    void closeSocket()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            sendQueue.signalDataAvailable();
            stopThread (1000);
        }
        socket.reset();
    }

    void process (int /*numSamples*/) override
    {
        if (inputArtNetFrameValid)
        {
            sendQueue.push ({ (uint16_t) universe, inputArtNetFrame });
            sendQueue.signalDataAvailable();

            // Lightweight Value mirror, added 2026-08-30 — this node
            // never populated outputValues[0] at all before, the same
            // gap found and fixed the same day in DmxOutDeviceNode.h
            // (see that file's own comment for the fuller story). Max
            // across all 512 received channels, same reasoning as every
            // other DMX/ArtNet mirror fixed the same day.
            PAX_Value v {};
            v.type     = PAX_TYPE_DMX;
            v.dataType = PAX_DATA_FLOAT;
            v.key      = (uint32_t) universe;
            v.value    = *std::max_element (inputArtNetFrame.begin(), inputArtNetFrame.end()) / 255.f;

            outputValues[0] = v;
            outputValueCount = 1;
        }
        // Deliberately no else branch — reverted 2026-08-30 (3rd pass),
        // same reasoning and same fix as DmxOutDeviceNode.h's own revert
        // (see that file's own comment for the full story) — the user's
        // own explicit decision is hold-last-state on disconnect for
        // real physical DMX/ArtNet hardware output, both for sendQueue
        // itself (never touched) and this visual mirror (simply not
        // updated when inputArtNetFrameValid is false), consistent with
        // each other and with what the physical fixture is actually
        // still doing.
    }

    juce::String targetHost;
    int          universe = 0;

private:
    void run() override
    {
        // 18 hdr + 512 DMX
        static constexpr int kBufCap = ArtNetCodec::kHdrSize + 512;
        std::array<uint8_t, kBufCap>  buf;
        Frame                          lastSent {};
        bool                           hasSent = false;

        while (! threadShouldExit())
        {
            Frame f;
            if (! sendQueue.pop (f))
            {
                sendQueue.waitForData (100);
                continue;
            }
            if (socket == nullptr) continue;

            // Change detection — only send when values or universe differ
            if (hasSent && f.universe == lastSent.universe
                && std::memcmp (f.data.data(), lastSent.data.data(), 512) == 0)
                continue;
            lastSent = f;
            hasSent  = true;

            int msgLen = ArtNetCodec::serialise (f.universe, f.data.data(), buf.data(), kBufCap);
            if (msgLen > 0)
                socket->write (targetHost, ArtNetCodec::kPort, buf.data(), msgLen);
        }
    }

    struct Frame { uint16_t universe = 0; std::array<uint8_t, 512> data {}; };

    static constexpr int kFifoSize = 64;
    struct SendFifo
    {
        void push (const Frame& f)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) frames[static_cast<size_t> (s1)] = f;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (Frame& f)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            f = frames[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        void signalDataAvailable() { event.signal(); }
        void waitForData (int timeoutMs) { event.wait (timeoutMs); }

        juce::AbstractFifo           fifo { kFifoSize };
        std::array<Frame, kFifoSize> frames;
        juce::WaitableEvent          event { false };  // auto-reset: resets after each wait()
    } sendQueue;

    std::unique_ptr<juce::DatagramSocket> socket;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtNetOutDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetDeviceManager
 *
 * Owned by PatchyProcessor. Tracks per-node Art-Net settings and reapplies
 * them after graph rebuilds — same pattern as OscDeviceManager.
 */
class ArtNetDeviceManager
{
public:
    struct Settings
    {
        int          universe   = 0;
        juce::String targetHost;   // ArtNetOut only
    };

    void storeSettings (const juce::String& nodeId, const Settings& s) { settings[nodeId] = s; }

    const Settings* getSettings (const juce::String& nodeId) const
    {
        auto it = settings.find (nodeId);
        return it != settings.end() ? &it->second : nullptr;
    }

    bool applyToGraph (const juce::String& nodeId, ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);
    void applyAllSettings (ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);

private:
    std::unordered_map<juce::String, Settings> settings;
};
