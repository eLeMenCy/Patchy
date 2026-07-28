#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
#include "SerialPort.h"
#include "../Pax/PaxAPI.h"

class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
//  Enttec DMX USB Pro protocol codec — no external dependencies.
//
//  Packet format:
//    0x7E          Start of message
//    Label         Message type (1 byte)
//    LengthLSB     Data length low byte
//    LengthMSB     Data length high byte
//    Data[]        0–600 bytes
//    0xE7          End of message
//
//  Key labels used:
//    3  = Get Widget Parameters (sent on open to verify device)
//    4  = Get Widget Parameters Reply
//    6  = Output Only Send DMX Packet    ← DMX Out
//    8  = Receive DMX On Change          ← enable DMX In
//    5  = Received DMX Packet            ← incoming DMX data
// ─────────────────────────────────────────────────────────────────────────────
namespace EnttecProCodec
{
    static constexpr uint8_t kSom           = 0x7E;   // Start of message
    static constexpr uint8_t kEom           = 0xE7;   // End of message
    static constexpr uint8_t kLabelOut      = 6;       // Send DMX output frame (universe 0 / Pro)
    static constexpr uint8_t kLabelOut2     = 202;     // Send DMX output frame (universe 1 / Pro Mk2)
    static constexpr uint8_t kLabelIn       = 8;       // Enable receive DMX on change
    static constexpr uint8_t kLabelRx       = 5;       // Received DMX packet
    static constexpr uint8_t kLabelGetParams = 3;      // Get widget parameters
    static constexpr int     kMaxDmx        = 512;
    static constexpr int     kHdrSize       = 4;       // SOM + label + 2× length
    static constexpr int     kPktMax        = kHdrSize + 1 + kMaxDmx + 1; // +1 start code, +1 EOM

    // ── Build an output DMX packet ────────────────────────────────────────────
    /** Serialise 512 DMX bytes into an Enttec Pro output packet.
     *  universe: 0 = label 6 (Pro / Mk2 port 1), 1 = label 202 (Mk2 port 2 only)
     *  buf must be >= kPktMax bytes. Returns total packet length. */
    static int buildOutputPacket (const uint8_t* dmx, int dmxLen, uint8_t* buf, int bufCap,
                                  int universe = 0)
    {
        // Data = start code (0x00) + dmx channels
        int dataLen = 1 + std::min (dmxLen, kMaxDmx);
        if (bufCap < kHdrSize + dataLen + 1) return 0;

        buf[0] = kSom;
        buf[1] = (universe == 1) ? kLabelOut2 : kLabelOut;
        buf[2] = (uint8_t)  (dataLen & 0xFF);
        buf[3] = (uint8_t) ((dataLen >> 8) & 0xFF);
        buf[4] = 0x00;   // DMX start code

        int copyLen = std::min (dmxLen, kMaxDmx);
        if (copyLen > 0 && dmx != nullptr)
            std::memcpy (buf + 5, dmx, (size_t) copyLen);
        if (copyLen < kMaxDmx)
            std::memset (buf + 5 + copyLen, 0, (size_t) (kMaxDmx - copyLen));

        buf[kHdrSize + dataLen] = kEom;
        return kHdrSize + dataLen + 1;
    }

    // ── Build "enable receive" packet ─────────────────────────────────────────
    static int buildEnableReceivePacket (uint8_t* buf, int bufCap)
    {
        if (bufCap < 6) return 0;
        // Label 8, data = 1 byte: 0x00 = Send always (not just on change)
        // Per Enttec spec: 0 = send always, 1 = send on data change only
        buf[0] = kSom;
        buf[1] = kLabelIn;
        buf[2] = 0x01; buf[3] = 0x00;   // data length = 1
        buf[4] = 0x00;                    // 0x00 = Send always
        buf[5] = kEom;
        return 6;
    }

    // ── Build "get widget params" packet ──────────────────────────────────────
    static int buildGetParamsPacket (uint8_t* buf, int bufCap)
    {
        if (bufCap < 5) return 0;
        buf[0] = kSom;
        buf[1] = kLabelGetParams;
        buf[2] = 0x00; buf[3] = 0x00;
        buf[4] = kEom;
        return 5;
    }

