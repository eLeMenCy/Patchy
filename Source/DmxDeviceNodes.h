#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
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
    // Restored 2026-08-31 (3rd pass) after briefly being removed entirely
    // — see openPort()'s own comment for the full story of why removing
    // it turned out to be wrong, once the complete spec was checked
    // rather than an earlier, incomplete search snippet.
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
    static bool detectIsMk2 (SerialPort& serial, std::vector<uint8_t>* leftoverOut = nullptr)
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

    // Real fix, 2026-09-01 (2nd pass) — the earlier attempt at this same
    // bug (a `portPath == devicePath` guard directly inside configure()
    // itself) was structurally unable to work at all: ProcessingGraph::
    // rebuild() destroys and recreates every node instance on every
    // single graph edit, anywhere in the whole graph, not just ones
    // involving this specific device. A freshly-constructed instance's
    // own devicePath is always at its own default (empty), never
    // carrying over from whatever instance came before it — so that
    // guard could never actually find a match, and every graph edit kept
    // paying the full serial-reopen cost regardless, exactly matching
    // the user's own follow-up report that the fix made no real
    // difference. The genuine fix has to transfer the actual, already-
    // open connection itself across the rebuild — mirroring the same
    // "transfer state across rebuild" mechanism this project already
    // uses successfully for DmxConsoleNode's own fader state (see
    // PatchyProcessor.cpp's own existing call site for the equivalent
    // pattern, extended to also cover this node type).
    //
    // Returns true if a genuine transfer happened (no reopen needed —
    // caller should NOT also call configure()), false if the caller
    // should fall through to a normal configure() (path genuinely
    // changed, or the old instance didn't have a working connection to
    // transfer in the first place).
    bool transferOrConfigure (const juce::String& portPath, DmxInDeviceNode* oldNode)
    {
        if (oldNode != nullptr && oldNode->devicePath == portPath && oldNode->serial.isOpen())
        {
            // Real crash found and fixed 2026-09-02 — same reasoning as
            // ArtNetInDeviceNode's own equivalent fix (see that file's own
            // comment for the full story): the old node's own receive
            // thread could still genuinely be running at the exact moment
            // this method transfers its connection out from under it.
            // Less immediately dangerous here than ArtNet's own case,
            // since a plain int file descriptor can't be null-dereferenced
            // the way a moved-from unique_ptr can — but two threads
            // simultaneously reading the same open connection is still
            // genuinely undefined, risky behaviour worth eliminating
            // outright rather than relying on it merely not crashing.
            // Deliberately does NOT call serial.close() to unblock a
            // pending read here (unlike closePort()'s own pattern) — that
            // would destroy the very connection this method exists to
            // transfer. Not needed anyway: this thread's own read already
            // uses a short 5ms timeout, so it naturally, promptly
            // rechecks threadShouldExit() on its own.
            //
            // Timeout reduced from 1000ms to 200ms, 2026-09-02 — found no
            // locking at all, anywhere in this project's own graph code,
            // between the message thread and the audio thread's own
            // concurrent, independent processing of the same live graph.
            // A full second of possible blocking here, on a thread this
            // one should exit from within single-digit milliseconds given
            // its own short polling interval, was needlessly generous —
            // 200ms is comfortably longer than any of these threads' own
            // longest wait cycle (ArtNet's own, at 100ms) while cutting
            // the worst case by 5x. Investigated as one contributor to a
            // user report of subtle audio-stream glitches during graph
            // edits — though the user's own follow-up clarified this
            // predates this session's own work, redirecting the primary
            // suspicion toward the pre-existing closeAllProtocolDeviceSockets()
            // mechanism instead (see ProcessingGraph.h's own comment).
            // Tightening this regardless, since it's a genuine, low-risk
            // improvement on its own merits either way.
            if (oldNode->isThreadRunning())
            {
                oldNode->signalThreadShouldExit();
                oldNode->stopThread (200);
            }

            serial.transferFrom (oldNode->serial);
            devicePath = portPath;
            isMk2.store (oldNode->isMk2.load(), std::memory_order_relaxed);
            // The receive thread itself belongs to this specific C++
            // object instance and can't be transferred — but the
            // connection it reads from now can be, so this simply starts
            // a fresh thread on the already-open, already-negotiated
            // connection rather than repeating the whole open/detect/
            // enable-receive/settle sequence from scratch.
            startThread (juce::Thread::Priority::normal);
            return true;
        }

        configure (portPath);
        return false;
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
        isMk2.store (EnttecProCodec::detectIsMk2 (serial, &pendingLeftoverBytes), std::memory_order_relaxed);
        juce::Logger::writeToLog ("DmxInDeviceNode: device is "
                                  + juce::String (isMk2.load() ? "Pro Mk2" : "Pro"));

        // "Enable receive on change" command — restored 2026-08-31 (3rd
        // pass), after removing it entirely (2nd pass) was ALSO confirmed
        // by real hardware testing not to fix this — the exact same
        // silence persisted even with it gone, which was itself the clue
        // that led to fetching Enttec's own COMPLETE API Spec v1.44 PDF
        // (not just the earlier, incomplete search snippet) for the full
        // picture. That revealed something the 2nd pass's own reasoning
        // had missed entirely: this classic (non-Mk2) widget doesn't have
        // independently-active input and output at all — it has a
        // single, half-duplex "DMX port direction" that only certain
        // commands switch. Per the spec's own wording for the "Output
        // Only Send DMX" command: "the periodic DMX packet output will
        // stop and the Widget DMX port direction will change to input
        // when the Widget receives any request message OTHER THAN the
        // Output Only Send DMX Packet request, OR THE GET WIDGET
        // PARAMETERS REQUEST" — meaning Get Widget Parameters (what
        // detectIsMk2() sends) is explicitly EXCLUDED from switching the
        // port to input. This "enable receive" command was the ONLY
        // other request this node ever sent — the one thing capable of
        // forcing the widget's own port direction back to input, if it
        // had ever been left in output mode by an earlier session (e.g.
        // a previous, differently-wired test, or QLC+). Removing it
        // entirely (2nd pass) likely removed the only mechanism able to
        // correct that, explaining why doing so made no difference at
        // all. The 1st pass's own original concern about this same
        // message's documented "reinitializes the DMX receive
        // processing" side effect is still worth respecting, though —
        // addressed here with a short settling delay before this node
        // starts actively listening, rather than by removing the command
        // this widget genuinely appears to need.
        uint8_t buf[16];
        int len = EnttecProCodec::buildEnableReceivePacket (buf, sizeof (buf));
        if (len > 0)
        {
            int written = serial.write (buf, len);
            if (written != len)
                juce::Logger::writeToLog ("DmxInDeviceNode: enable-receive write failed or incomplete ("
                                          + juce::String (written) + " of " + juce::String (len) + " bytes) on "
                                          + devicePath);
            else
                juce::Logger::writeToLog ("DmxInDeviceNode: enable-receive command sent (" + juce::String (written) + " bytes)");
        }
        else
        {
            juce::Logger::writeToLog ("DmxInDeviceNode: failed to build enable-receive packet");
        }

        // Settling delay — genuinely new in this pass, not present in
        // either earlier attempt. Gives the widget time to complete its
        // own documented reinitialization before this node's own receive
        // thread starts polling for a response, rather than racing it.
        juce::Thread::sleep (100);

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
    std::vector<uint8_t>     pendingLeftoverBytes;   // set by openPort(), consumed once by run() at its own startup
    void run() override
    {
        static constexpr int kBufSize = 600;
        std::array<uint8_t, kBufSize> rxBuf {};
        std::array<uint8_t, 512>      lastDmx {};
        std::array<uint8_t, 512>      dmxOut  {};
        int rxLen = 0;

        // Seed with anything detectIsMk2() read but didn't consume, rather
        // than starting this thread's own buffer empty and losing it —
        // see detectIsMk2()'s own comment for the full story.
        if (! pendingLeftoverBytes.empty())
        {
            int seedLen = std::min ((int) pendingLeftoverBytes.size(), kBufSize);
            std::memcpy (rxBuf.data(), pendingLeftoverBytes.data(), (size_t) seedLen);
            rxLen = seedLen;
        }

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

    // Real fix, 2026-09-01 (2nd pass) — same reasoning and same fix as
    // DmxInDeviceNode's own (see that file's own comment for the full
    // story). Also compares universe, since that can genuinely change
    // independently of the device path itself.
    bool transferOrConfigure (const juce::String& portPath, int universeToUse, DmxOutDeviceNode* oldNode)
    {
        if (oldNode != nullptr && oldNode->devicePath == portPath
            && oldNode->universe == universeToUse && oldNode->serial.isOpen())
        {
            // Real crash fix, 2026-09-02 — same reasoning as
            // DmxInDeviceNode's own (see that file's own comment for the
            // full story), using this thread's own correct wake-up
            // mechanism: it blocks on sendQueue, not a direct serial read,
            // matching this class's own closePort(). Timeout reduced from
            // 1000ms to 200ms, 2026-09-02 — see DmxInDeviceNode's own
            // equivalent comment for the full reasoning.
            if (oldNode->isThreadRunning())
            {
                oldNode->signalThreadShouldExit();
                oldNode->sendQueue.signalDataAvailable();
                oldNode->stopThread (200);
            }

            serial.transferFrom (oldNode->serial);
            devicePath = portPath;
            universe   = universeToUse;
            isMk2.store (oldNode->isMk2.load(), std::memory_order_relaxed);
            startThread (juce::Thread::Priority::normal);
            return true;
        }

        configure (portPath, universeToUse);
        return false;
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

    bool applyToGraph (const juce::String& nodeId, ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);
    void applyAllSettings (ProcessingGraph& graph, ProcessingGraph* oldGraph = nullptr);

private:
    std::unordered_map<juce::String, Settings> settings;
};
