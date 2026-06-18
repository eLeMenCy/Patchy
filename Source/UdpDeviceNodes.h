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
 * float (PAX_DATA_FLOAT); any other size is carried as a blob (PAX_DATA_BLOB,
 * truncated to 56 bytes — the inline capacity of PAX_Value::data).
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
                std::memcpy (&v.value, pkt.data.data(), 4);
            }
            else
            {
                v.dataType = PAX_DATA_BLOB;
                v.dataSize = static_cast<uint16_t> (std::min (pkt.size, (int) sizeof (v.data)));
                std::memcpy (v.data, pkt.data.data(), static_cast<size_t> (v.dataSize));
            }

            outputValues[static_cast<size_t> (outputValueCount++)] = v;
        }

        if (outputValueCount > 0)
            recordMidiActivity (outputValueCount);   // reuse activity counter for flash feedback

        bytesSinceLastPoll.fetch_add (0, std::memory_order_relaxed); // no-op keeps symbol referenced
    }

    int  port = 0;
    UdpMode mode = UdpMode::Unicast;
    juce::String multicastAddr;

    // Activity tracking for UI byte-rate label (message thread polls + resets)
    std::atomic<int> bytesSinceLastPoll { 0 };
    int  drainByteActivity() { return bytesSinceLastPoll.exchange (0, std::memory_order_relaxed); }

private:
    void run() override
    {
        static constexpr int kMaxPacket = 1500;
        std::array<uint8_t, kMaxPacket> buf;

        while (! threadShouldExit())
        {
            if (socket == nullptr) break;
            int ready = socket->waitUntilReady (true, 100);
            if (ready <= 0) continue; // timeout or error — loop and check threadShouldExit

            int bytesRead = socket->read (buf.data(), kMaxPacket, false);
            if (bytesRead <= 0) continue;

            ReceivedPacket pkt;
            pkt.size = std::min (bytesRead, (int) pkt.data.size());
            std::memcpy (pkt.data.data(), buf.data(), static_cast<size_t> (pkt.size));
            fifo.push (pkt);
            bytesSinceLastPoll.fetch_add (bytesRead, std::memory_order_relaxed);
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
 * doing it from the audio thread by queuing to a background thread).
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
                std::memcpy (pkt.data.data(), &v.value, 4);
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
        juce::WaitableEvent                  event { true };
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
