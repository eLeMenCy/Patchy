#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
#include "PortBindState.h"
#include <map>
#include <memory>
#include <mutex>
#include "SerialPort.h"
#include "../Pax/PaxAPI.h"
#include <algorithm>
#include <cstdio>
#include <vector>

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
    inline int buildOutputPacket (const uint8_t* dmx, int dmxLen, uint8_t* buf, int bufCap,
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
    // Restored 2026-08-31 (3rd pass) after briefly being removed entirely
    // — see openPort()'s own comment for the full story of why removing
    // it turned out to be wrong, once the complete spec was checked
    // rather than an earlier, incomplete search snippet.
    inline int buildEnableReceivePacket (uint8_t* buf, int bufCap)
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
    inline int buildGetParamsPacket (uint8_t* buf, int bufCap)
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
    inline int parseRxPacket (const uint8_t* buf, int len, uint8_t* dmxOut)
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
    // Real bug found and fixed 2026-08-31 (2nd pass) — the earlier fix to
    // this function's own read loop (see its own comment below) made
    // things WORSE, not better, for DmxInDeviceNode specifically: reading
    // for a much longer window increased the chance of this function
    // accidentally reading past its own expected reply and into the start
    // of a genuine, already-arriving DMX packet — the widget "always
    // sends" DMX by default per Enttec's own spec, so one could arrive at
    // any moment, including during this read. This function's own parser
    // only recognises its own expected reply label and silently discards
    // everything else, with no way for the caller to recover it — meaning
    // a real DMX packet's own start marker, if it happened to land here,
    // was gone for good, leaving only its own truncated tail for the
    // receive thread to find (with no way to recognise it as a packet at
    // all). This lines up precisely with the consistent, reproducible
    // headerless fragment seen in real hardware testing. Added an
    // optional output parameter so a caller with somewhere to put it
    // (DmxInDeviceNode's own receive buffer) can preserve whatever this
    // function read but didn't use, rather than it being silently lost —
    // DmxOutDeviceNode's own call site, with no such buffer to seed,
    // simply omits it and is entirely unaffected.
    inline bool detectIsMk2 (SerialPort& serial, std::vector<uint8_t>* leftoverOut = nullptr)
    {
        uint8_t req[5];
        if (buildGetParamsPacket (req, sizeof (req)) == 0) return false;
        int written = serial.write (req, 5);
        if (written != 5)
            juce::Logger::writeToLog ("EnttecPro: get-params write failed or incomplete ("
                                      + juce::String (written) + " of 5 bytes)");

        // Read reply — real bug fixed 2026-08-31: this loop used to stop
        // as soon as 8 bytes had arrived (`total < 8`), but the real
        // Enttec "Get Widget Parameters" reply (per the official API spec
        // v1.44) carries several more configuration fields beyond the
        // firmware version bytes this function actually needs — DMX
        // output break time, mark-after-break time, output rate, and
        // more — so the genuine, complete reply is very likely longer
        // than 8 bytes. Stopping early could leave part of that response
        // still arriving from the widget at the exact moment the caller
        // sends its own next command (openPort()'s own "enable receive"
        // packet, sent immediately after this function returns) —
        // investigated as a real, if unconfirmed, possible contributor to
        // a user report that DmxInDeviceNode never receives real DMX from
        // a physically-confirmed-working hardware chain (verified working
        // in QLC+ with the same cabling). Now keeps reading for the full
        // 300ms budget (or until the buffer fills), giving the parser
        // below its best chance at a complete, validated packet, rather
        // than handing it a possibly-truncated one.
        uint8_t resp[64] {};
        int total = 0;
        for (int attempt = 0; attempt < 6 && total < (int) sizeof (resp); ++attempt)
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

            // Preserve anything after this recognised packet's own EOM —
            // could be the start of a real, separate incoming packet.
            int consumedEnd = i + kHdrSize + dataLen + 1;   // +1 for EOM byte
            if (leftoverOut != nullptr && consumedEnd < total)
                leftoverOut->assign (resp + consumedEnd, resp + total);

            return fwMajor >= 2;   // Mk2 has major version 2+
        }

        // No recognised reply found at all — none of what was read could
        // be attributed to this function's own request, so preserve all
        // of it rather than silently discarding a potentially-real packet.
        if (leftoverOut != nullptr && total > 0)
            leftoverOut->assign (resp, resp + total);

        return false;   // no reply or unrecognised — assume Pro
    }

} // namespace EnttecProCodec