    // ── Parse an incoming packet from the Pro ────────────────────────────────
    /** Scan buf[0..len) for a complete label-5 (Received DMX) packet.
     *  On success, fills dmxOut (512 bytes) and returns the number of DMX channels.
     *  Returns 0 if no complete packet found yet.
     *
     *  Label 5 data layout (per Enttec API spec v1.44):
     *    Byte 0:     Status byte (bit0=overflow, bit1=overrun) — skip
     *    Byte 1:     DMX start code (normally 0x00) — skip
     *    Bytes 2..N: DMX channel data (channels 1..N-1)
     */
    static int parseRxPacket (const uint8_t* buf, int len, uint8_t* dmxOut)
    {
        for (int i = 0; i < len - kHdrSize; ++i)
        {
            if (buf[i] != kSom) continue;
            if (buf[i + 1] != kLabelRx) continue;

            int dataLen = buf[i + 2] | (buf[i + 3] << 8);
            if (dataLen < 2 || dataLen > kMaxDmx + 2) continue;

            int pktEnd = i + kHdrSize + dataLen;
            if (pktEnd >= len) continue;   // incomplete
            if (buf[pktEnd] != kEom)      continue;   // corrupt

            // Data: byte 0 = status (skip), byte 1 = start code (skip),
            //       bytes 2..dataLen-1 = DMX channels 1..N
            int dmxLen = dataLen - 2;   // subtract status byte + start code
            if (dmxLen <= 0) continue;
            dmxLen = std::min (dmxLen, kMaxDmx);

            const uint8_t* dmx = buf + i + kHdrSize + 2;  // skip status + start code
            std::memcpy (dmxOut, dmx, (size_t) dmxLen);
            if (dmxLen < kMaxDmx)
                std::memset (dmxOut + dmxLen, 0, (size_t) (kMaxDmx - dmxLen));
            return dmxLen;
        }
        return 0;
    }

    // ── Detect Mk2 by querying widget parameters ──────────────────────────────
    /** Send Label 3 (Get Widget Parameters), read reply, return true if Mk2.
     *  Call this after opening the serial port, before starting the receive thread.
     *  The Mk2 has firmware major version >= 2. */
    static bool detectIsMk2 (SerialPort& serial)
    {
        uint8_t req[5];
        if (buildGetParamsPacket (req, sizeof (req)) == 0) return false;
        serial.write (req, 5);

        // Read reply — up to 48 bytes, 300ms timeout
        uint8_t resp[64] {};
        int total = 0;
        for (int attempt = 0; attempt < 6 && total < 8; ++attempt)
        {
            int n = serial.read (resp + total, (int) sizeof (resp) - total, 50);
            if (n > 0) total += n;
        }

        // Find SOM + label 3 or 4 reply
        for (int i = 0; i < total - 6; ++i)
        {
            if (resp[i] != kSom) continue;
            // Label 3 (echo) or 4 (reply)
            if (resp[i + 1] != 3 && resp[i + 1] != 4) continue;
            int dataLen = resp[i + 2] | (resp[i + 3] << 8);
            if (dataLen < 4) continue;
            if (i + kHdrSize + dataLen >= total) continue;
            // Byte 0 = firmware LSB, byte 1 = firmware MSB
            uint8_t fwMajor = resp[i + kHdrSize + 1];   // MSB = major version
            juce::Logger::writeToLog ("EnttecPro: firmware version "
                                      + juce::String (resp[i + kHdrSize]) + "."
                                      + juce::String (fwMajor));
            return fwMajor >= 2;   // Mk2 has major version 2+
        }
        return false;   // no reply or unrecognised — assume Pro
    }

} // namespace EnttecProCodec


// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxInDeviceNode  (nodeType 14)
 *
 * Receives DMX512 from an Enttec DMX USB Pro interface and emits the full
 * 512-channel universe every block via the dedicated DMX frame path
 * (PaxAPI.h v4 — outputDmxFrame/outputDmxFrameValid), plus a lightweight
 * PAX_Value mirror alongside it (type=PAX_TYPE_DMX, dataType=FLOAT,
 * value=channel[0]/255, no blob) for anything that only wants a plain
 * scalar. The frame is the real payload now — the old approach of cramming
 * the universe into PAX_Value.data[] silently truncated at 56 of 512
 * channels; see Architecture.md for the discovery and the fix.
 */
