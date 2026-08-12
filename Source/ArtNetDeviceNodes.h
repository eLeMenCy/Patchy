#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
#include "../Pax/PaxAPI.h"

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
        out.value    = dmx[0] / 255.f;   // ch1 normalised, convenient shortcut

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
                           private juce::Thread
{
public:
    explicit ArtNetInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("ArtNetIn_" + nodeId)
    {}

    ~ArtNetInDeviceNode() override { closeSocket(); }

    void configure (int universeToUse)
    {
        closeSocket();
        universe = universeToUse;
        openSocket();
    }

    void openSocket()
    {
        socket = std::make_unique<juce::DatagramSocket> (false);
        if (! socket->bindToPort (ArtNetCodec::kPort))
        {
            juce::Logger::writeToLog ("ArtNetInDeviceNode: failed to bind port "
                                      + juce::String (ArtNetCodec::kPort));
            socket.reset();
            return;
        }
        startThread (juce::Thread::Priority::normal);
        juce::Logger::writeToLog ("ArtNetInDeviceNode: listening on :"
                                  + juce::String (ArtNetCodec::kPort)
                                  + " universe " + juce::String (universe));
    }

    void closeSocket()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            if (socket != nullptr) socket->shutdown();
            stopThread (1000);
        }
        socket.reset();
    }

    void process (int /*numSamples*/) override
    {
        ParsedPacket pkt;
        int count = 0;
        bool gotNew = false;
        while (fifo.pop (pkt) && count < kMaxValueEvents)
        {
            // Filter by universe if set (universe == -1 means accept all)
            if (universe >= 0 && (int) pkt.value.key != universe)
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
        if (hasReceived || gotNew)
        {
            hasReceived = true;
            outputArtNetFrame      = lastReceived;
            outputArtNetFrameValid = true;
            outputArtNetUniverse   = lastUniverse;
        }
    }

    int universe = 0;

    std::atomic<int> bytesSinceLastPoll { 0 };
    int drainByteActivity() { return bytesSinceLastPoll.exchange (0, std::memory_order_relaxed); }

private:
    void run() override
    {
        static constexpr int kMaxPacket = 530;   // 18 hdr + 512 DMX
        std::array<uint8_t, kMaxPacket> buf;
        std::array<uint8_t, 512> lastDmx {};   // last seen universe data for change detection

        while (! threadShouldExit())
        {
            if (socket == nullptr) break;
            int ready = socket->waitUntilReady (true, 100);
            if (ready <= 0) continue;

            juce::String senderHost;
            int          senderPort = 0;
            int bytesRead = socket->read (buf.data(), kMaxPacket, false,
                                          senderHost, senderPort);
            if (bytesRead <= 0) continue;

            bytesSinceLastPoll.fetch_add (bytesRead, std::memory_order_relaxed);

            PAX_Value v {};
            std::array<uint8_t, 512> fullFrame {};
            if (ArtNetCodec::parse (buf.data(), bytesRead, v, fullFrame))
            {
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
    }

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

    std::unique_ptr<juce::DatagramSocket> socket;

    // Last-known-frame state — see process()'s comment for why this
    // persists between UDP packets rather than only existing per-block.
    std::array<uint8_t, 512> lastReceived {};
    int                      lastUniverse = 0;
    bool                     hasReceived  = false;

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
        }
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
        juce::WaitableEvent          event { true };
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

    bool applyToGraph (const juce::String& nodeId, ProcessingGraph& graph);
    void applyAllSettings (ProcessingGraph& graph);

private:
    std::unordered_map<juce::String, Settings> settings;
};