// ─────────────────────────────────────────────────────────────────────────────
/**
 * DmxSharedPort — v0.0.916 (2026-09-28)
 *
 * ONE open connection per Enttec interface (device path), shared by every
 * DMX In / DMX Out node using it — the same idea as ArtNetReceiver.
 *
 * Why:
 *  - Exclusivity: the port is opened with TIOCEXCL (SerialPort::open), so a
 *    second Patchy instance or another app gets a clear "in use" instead of
 *    both writing interleaved frames to the same interface. That only works
 *    if Patchy itself opens each interface exactly once — hence this class.
 *  - Two DMX Outs on one Pro Mk2 (port 1 + port 2) used to open the device
 *    twice and write independently, so their packets could interleave on
 *    the wire; writePacket() now sends each packet whole, under a lock.
 *  - Graph rebuilds: a new node just acquires the port its predecessor
 *    still holds, so the old stop-thread / transfer-fd dance is gone.
 *
 * Reading: one reader thread per interface, running while at least one
 * DMX In is attached; it parses every received frame and hands it to all
 * attached In nodes (each does its own change detection, so a node created
 * by a rebuild gets the current frame straight away). The first In also
 * sends the "enable receive" command (see DmxInDeviceNode history).
 *
 * Classic DMX USB Pro is HALF-DUPLEX (one DMX port whose direction flips
 * with the last command — Enttec API spec, quoted in the history below),
 * so In + Out on the same classic Pro can't work; hasHalfDuplexConflict()
 * lets both nodes show it. Pro Mk2: different ports, allowed.
 *
 * Lifetime: shared_ptr held by the nodes; the registry only keeps
 * weak_ptrs. Closed (reader stopped, port released) when the last node
 * lets go. Message thread for acquire/attach/detach.
 */
class DmxSharedPort : private juce::Thread
{
public:
    class InListener
    {
    public:
        virtual ~InListener() = default;
        /** Reader thread: every parsed 512-channel frame. */
        virtual void dmxFrameReceived (const uint8_t* dmx512) = 0;
        /** Reader thread: raw bytes read (byte-rate display). */
        virtual void dmxBytesReceived (int numBytes) = 0;
    };

    enum class OpenResult { ok, inUse, failed };

    /** Returns the shared port for devicePath, opening it if nobody holds
     *  it yet. nullptr on failure (result says whether it's held by another
     *  process). */
    static std::shared_ptr<DmxSharedPort> acquire (const juce::String& devicePath, OpenResult& result);

    ~DmxSharedPort() override;

    bool isMk2() const { return mk2; }

    /** Any thread. Writes one whole packet under a lock. */
    int writePacket (const uint8_t* data, int len);

    void addIn    (InListener* listener);   // first one sends enable-receive + starts the reader
    void removeIn (InListener* listener);   // after return, listener is never called again
    void addOut()    { outCount.fetch_add (1); }
    void removeOut() { outCount.fetch_sub (1); }

    bool hasHalfDuplexConflict() const
    {
        return ! mk2 && inCount.load() > 0 && outCount.load() > 0;
    }

private:
    explicit DmxSharedPort (const juce::String& path);
    void run() override;   // reader thread

