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
//  We carry the full universe blob in PAX_Value::data (up to 56 bytes = first
//  56 DMX channels). PAX_Value::key = universe number. PAX_Value::value =
//  channel[0] normalised to 0.0–1.0 (convenient for single-channel use).
// ─────────────────────────────────────────────────────────────────────────────
namespace ArtNetCodec
{
    static constexpr uint16_t kOpDmx    = 0x5000u;
    static constexpr uint16_t kProtVer  = 14u;
    static constexpr int      kPort     = 6454;
    static constexpr int      kHdrSize  = 18;   // bytes before DMX data

    static const char* kArtNetId = "Art-Net";   // 7 chars + null = 8 bytes

    // ── Validate and parse an ArtDmx packet into a PAX_Value ─────────────────
    static bool parse (const uint8_t* buf, int len, PAX_Value& out)
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
        out.dataType = PAX_DATA_BLOB;
        out.key      = universe;
        out.value    = dmx[0] / 255.f;   // ch1 normalised, convenient shortcut

        // Copy as many channels as fit in data[]
        uint16_t copyLen = static_cast<uint16_t> (
            std::min ((int) sizeof (out.data), (int) dmxLen));
        std::memcpy (out.data, dmx, copyLen);
        out.dataSize = copyLen;

        return true;
    }

    // ── Serialise a PAX_Value blob into an ArtDmx packet ─────────────────────
    /** Returns byte count, or 0 on failure. buf must be >= kHdrSize + 512. */
    static int serialise (uint16_t universe, const PAX_Value& v,
                          uint8_t* buf, int bufCap)
    {
        // DMX data comes from v.data[]; pad to even length, minimum 2
        unsigned dmxLen = v.dataSize > 0 ? (unsigned) v.dataSize : 1u;
        if (dmxLen & 1u) ++dmxLen;   // must be even
        dmxLen = std::min (dmxLen, 512u);

        if ((unsigned) bufCap < (unsigned) kHdrSize + dmxLen) return 0;

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

        // Length big-endian
        buf[16] = (uint8_t) (dmxLen >> 8);
        buf[17] = (uint8_t) (dmxLen & 0xFFu);

        // DMX data — copy what we have, zero-pad the rest
        unsigned srcLen = std::min ((unsigned) v.dataSize, dmxLen);
        if (srcLen > 0)
            std::memcpy (buf + kHdrSize, v.data, (size_t) srcLen);
        if (srcLen < dmxLen)
            std::memset (buf + kHdrSize + srcLen, 0, (size_t) (dmxLen - srcLen));

        return (int) (kHdrSize + dmxLen);
    }

} // namespace ArtNetCodec


// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetInDeviceNode  (nodeType 12)
 *
 * Listens on UDP port 6454 (Art-Net standard port). Parses incoming ArtDmx
 * packets and emits one PAX_Value per packet:
 *   - type    = PAX_TYPE_DMX
 *   - key     = universe number
 *   - value   = channel[0] / 255.0 (0.0–1.0)
 *   - data[]  = raw DMX channel bytes (up to 56 channels)
 *   - dataSize= number of channels copied
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
        while (fifo.pop (pkt) && count < kMaxValueEvents)
        {
            // Filter by universe if set (universe == -1 means accept all)
            if (universe >= 0 && (int) pkt.value.key != universe)
                continue;

            outputValues[static_cast<size_t>(count)] = pkt.value;
            ++count;
        }
        outputValueCount = count;
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
            if (ArtNetCodec::parse (buf.data(), bytesRead, v))
            {
                // Only flash on actual DMX value changes, not on every refresh packet
                const uint8_t* dmx = buf.data() + ArtNetCodec::kHdrSize;
                int dmxLen = std::min ((int) v.dataSize, 512);
                if (std::memcmp (dmx, lastDmx.data(), (size_t) dmxLen) != 0)
                {
                    std::memcpy (lastDmx.data(), dmx, (size_t) dmxLen);
                    recordMidiActivity (1);
                }
                ParsedPacket pkt;
                pkt.value = v;
                fifo.push (pkt);
            }
        }
    }

    struct ParsedPacket { PAX_Value value {}; };

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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArtNetInDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * ArtNetOutDeviceNode  (nodeType 13)
 *
 * Receives PAX_Value blobs from the graph and sends them as ArtDmx packets
 * to a configured target host on port 6454.
 *
 * PAX_Value::data[] = raw DMX channel bytes (up to 56 channels)
 * PAX_Value::key    = universe override (0 = use configured universe)
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
        juce::Logger::writeToLog ("ArtNetOutDeviceNode: ready → "
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
        for (int i = 0; i < inputValueCount; ++i)
            sendQueue.push (inputValues[static_cast<size_t> (i)]);

        if (inputValueCount > 0)
            sendQueue.signalDataAvailable();
    }

    juce::String targetHost;
    int          universe = 0;

private:
    void run() override
    {
        // 18 hdr + 512 DMX
        static constexpr int kBufCap = ArtNetCodec::kHdrSize + 512;
        std::array<uint8_t, kBufCap> buf;

        while (! threadShouldExit())
        {
            PAX_Value v {};
            if (! sendQueue.pop (v))
            {
                sendQueue.waitForData (100);
                continue;
            }
            if (socket == nullptr) continue;

            // Allow upstream to override universe via key (if non-zero)
            uint16_t uni = (v.key != 0) ? (uint16_t) v.key
                                        : (uint16_t) universe;

            int msgLen = ArtNetCodec::serialise (uni, v, buf.data(), kBufCap);
            if (msgLen > 0)
                socket->write (targetHost, ArtNetCodec::kPort, buf.data(), msgLen);
        }
    }

    static constexpr int kFifoSize = 64;
    struct SendFifo
    {
        void push (const PAX_Value& v)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) values[static_cast<size_t> (s1)] = v;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (PAX_Value& v)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            v = values[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        void signalDataAvailable() { event.signal(); }
        void waitForData (int timeoutMs) { event.wait (timeoutMs); }

        juce::AbstractFifo               fifo { kFifoSize };
        std::array<PAX_Value, kFifoSize> values;
        juce::WaitableEvent              event { true };
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