class DmxInDeviceNode : public NodeProcessor,
                        private juce::Thread
{
public:
    explicit DmxInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("DmxIn_" + nodeId)
    {}

    ~DmxInDeviceNode() override { closePort(); }

    void configure (const juce::String& portPath)
    {
        closePort();
        devicePath = portPath;
        openPort();
    }

    void openPort()
    {
        if (devicePath.isEmpty()) return;

        if (! serial.open (devicePath.toStdString(), 57600, 8, 'N', 2))
        {
            juce::Logger::writeToLog ("DmxInDeviceNode: failed to open " + devicePath);
            return;
        }

        // Detect device type before starting receive thread
        isMk2.store (EnttecProCodec::detectIsMk2 (serial), std::memory_order_relaxed);
        juce::Logger::writeToLog ("DmxInDeviceNode: device is "
                                  + juce::String (isMk2.load() ? "Pro Mk2" : "Pro"));

        // Send "enable receive on change" command
        uint8_t buf[16];
        int len = EnttecProCodec::buildEnableReceivePacket (buf, sizeof (buf));
        if (len > 0) serial.write (buf, len);

        startThread (juce::Thread::Priority::normal);
        juce::Logger::writeToLog ("DmxInDeviceNode: listening on " + devicePath);
    }

    void closePort()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            if (serial.isOpen()) serial.close();
            stopThread (1000);
        }
        serial.close();
    }

    void process (int /*numSamples*/) override
    {
        // Drain the entire FIFO but only keep the latest frame —
        // serial arrives in bursts; we want the most recent value,
        // not a queue of stale ones that cause jump artifacts.
        ReceivedUniverse u;
        bool gotNew = false;
        while (fifo.pop (u))
        {
            std::memcpy (lastReceived.data(), u.dmx, 512);
            gotNew = true;
        }

        if (gotNew) recordMidiActivity (1);

        // Always emit the last known frame at audio-block rate —
        // gives downstream Monitor a steady ~86Hz supply instead
        // of bursty serial packets (44Hz with USB jitter).
        if (hasReceived || gotNew)
        {
            hasReceived = true;

            // Full universe via the dedicated DMX frame path (API v4) — the
            // real payload now, addresses all 512 channels, not just 56.
            outputDmxFrame      = lastReceived;
            outputDmxFrameValid = true;

            // Lightweight Value mirror alongside it — no blob, channel[0]
            // only, purely for anything that expects a plain scalar on this
            // edge (e.g. a future DMX→Value adapter). Real channel data no
            // longer travels via PAX_Value; DmxOutDeviceNode/DmxMonitorNode
            // read the frame above, not this.
            PAX_Value v {};
            v.type     = PAX_TYPE_DMX;
            v.dataType = PAX_DATA_FLOAT;
            v.key      = 0;
            v.value    = lastReceived[0] / 255.f;
            outputValues[0]  = v;
            outputValueCount = 1;
        }
        else
        {
            outputValueCount = 0;
        }
    }

    std::atomic<int>  bytesSinceLastPoll { 0 };
    int drainByteActivity() { return bytesSinceLastPoll.exchange (0, std::memory_order_relaxed); }

    std::atomic<bool> isMk2 { false };
    juce::String      devicePath;

private:
    std::array<uint8_t, 512> lastReceived {};
    bool                     hasReceived = false;
    void run() override
    {
        static constexpr int kBufSize = 600;
        std::array<uint8_t, kBufSize> rxBuf {};
        std::array<uint8_t, 512>      lastDmx {};
        std::array<uint8_t, 512>      dmxOut  {};
        int rxLen = 0;

        while (! threadShouldExit())
        {
            if (! serial.isOpen()) break;

            uint8_t tmp[64];
            int n = serial.read (tmp, sizeof (tmp), 5);  // 5ms timeout — DMX frame = 22ms
            if (n <= 0) continue;

            bytesSinceLastPoll.fetch_add (n, std::memory_order_relaxed);

            // Append to receive buffer
            int copyLen = std::min (n, kBufSize - rxLen);
            if (copyLen > 0)
            {
                std::memcpy (rxBuf.data() + rxLen, tmp, (size_t) copyLen);
                rxLen += copyLen;
            }

            // Try to parse a complete packet
            int channels = EnttecProCodec::parseRxPacket (rxBuf.data(), rxLen, dmxOut.data());
            if (channels > 0)
            {
                // Change detection — only emit on actual value changes
                if (std::memcmp (dmxOut.data(), lastDmx.data(), 512) != 0)
                {
                    std::memcpy (lastDmx.data(), dmxOut.data(), 512);
                    // Don't call recordMidiActivity here — process() handles it
                    // at a regular audio-block rate to avoid bursty flash jitter
                    ReceivedUniverse u;
                    std::memcpy (u.dmx, dmxOut.data(), 512);
                    fifo.push (u);
                }
                rxLen = 0;   // consumed — reset buffer
            }
            else if (rxLen >= kBufSize)
            {
                rxLen = 0;   // overflow — discard and resync
            }
        }
    }

    struct ReceivedUniverse { uint8_t dmx[512] {}; };

    static constexpr int kFifoSize = 8;
    struct UniverseFifo
    {
        void push (const ReceivedUniverse& u)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) items[static_cast<size_t> (s1)] = u;
            fifo.finishedWrite (n1);
        }
        bool pop (ReceivedUniverse& u)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            u = items[static_cast<size_t> (s1)];
            fifo.finishedRead (n1);
            return true;
        }
        juce::AbstractFifo                         fifo { kFifoSize };
        std::array<ReceivedUniverse, kFifoSize>    items;
    } fifo;

    SerialPort serial;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxInDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxOutDeviceNode  (nodeType 15)
 *
 * Receives full 512-channel DMX universes via the dedicated DMX frame path
 * (PaxAPI.h v4 — inputDmxFrame/inputDmxFrameValid) and sends them as
 * DMX512 frames via an Enttec DMX USB Pro interface. Only sends when data
 * changes (memcmp vs last sent frame). No longer reads PAX_Value at all
 * for the channel payload — see Architecture.md for why (the old blob
 * approach silently truncated at 56 of 512 channels).
 */