    juce::String             devicePath;
    SerialPort               serial;
    bool                     mk2 = false;
    std::vector<uint8_t>     leftover;          // bytes detectIsMk2() read but didn't consume
    std::mutex               writeLock;
    std::mutex               inLock;            // guards ins; held while dispatching
    std::vector<InListener*> ins;
    std::atomic<int>         inCount  { 0 };
    std::atomic<int>         outCount { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DmxSharedPort)
};



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
                        private DmxSharedPort::InListener
{
public:
    explicit DmxInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi)
    {}

    ~DmxInDeviceNode() override { closePort(); }

    void configure (const juce::String& portPath)
    {
        closePort();
        devicePath = portPath;
        openPort();
    }

    // v0.0.916 (2026-09-28) — shared port. The old transfer (stop the old
    // node's reader thread, move its fd; history: 2026-09-01 "graph edits
    // reopened the port", 2026-09-02 reader-thread crash fix) is replaced
    // by DmxSharedPort: the new node acquires the port its predecessor
    // still holds, so a rebuild never reopens the interface. Returns true
    // when an old node existed (kept for the manager's call site).
    bool transferOrConfigure (const juce::String& portPath, DmxInDeviceNode* oldNode)
    {
        configure (portPath);
        return oldNode != nullptr;
    }

    // v0.0.916 — attach to the shared port. The enable-receive command,
    // its settling delay and the Mk2 detection moved into DmxSharedPort
    // (history of the enable-receive command: 2026-08-31, 3rd pass — the
    // classic Pro is half-duplex and needs it to switch its port to input).
    void openPort()
    {
        if (devicePath.isEmpty()) { openState.clear(); return; }

        DmxSharedPort::OpenResult result;
        port = DmxSharedPort::acquire (devicePath, result);
        if (port == nullptr)
        {
            if (openState.failed (result == DmxSharedPort::OpenResult::inUse ? 1 : 2))
                juce::Logger::writeToLog ("DmxInDeviceNode: failed to open " + devicePath
                                          + (result == DmxSharedPort::OpenResult::inUse
                                                 ? " (in use by another application) — retrying quietly"
                                                 : " — retrying quietly"));
            return;
        }
        openState.clear();
        isMk2.store (port->isMk2(), std::memory_order_relaxed);
        port->addIn (this);
        juce::Logger::writeToLog ("DmxInDeviceNode: listening on " + devicePath);
    }

    // v0.0.916 — message thread (WebBridge activity timer). See PortBindState.h.
    void retryOpenIfNeeded()
    {
        if (port == nullptr && openState.shouldRetry())
            openPort();
    }

    /** v0.0.916 — 0 fine, 1 interface in use by another app, 2 can't open,
     *  3 half-duplex conflict (In + Out on the same classic Pro). */
    int statusForUi() const
    {
        if (port != nullptr) return port->hasHalfDuplexConflict() ? 3 : 0;
        return openState.portInUseForUi();   // 1 / 2 after 1 s of failing
    }

    void closePort()
    {
        if (port != nullptr)
        {
            port->removeIn (this);   // no callback after this returns
            port.reset();
        }
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
    // v0.0.916 — called by the shared port's reader thread (was this
    // node's own run()). Change detection stays per node, so a node created
    // by a rebuild emits the current frame straight away.
    void dmxBytesReceived (int n) override
    {
        bytesSinceLastPoll.fetch_add (n, std::memory_order_relaxed);
    }

    void dmxFrameReceived (const uint8_t* dmx512) override
    {
        if (haveLastDmx && std::memcmp (dmx512, lastDmx.data(), 512) == 0) return;
        haveLastDmx = true;   // the very first frame always counts as a change
        std::memcpy (lastDmx.data(), dmx512, 512);
        // Don't call recordMidiActivity here — process() handles it at a
        // regular audio-block rate to avoid bursty flash jitter.
        ReceivedUniverse u;
        std::memcpy (u.dmx, dmx512, 512);
        fifo.push (u);
    }

    std::array<uint8_t, 512> lastDmx {};           // reader thread only
    bool                     haveLastDmx = false;  // reader thread only

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

    std::shared_ptr<DmxSharedPort> port;        // v0.0.916 — message thread
    PortBindState                  openState;   // v0.0.916 — 1 = in use, 2 = can't open

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

    // v0.0.916 (2026-09-28) — shared port; see DmxInDeviceNode's own
    // transferOrConfigure(). The new node's writer thread starts on the
    // port its predecessor still holds (no reopen); during the overlap both
    // may send, each packet whole under DmxSharedPort's write lock.
    bool transferOrConfigure (const juce::String& portPath, int universeToUse, DmxOutDeviceNode* oldNode)
    {
        configure (portPath, universeToUse);
        return oldNode != nullptr;
    }

    void openPort()
    {
        if (devicePath.isEmpty()) { openState.clear(); return; }

        DmxSharedPort::OpenResult result;
        port = DmxSharedPort::acquire (devicePath, result);
        if (port == nullptr)
        {
            if (openState.failed (result == DmxSharedPort::OpenResult::inUse ? 1 : 2))
                juce::Logger::writeToLog ("DmxOutDeviceNode: failed to open " + devicePath
                                          + (result == DmxSharedPort::OpenResult::inUse
                                                 ? " (in use by another application) — retrying quietly"
                                                 : " — retrying quietly"));
            return;
        }
        openState.clear();
        isMk2.store (port->isMk2(), std::memory_order_relaxed);
        port->addOut();
        juce::Logger::writeToLog ("DmxOutDeviceNode: sending on " + devicePath
                                  + " universe " + juce::String (universe));
        startThread (juce::Thread::Priority::normal);
    }

    // v0.0.916 — see DmxInDeviceNode's own retryOpenIfNeeded()/statusForUi().
    void retryOpenIfNeeded()
    {
        if (port == nullptr && openState.shouldRetry())
            openPort();
    }

    int statusForUi() const
    {
        if (port != nullptr) return port->hasHalfDuplexConflict() ? 3 : 0;
        return openState.portInUseForUi();
    }

    void closePort()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            sendQueue.signalDataAvailable();
            stopThread (1000);
        }
        if (port != nullptr)   // v0.0.916 — after the writer thread stopped
        {
            port->removeOut();
            port.reset();
        }
    }

    void process (int /*numSamples*/) override
    {
        if (inputDmxFrameValid)
        {
            sendQueue.push (inputDmxFrame);
            sendQueue.signalDataAvailable();

            // Lightweight Value mirror, added 2026-08-30 — this node
            // never populated outputValues[0] at all before, discovered
            // as a bonus finding while fixing DmxMonitorNode's own input-
            // handle CSS targeting the same day (see App.tsx and
            // Architecture.md) — that frontend fix alone wasn't enough to
            // actually light this node's own glow up, since the value it
            // reads was never being set here in the first place. Max
            // across all 512 received channels, same reasoning as every
            // other DMX/ArtNet mirror fixed the same day.
            PAX_Value v {};
            v.type     = PAX_TYPE_DMX;
            v.dataType = PAX_DATA_FLOAT;
            v.key      = (uint32_t) universe;
            v.value    = *std::max_element (inputDmxFrame.begin(), inputDmxFrame.end()) / 255.f;

            outputValues[0] = v;
            outputValueCount = 1;
        }
        else
        {
            // Visual-mirror-only reset, added 2026-08-30 (2nd pass) — same
            // reasoning as DmxMonitorNode's own fix (see that file's own
            // comment for the full story): without this, the port/edge
            // glow this mirror drives would stay stuck at its last real
            // value forever after disconnection, never dimming back down.
            //
            // Deliberately does NOT touch sendQueue at all — this node
            // sends to REAL physical DMX hardware, not just a UI display,
            // and whether a disconnected source should make a physical
            // fixture blackout versus hold its last commanded state is a
            // genuine, separate design/safety question with real live-show
            // implications (many real DMX systems deliberately hold last
            // state rather than auto-blackout on a glitch) — not something
            // to decide unilaterally as a side effect of a visual bug fix.
            // If a blackout-on-disconnect behaviour is ever wanted here,
            // it should be its own explicit, discussed decision.
            PAX_Value v {};
            v.type     = PAX_TYPE_DMX;
            v.dataType = PAX_DATA_FLOAT;
            v.key      = (uint32_t) universe;
            v.value    = 0.f;

            outputValues[0] = v;
            outputValueCount = 1;
        }
        // Deliberately no else branch — reverted 2026-08-30 (3rd pass),
        // after first adding one that reset only the visual mirror to
        // dark on disconnection while leaving sendQueue alone. That was
        // wrong: this node sends to REAL physical DMX hardware, and the
        // user's own explicit decision is hold-last-state on disconnect
        // — an unintended blackout mid-glitch is a genuine live-show
        // risk. Resetting only the glow while the physical fixture keeps
        // holding its last value would have been actively misleading in
        // the opposite direction — Patchy showing "nothing happening"
        // while a real light stays lit. So both sendQueue (never touched
        // at all) and outputValues[0] (simply not updated when
        // inputDmxFrameValid is false) now consistently hold their last
        // real value together, matching what the physical hardware is
        // actually still doing. Contrast with DmxMonitorNode's own fix,
        // which deliberately DOES reset to dark on disconnect — a
        // Monitor has no real hardware to protect, and its whole job is
        // honest, trustworthy diagnostics, not holding state.
    }

    juce::String      devicePath;
    int               universe = 0;
    std::atomic<bool> isMk2    { false };

