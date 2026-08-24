#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
#include "../Pax/PaxAPI.h"

// Forward declare to avoid circular include
class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
//  UDP mode — Unicast (bind + optional remote send target), Multicast (join
//  group), or Broadcast (set broadcast flag on send).
// ─────────────────────────────────────────────────────────────────────────────
enum class UdpMode { Unicast = 0, Multicast = 1, Broadcast = 2 };

// ── Raw 4-byte float payload byte order — network byte order (big-endian),
//    the conventional default most external senders assume (e.g. Python's
//    struct.pack('>f', ...)), and the only sensible fixed choice given UDP
//    itself defines no float-encoding standard the way OSC does. Bug fix
//    2026-08-23: both directions previously used a raw memcpy with no byte-
//    order handling at all, meaning a big-endian sender's floats were
//    silently misinterpreted on a little-endian host (Apple Silicon/x86) —
//    e.g. 0.1-0.8 all landed as either huge negative numbers or values so
//    tiny they normalized to 0, while 0.9 landed as a huge positive number
//    that clamped to a downstream Pax's own max. These two helpers build/
//    read the bit pattern explicitly via bit-shifts (endianness-agnostic by
//    definition, since shifts operate on value not memory layout) before a
//    plain memcpy reinterpretation — correct regardless of host endianness,
//    not just a little-endian-specific fix. ─────────────────────────────────
inline float udpBigEndianFloatFromBytes (const uint8_t* b)
{
    uint32_t bits = (static_cast<uint32_t> (b[0]) << 24)
                  | (static_cast<uint32_t> (b[1]) << 16)
                  | (static_cast<uint32_t> (b[2]) << 8)
                  |  static_cast<uint32_t> (b[3]);
    float result;
    std::memcpy (&result, &bits, 4);
    return result;
}

inline void udpBigEndianBytesFromFloat (float value, uint8_t* outBytes)
{
    uint32_t bits;
    std::memcpy (&bits, &value, 4);
    outBytes[0] = static_cast<uint8_t> (bits >> 24);
    outBytes[1] = static_cast<uint8_t> (bits >> 16);
    outBytes[2] = static_cast<uint8_t> (bits >> 8);
    outBytes[3] = static_cast<uint8_t> (bits);
}

// ── Raw packet — full detail (sender IP/port + byte preview), used only for
//    the UDP Monitor display; independent of the routed PAX_Value. ───────────
struct RawUdpPacket
{
    juce::String             senderIp;
    int                      senderPort = 0;
    int                      byteCount  = 0;
    static constexpr int     kPreviewLen = 64;
    std::array<uint8_t, kPreviewLen> preview {};
    int                      previewLen = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * UdpInDeviceNode  (nodeType 8)
 *
 * 1 Value output port — feeds PAX_Value events to downstream graph nodes.
 * Listens on a UDP port (Unicast or Multicast) on a background thread and
 * pushes received datagrams into a lock-free FIFO. process() drains the FIFO
 * once per audio block.
 *
 * Payload mapping: a datagram of exactly 4 bytes is interpreted as a raw
 * float (PAX_DATA_FLOAT) in network byte order (big-endian) — see
 * udpBigEndianFloatFromBytes() above; any other size is carried as a blob
 * (PAX_DATA_BLOB, truncated to 56 bytes — the inline capacity of
 * PAX_Value::data).
 */
class UdpInDeviceNode : public NodeProcessor,
                         private juce::Thread
{
public:
    explicit UdpInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),   // borrow Midi type bucket for now (event-driven, not audio/AV)
          juce::Thread ("UdpIn_" + nodeId)
    {}

    ~UdpInDeviceNode() override { closeSocket(); }

    void configure (int portToUse, UdpMode modeToUse, const juce::String& multicastGroup)
    {
        closeSocket();
        port            = portToUse;
        mode            = modeToUse;
        multicastAddr   = multicastGroup;
        openSocket();
    }

    void openSocket()
    {
        if (port <= 0) return;

        socket = std::make_unique<juce::DatagramSocket> (/*canBroadcast*/ mode == UdpMode::Broadcast);

        if (! socket->bindToPort (port))
        {
            juce::Logger::writeToLog ("UdpInDeviceNode: failed to bind port " + juce::String (port));
            socket.reset();
            return;
        }

        if (mode == UdpMode::Multicast && multicastAddr.isNotEmpty())
        {
            if (! socket->joinMulticast (multicastAddr))
                juce::Logger::writeToLog ("UdpInDeviceNode: failed to join multicast " + multicastAddr);
        }

        juce::Logger::writeToLog ("UdpInDeviceNode: listening on port " + juce::String (port)
                                  + (mode == UdpMode::Multicast ? " (multicast " + multicastAddr + ")" : ""));
        startThread (juce::Thread::Priority::normal);
    }