class DmxOutDeviceNode : public NodeProcessor,
                         private juce::Thread
{
public:
    explicit DmxOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("DmxOut_" + nodeId)
    {}

    ~DmxOutDeviceNode() override { closePort(); }

    void configure (const juce::String& portPath, int universeToUse = 0)
    {
        closePort();
        devicePath = portPath;
        universe   = universeToUse;
        openPort();
    }

    void openPort()
    {
        if (devicePath.isEmpty()) return;

        if (! serial.open (devicePath.toStdString(), 57600, 8, 'N', 2))
        {
            juce::Logger::writeToLog ("DmxOutDeviceNode: failed to open " + devicePath);
            return;
        }

        // Detect device type
        isMk2.store (EnttecProCodec::detectIsMk2 (serial), std::memory_order_relaxed);
        juce::Logger::writeToLog ("DmxOutDeviceNode: device is "
                                  + juce::String (isMk2.load() ? "Pro Mk2" : "Pro")
                                  + " on " + devicePath
                                  + " universe " + juce::String (universe));

        startThread (juce::Thread::Priority::normal);
    }

    void closePort()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            sendQueue.signalDataAvailable();
            stopThread (1000);
        }
        serial.close();
    }

    void process (int /*numSamples*/) override
    {
        if (inputDmxFrameValid)
        {
            sendQueue.push (inputDmxFrame);
            sendQueue.signalDataAvailable();
        }
    }

    juce::String      devicePath;
    int               universe = 0;
    std::atomic<bool> isMk2    { false };

private:
    void run() override
    {
        std::array<uint8_t, EnttecProCodec::kPktMax> pkt;
        std::array<uint8_t, 512> lastDmx {};

        while (! threadShouldExit())
        {
            std::array<uint8_t, 512> dmx;
            if (! sendQueue.pop (dmx))
            {
                sendQueue.waitForData (50);
                continue;
            }
            if (! serial.isOpen()) continue;

            // Change detection — only send when values differ
            if (std::memcmp (dmx.data(), lastDmx.data(), 512) == 0) continue;
            std::memcpy (lastDmx.data(), dmx.data(), 512);

            int len = EnttecProCodec::buildOutputPacket (dmx.data(), 512, pkt.data(), (int) pkt.size(), universe);
            if (len > 0)
                serial.write (pkt.data(), len);
        }
    }

    static constexpr int kFifoSize = 16;
    struct SendFifo
    {
        void push (const std::array<uint8_t, 512>& frame)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) frames[static_cast<size_t> (s1)] = frame;
            fifo.finishedWrite (n1);
        }
        bool pop (std::array<uint8_t, 512>& frame)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            frame = frames[static_cast<size_t> (s1)];
            fifo.finishedRead (n1);
            return true;
        }
        void signalDataAvailable() { event.signal(); }
        void waitForData (int ms)  { event.wait (ms); }

        juce::AbstractFifo                            fifo { kFifoSize };
        std::array<std::array<uint8_t, 512>, kFifoSize> frames;
        juce::WaitableEvent                            event { false };  // auto-reset: resets after each wait()
    } sendQueue;

    SerialPort serial;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxOutDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxDeviceManager
 *
 * Owned by PatchyProcessor. Tracks per-node DMX device path and reapplies
 * after graph rebuilds — same pattern as ArtNetDeviceManager.
 */
class DmxDeviceManager
{
public:
    struct Settings
    {
        juce::String devicePath;
        int          universe = 0;   // 0 = Pro / Mk2 port 1, 1 = Mk2 port 2
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