private:
    void run() override
    {
        std::array<uint8_t, EnttecProCodec::kPktMax> pkt;
        std::array<uint8_t, 512> lastDmx {};
        bool haveLastDmx = false;   // v0.0.917 — see below

        while (! threadShouldExit())
        {
            std::array<uint8_t, 512> dmx;
            if (! sendQueue.pop (dmx))
            {
                sendQueue.waitForData (50);
                continue;
            }
            if (port == nullptr) continue;

            // Change detection — only send when values differ.
            // Fix, v0.0.917 (2026-09-29) — the FIRST frame is always sent:
            // lastDmx starts all-zero, so a node created by a rebuild (every
            // undo/redo) silently skipped an all-zero frame — e.g. undoing a
            // fader back to 0 left the fixture at its old value.
            if (haveLastDmx && std::memcmp (dmx.data(), lastDmx.data(), 512) == 0) continue;
            haveLastDmx = true;
            std::memcpy (lastDmx.data(), dmx.data(), 512);

            int len = EnttecProCodec::buildOutputPacket (dmx.data(), 512, pkt.data(), (int) pkt.size(), universe);
            if (len > 0)
                port->writePacket (pkt.data(), len);   // v0.0.916 — whole packet under the port's lock
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

    std::shared_ptr<DmxSharedPort> port;        // v0.0.916 — set/reset on the message thread while the writer thread is stopped
    PortBindState                  openState;   // v0.0.916 — 1 = in use, 2 = can't open

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

    bool applyToGraph (const juce::String& nodeId, ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);
    void applyAllSettings (ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);

private:
    std::unordered_map<juce::String, Settings> settings;
};