    void closeSocket()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            if (socket) socket->shutdown();
            stopThread (1000);
        }
        socket.reset();
    }

    void process (int /*numSamples*/) override
    {
        outputValueCount = 0;
        ReceivedPacket pkt;
        while (outputValueCount < kMaxValueEvents && fifo.pop (pkt))
        {
            PAX_Value v {};
            v.key  = static_cast<uint32_t> (port);
            v.type = PAX_TYPE_UDP;

            if (pkt.size == 4)
            {
                v.dataType = PAX_DATA_FLOAT;
                v.value = udpBigEndianFloatFromBytes (pkt.data.data());
            }
            else
            {
                v.dataType = PAX_DATA_BLOB;
                v.dataSize = static_cast<uint16_t> (std::min (pkt.size, (int) sizeof (v.data)));
                std::memcpy (v.data, pkt.data.data(), static_cast<size_t> (v.dataSize));
            }

            outputValues[static_cast<size_t> (outputValueCount++)] = v;
        }

        // Drain full-detail raw packets for the UDP Monitor (if connected).
        // Held as a member (not consumed) so every downstream Monitor edge
        // can read this block's packets — mirrors OscInDeviceNode's pattern.
        lastRawPackets.clear();
        RawPacketMsg raw;
        while (monitorFifo.pop (raw))
            lastRawPackets.push_back (raw.pkt);
    }

    int  port = 0;
    UdpMode mode = UdpMode::Unicast;
    juce::String multicastAddr;

    // Activity tracking for UI byte-rate label (message thread polls + resets)
    std::atomic<int> bytesSinceLastPoll { 0 };
    int  drainByteActivity() { return bytesSinceLastPoll.exchange (0, std::memory_order_relaxed); }

    // Full-detail packets from this block — read by ProcessingGraph when
    // routing into a UdpMonitorNode. Not lock-free (message-thread-safe read
    // of a value refreshed once per process() call).
    std::vector<RawUdpPacket> lastRawPackets;

private:
    void run() override
    {
        static constexpr int kMaxPacket = 1500;
        std::array<uint8_t, kMaxPacket> buf;
        std::array<uint8_t, 56> lastData {};
        int lastSize = 0;
        juce::String senderIp;
        int senderPort = 0;

        while (! threadShouldExit())
        {
            if (socket == nullptr) break;
            int ready = socket->waitUntilReady (true, 100);
            if (ready <= 0) continue;

            int bytesRead = socket->read (buf.data(), kMaxPacket, false, senderIp, senderPort);
            if (bytesRead <= 0) continue;

            bytesSinceLastPoll.fetch_add (bytesRead, std::memory_order_relaxed);

            // Only flash on data change, not on every packet (handles continuous streams)
            int cmpLen = std::min (bytesRead, (int) sizeof (lastData));
            if (bytesRead != lastSize || std::memcmp (buf.data(), lastData.data(), (size_t) cmpLen) != 0)
            {
                lastSize = bytesRead;
                std::memcpy (lastData.data(), buf.data(), (size_t) cmpLen);
                recordMidiActivity (1);
            }

            ReceivedPacket pkt;
            pkt.size = std::min (bytesRead, (int) pkt.data.size());
            std::memcpy (pkt.data.data(), buf.data(), static_cast<size_t> (pkt.size));
            fifo.push (pkt);

            // Separate full-detail capture for the Monitor — sender IP/port
            // + a byte preview, independent of the routed PAX_Value above.
            RawUdpPacket rawPkt;
            rawPkt.senderIp   = senderIp;
            rawPkt.senderPort = senderPort;
            rawPkt.byteCount  = bytesRead;
            rawPkt.previewLen = std::min (bytesRead, (int) rawPkt.preview.size());
            std::memcpy (rawPkt.preview.data(), buf.data(), static_cast<size_t> (rawPkt.previewLen));
            RawPacketMsg rm;
            rm.pkt = rawPkt;
            monitorFifo.push (rm);
        }
    }

    struct ReceivedPacket
    {
        std::array<uint8_t, 56> data {};
        int size = 0;
    };

    static constexpr int kFifoSize = 256;
    struct PacketFifo
    {
        void push (const ReceivedPacket& p)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) packets[static_cast<size_t> (s1)] = p;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (ReceivedPacket& p)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            p = packets[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        juce::AbstractFifo                          fifo { kFifoSize };
        std::array<ReceivedPacket, kFifoSize>        packets;
    } fifo;

    // Separate small FIFO for full-detail Monitor packets (juce::String
    // members mean this isn't lock-free, but it's only ever touched by the
    // socket thread (push) and the message-thread process() call (pop)).
    struct RawPacketMsg { RawUdpPacket pkt; };
    static constexpr int kMonitorFifoSize = 64;
    struct MonitorFifo
    {
        void push (const RawPacketMsg& m)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) messages[static_cast<size_t> (s1)] = m;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (RawPacketMsg& m)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            m = messages[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        juce::AbstractFifo                          fifo { kMonitorFifoSize };
        std::array<RawPacketMsg, kMonitorFifoSize>  messages;
    } monitorFifo;

    std::unique_ptr<juce::DatagramSocket> socket;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UdpInDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * UdpOutDeviceNode  (nodeType 9)
 *
 * 1 Value input port — receives PAX_Value events from upstream graph nodes.
 * Sends each value as a UDP datagram to a configured target (Unicast IP:port,
 * Multicast group, or Broadcast). Sends happen on process() (message-rate,
 * not audio-rate — UDP send is non-blocking and cheap, but we still avoid
 * doing it from the audio thread by queuing to a background thread). A
 * PAX_DATA_FLOAT value is sent as 4 bytes in network byte order (big-endian)
 * — see udpBigEndianBytesFromFloat() above — matching UdpInDeviceNode's own
 * read-side convention.
 */
class UdpOutDeviceNode : public NodeProcessor,
                          private juce::Thread
{
public:
    explicit UdpOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("UdpOut_" + nodeId)
    {}

    ~UdpOutDeviceNode() override { closeSocket(); }

    void configure (int portToUse, UdpMode modeToUse,
                    const juce::String& targetHostToUse, const juce::String& multicastGroup)
    {
        closeSocket();
        port          = portToUse;
        mode          = modeToUse;
        targetHost    = targetHostToUse;
        multicastAddr = multicastGroup;
        openSocket();
    }

    void openSocket()
    {
        if (port <= 0) return;
        socket = std::make_unique<juce::DatagramSocket> (/*canBroadcast*/ mode == UdpMode::Broadcast);
        // Outbound socket — no bind needed for unicast/broadcast sends.
        // For multicast sends, JUCE's DatagramSocket can send directly to a
        // multicast group address without joining it (join is for receiving).
        startThread (juce::Thread::Priority::normal);
        juce::Logger::writeToLog ("UdpOutDeviceNode: ready to send to port " + juce::String (port));
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
        // Drain inputValues (set by ProcessingGraph from upstream node's outputValues)
        for (int i = 0; i < inputValueCount; ++i)
        {
            const auto& v = inputValues[static_cast<size_t> (i)];

            OutPacket pkt;
            if (v.dataType == PAX_DATA_FLOAT || v.dataSize == 0)
            {
                pkt.size = 4;
                udpBigEndianBytesFromFloat (v.value, pkt.data.data());
            }
            else
            {
                pkt.size = std::min ((int) v.dataSize, (int) pkt.data.size());
                std::memcpy (pkt.data.data(), v.data, static_cast<size_t> (pkt.size));
            }
            sendQueue.push (pkt);
        }
        sendQueue.signalDataAvailable();
    }

    int     port = 0;
    UdpMode mode = UdpMode::Unicast;
    juce::String targetHost;
    juce::String multicastAddr;

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            OutPacket pkt;
            if (! sendQueue.pop (pkt))
            {
                sendQueue.waitForData (100);
                continue;
            }
            if (socket == nullptr) continue;

            juce::String dest = (mode == UdpMode::Multicast) ? multicastAddr
                               : (mode == UdpMode::Broadcast) ? "255.255.255.255"
                               : targetHost;
            if (dest.isEmpty()) continue;

            socket->write (dest, port, pkt.data.data(), pkt.size);
        }
    }

    struct OutPacket
    {
        std::array<uint8_t, 56> data {};
        int size = 0;
    };

    static constexpr int kFifoSize = 256;
    struct SendFifo
    {
        void push (const OutPacket& p)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) packets[static_cast<size_t> (s1)] = p;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (OutPacket& p)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            p = packets[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        void signalDataAvailable() { event.signal(); }
        void waitForData (int timeoutMs) { event.wait (timeoutMs); }

        juce::AbstractFifo                   fifo { kFifoSize };
        std::array<OutPacket, kFifoSize>     packets;
        juce::WaitableEvent                  event { false };  // auto-reset: resets after each wait()
    } sendQueue;

    std::unique_ptr<juce::DatagramSocket> socket;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UdpOutDeviceNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * UdpDeviceManager
 *
 * Owned by PatchyProcessor. Tracks per-node UDP settings (port, mode, target/
 * multicast address) and (re)applies them after ProcessingGraph rebuilds —
 * same pattern as MidiDeviceManager / AudioDeviceManager.
 */
class UdpDeviceManager
{
public:
    struct Settings
    {
        int          port = 0;
        UdpMode      mode = UdpMode::Unicast;
        juce::String targetHost;     // unicast send target (UdpOut only)
        juce::String multicastAddr;  // multicast group (both directions)
    };

    void storeSettings (const juce::String& nodeId, const Settings& s) { settings[nodeId] = s; }

    const Settings* getSettings (const juce::String& nodeId) const
    {
        auto it = settings.find (nodeId);
        return it != settings.end() ? &it->second : nullptr;
    }

    /** Apply stored settings to a node in the given graph. Returns true if found. */
    bool applyToGraph (const juce::String& nodeId, ProcessingGraph& graph);

    /** Re-apply all stored settings to a freshly rebuilt graph. */
    void applyAllSettings (ProcessingGraph& graph);

private:
    std::unordered_map<juce::String, Settings> settings;
};
