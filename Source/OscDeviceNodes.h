#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include "NodeProcessor.h"
#include "../Pax/PaxAPI.h"

// Forward declare to avoid circular include
class ProcessingGraph;

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal OSC 1.0 parser / serialiser — no external dependencies.
//
//  OSC packet layout (binary, big-endian, all strings/blobs padded to 4 bytes):
//
//    Address pattern   null-terminated string  e.g. "/volume\0"
//    Type tag string   null-terminated string  e.g. ",f\0\0"
//    Arguments         packed per type tag
//
//  Supported argument types (superset of what we map to PAX_Value):
//    f  float32      → PAX_DATA_FLOAT  (v.value)
//    i  int32        → PAX_DATA_FLOAT  (cast to float, v.value)
//    s  OSC-string   → PAX_DATA_BLOB   (v.data, null-terminated)
//    b  blob         → PAX_DATA_BLOB   (v.data, raw bytes)
//    T  True         → PAX_DATA_FLOAT  (v.value = 1.f)
//    F  False        → PAX_DATA_FLOAT  (v.value = 0.f)
//    (anything else is skipped)
//
//  We carry the OSC address pattern in PAX_Value::data so downstream Pax can
//  filter by address; v.key is a djb2 hash of the address for fast comparison.
// ─────────────────────────────────────────────────────────────────────────────
namespace OscCodec
{
    // ── djb2 hash — fast address key ─────────────────────────────────────────
    static inline uint32_t hashAddress (const char* s)
    {
        uint32_t h = 5381u;
        while (*s) h = ((h << 5) + h) + (uint8_t) *s++;
        return h;
    }

    // ── OSC-string helpers ────────────────────────────────────────────────────
    static inline int alignUp4 (int n) { return (n + 3) & ~3; }

    /** Read a null-terminated OSC string from buf at offset.
     *  Returns pointer to start; advances offset past the padded string. */
    static inline const char* readString (const uint8_t* buf, int bufLen,
                                          int& offset)
    {
        if (offset >= bufLen) return nullptr;
        const char* s = reinterpret_cast<const char*> (buf + offset);
        int len = 0;
        while (offset + len < bufLen && buf[offset + len] != 0) ++len;
        offset += alignUp4 (len + 1);
        return s;
    }

    /** Read a big-endian int32 from buf at offset. */
    static inline int32_t readInt32 (const uint8_t* buf, int bufLen, int& offset)
    {
        if (offset + 4 > bufLen) return 0;
        int32_t v = (int32_t) (((uint32_t) buf[offset]     << 24) |
                                ((uint32_t) buf[offset + 1] << 16) |
                                ((uint32_t) buf[offset + 2] <<  8) |
                                 (uint32_t) buf[offset + 3]);
        offset += 4;
        return v;
    }

    /** Read a big-endian float32 from buf at offset. */
    static inline float readFloat (const uint8_t* buf, int bufLen, int& offset)
    {
        int32_t raw = readInt32 (buf, bufLen, offset);
        float f;
        std::memcpy (&f, &raw, 4);
        return f;
    }

    // ── Parse one OSC message into a PAX_Value ────────────────────────────────
    /** Returns true if parsing succeeded and out is populated. */
    static bool parse (const uint8_t* buf, int len, PAX_Value& out)
    {
        if (len < 8 || buf[0] != '/') return false;   // must start with '/'

        int offset = 0;
        const char* address = readString (buf, len, offset);
        if (address == nullptr || offset >= len) return false;

        const char* typeTags = readString (buf, len, offset);
        if (typeTags == nullptr || typeTags[0] != ',') return false;

        // Store address in data[] for downstream filtering
        out = PAX_Value{};
        out.key      = hashAddress (address);
        out.type     = PAX_TYPE_OSC;
        out.dataType = PAX_DATA_BLOB;

        // Copy address into data (null-terminated, up to capacity)
        size_t addrLen = std::min (std::strlen (address), sizeof (out.data) - 1);
        std::memcpy (out.data, address, addrLen);
        out.data[addrLen] = 0;
        out.dataSize = static_cast<uint16_t> (addrLen + 1);

        // Use the first argument as the primary value
        const char* tag = typeTags + 1;   // skip ','
        bool gotValue = false;

        for (; *tag != '\0'; ++tag)
        {
            switch (*tag)
            {
                case 'f':
                    if (! gotValue)
                    {
                        out.value    = readFloat (buf, len, offset);
                        out.dataType = PAX_DATA_FLOAT;
                        gotValue     = true;
                    }
                    else { offset += 4; }
                    break;

                case 'i':
                    if (! gotValue)
                    {
                        out.value    = static_cast<float> (readInt32 (buf, len, offset));
                        out.dataType = PAX_DATA_FLOAT;
                        gotValue     = true;
                    }
                    else { offset += 4; }
                    break;

                case 's':
                {
                    if (! gotValue)
                    {
                        // String arg — append to data after address (space-separated)
                        const char* s = readString (buf, len, offset);
                        if (s)
                        {
                            // Write string value into data (overwrite address, keep it simple)
                            size_t sLen = std::min (std::strlen (s), sizeof (out.data) - 1);
                            std::memcpy (out.data, s, sLen);
                            out.data[sLen] = 0;
                            out.dataSize   = static_cast<uint16_t> (sLen + 1);
                            out.dataType   = PAX_DATA_BLOB;
                            gotValue       = true;
                        }
                    }
                    else
                    {
                        // skip
                        int dummy = offset;
                        readString (buf, len, dummy);
                        offset = dummy;
                    }
                    break;
                }

                case 'b':
                {
                    int32_t blobLen = readInt32 (buf, len, offset);
                    if (! gotValue && blobLen > 0)
                    {
                        uint16_t copyLen = static_cast<uint16_t> (
                            std::min ((int32_t) sizeof (out.data), blobLen));
                        std::memcpy (out.data, buf + offset, copyLen);
                        out.dataSize = copyLen;
                        out.dataType = PAX_DATA_BLOB;
                        gotValue     = true;
                    }
                    offset += alignUp4 (blobLen);
                    break;
                }

                case 'T':
                    if (! gotValue) { out.value = 1.f; out.dataType = PAX_DATA_FLOAT; gotValue = true; }
                    break;
                case 'F':
                    if (! gotValue) { out.value = 0.f; out.dataType = PAX_DATA_FLOAT; gotValue = true; }
                    break;

                default: break;   // skip unknown tags (N, I, h, d, c, r, m, t)
            }
        }

        return true;   // valid OSC message even if no typed args
    }

    // ── Serialise a PAX_Value to an OSC message ───────────────────────────────
    /** Writes an OSC message to buf. Returns byte count, or 0 on failure.
     *  address must be a valid OSC address string (starts with '/').
     *  buf must be at least 128 bytes. */
    static int serialise (const char* address, const PAX_Value& v,
                          uint8_t* buf, int bufCap)
    {
        auto writeString = [&](const char* s, int& pos) -> bool
        {
            int len = (int) std::strlen (s) + 1;   // include null
            int padded = alignUp4 (len);
            if (pos + padded > bufCap) return false;
            std::memcpy (buf + pos, s, (size_t) len);
            std::memset (buf + pos + len, 0, (size_t) (padded - len));
            pos += padded;
            return true;
        };

        auto writeInt32 = [&](int32_t val, int& pos) -> bool
        {
            if (pos + 4 > bufCap) return false;
            buf[pos]     = (uint8_t) (val >> 24);
            buf[pos + 1] = (uint8_t) (val >> 16);
            buf[pos + 2] = (uint8_t) (val >>  8);
            buf[pos + 3] = (uint8_t)  val;
            pos += 4;
            return true;
        };

        auto writeFloat = [&](float val, int& pos) -> bool
        {
            int32_t raw; std::memcpy (&raw, &val, 4);
            return writeInt32 (raw, pos);
        };

        int pos = 0;
        if (! writeString (address, pos)) return 0;

        if (v.dataType == PAX_DATA_FLOAT)
        {
            if (! writeString (",f", pos))  return 0;
            if (! writeFloat  (v.value, pos)) return 0;
        }
        else
        {
            // Send blob data
            if (! writeString (",b", pos)) return 0;
            int32_t blobLen = v.dataSize;
            if (! writeInt32 (blobLen, pos)) return 0;
            int padded = alignUp4 (blobLen);
            if (pos + padded > bufCap) return 0;
            std::memcpy (buf + pos, v.data, (size_t) blobLen);
            std::memset (buf + pos + blobLen, 0, (size_t) (padded - blobLen));
            pos += padded;
        }
        return pos;
    }

    // ── Decode ALL arguments for display purposes (Monitor use only) ─────────
    /** Unlike parse(), this never collapses to a single PAX_Value — every
     *  argument is kept so the OSC Monitor can show the complete message.
     *  Returns true if the packet is valid OSC (even with zero args). */
    static bool decodeForMonitor (const uint8_t* buf, int len,
                                  juce::String& outAddress,
                                  juce::String& outTypeTags,
                                  juce::String& outArgsDisplay)
    {
        if (len < 8 || buf[0] != '/') return false;

        int offset = 0;
        const char* address = readString (buf, len, offset);
        if (address == nullptr || offset >= len) return false;

        const char* typeTags = readString (buf, len, offset);
        if (typeTags == nullptr || typeTags[0] != ',') return false;

        outAddress  = juce::String (address);
        outTypeTags = juce::String (typeTags + 1);

        juce::String args;
        const char* tag = typeTags + 1;
        for (; *tag != '\0'; ++tag)
        {
            if (! args.isEmpty()) args += "  ";
            switch (*tag)
            {
                case 'f': args += juce::String (readFloat (buf, len, offset), 4); break;
                case 'i': args += juce::String (readInt32 (buf, len, offset)); break;
                case 's':
                {
                    const char* s = readString (buf, len, offset);
                    args += s != nullptr ? juce::String (s) : juce::String();
                    break;
                }
                case 'b':
                {
                    int32_t blobLen = readInt32 (buf, len, offset);
                    offset += alignUp4 (blobLen);
                    args += "[blob " + juce::String (blobLen) + "B]";
                    break;
                }
                case 'T': args += "true";  break;
                case 'F': args += "false"; break;
                default:  break;   // N, I, h, d, c, r, m, t — no payload to show
            }
        }
        outArgsDisplay = args;
        return true;
    }

} // namespace OscCodec

// ── Raw decoded message — full detail, used only for OSC Monitor display ────
struct RawOscMessage
{
    juce::String address;
    juce::String typeTags;
    juce::String argsDisplay;
    int          byteCount = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * OscInDeviceNode  (nodeType 10)
 *
 * 1 OSC output port — feeds parsed OSC messages as PAX_Value events to
 * downstream graph nodes. Listens on a UDP port on a background thread,
 * parses incoming OSC 1.0 messages, and pushes results into a lock-free FIFO.
 * process() drains the FIFO once per audio block.
 *
 * PAX_Value mapping:
 *   key      = djb2 hash of OSC address pattern (e.g. hash("/volume"))
 *   type     = PAX_TYPE_OSC
 *   dataType = PAX_DATA_FLOAT for f/i/T/F args, PAX_DATA_BLOB for s/b
 *   value    = first numeric argument (if any)
 *   data[]   = OSC address string (null-terminated) or string/blob arg
 */
class OscInDeviceNode : public NodeProcessor,
                        private juce::Thread
{
public:
    explicit OscInDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("OscIn_" + nodeId)
    {}

    ~OscInDeviceNode() override { closeSocket(); }

    void configure (int portToUse)
    {
        closeSocket();
        port = portToUse;
        openSocket();
    }

    void openSocket()
    {
        if (port <= 0) return;

        socket = std::make_unique<juce::DatagramSocket> (false);

        if (! socket->bindToPort (port))
        {
            juce::Logger::writeToLog ("OscInDeviceNode: failed to bind port "
                                      + juce::String (port));
            socket.reset();
            return;
        }

        juce::Logger::writeToLog ("OscInDeviceNode: listening on port "
                                  + juce::String (port));
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
        ParsedMessage msg;
        while (outputValueCount < kMaxValueEvents && fifo.pop (msg))
        {
            outputValues[static_cast<size_t> (outputValueCount++)] = msg.value;
        }

        if (outputValueCount > 0)
            recordMidiActivity (outputValueCount);

        bytesSinceLastPoll.fetch_add (0, std::memory_order_relaxed);

        // Drain full-detail raw messages for the OSC Monitor (if connected).
        // Held as a member (not consumed) so every downstream Monitor edge
        // can read this block's messages — mirrors how outputMidi works.
        lastRawMessages.clear();
        RawMessage raw;
        while (monitorFifo.pop (raw))
            lastRawMessages.push_back (raw.msg);
    }

    int port = 0;

    std::atomic<int> bytesSinceLastPoll { 0 };
    int drainByteActivity() { return bytesSinceLastPoll.exchange (0, std::memory_order_relaxed); }

    // Full-detail decoded messages from this block — read by ProcessingGraph
    // when routing into an OscMonitorNode. Not lock-free (message-thread-safe
    // read of a value refreshed once per process() call).
    std::vector<RawOscMessage> lastRawMessages;

private:
    void run() override
    {
        static constexpr int kMaxPacket = 1500;
        std::array<uint8_t, kMaxPacket> buf;

        while (! threadShouldExit())
        {
            if (socket == nullptr) break;
            int ready = socket->waitUntilReady (true, 100);
            if (ready <= 0) continue;

            int bytesRead = socket->read (buf.data(), kMaxPacket, false);
            if (bytesRead <= 0) continue;

            bytesSinceLastPoll.fetch_add (bytesRead, std::memory_order_relaxed);

            PAX_Value v {};
            if (OscCodec::parse (buf.data(), bytesRead, v))
            {
                ParsedMessage msg;
                msg.value = v;
                fifo.push (msg);
            }

            // Separate full-detail decode for the Monitor — independent of the
            // collapsed PAX_Value above, never lossy on multi-arg messages.
            RawOscMessage rawMsg;
            if (OscCodec::decodeForMonitor (buf.data(), bytesRead,
                                            rawMsg.address, rawMsg.typeTags, rawMsg.argsDisplay))
            {
                rawMsg.byteCount = bytesRead;
                RawMessage rm;
                rm.msg = rawMsg;
                monitorFifo.push (rm);
            }
        }
    }

    struct ParsedMessage { PAX_Value value {}; };

    static constexpr int kFifoSize = 256;
    struct MessageFifo
    {
        void push (const ParsedMessage& m)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) messages[static_cast<size_t> (s1)] = m;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (ParsedMessage& m)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            m = messages[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        juce::AbstractFifo                             fifo { kFifoSize };
        std::array<ParsedMessage, kFifoSize>           messages;
    } fifo;

    // Separate small FIFO for full-detail Monitor messages (juce::String
    // members mean this isn't lock-free, but it's only ever touched by the
    // socket thread (push) and the message-thread process() call (pop)).
    struct RawMessage { RawOscMessage msg; };
    static constexpr int kMonitorFifoSize = 64;
    struct MonitorFifo
    {
        void push (const RawMessage& m)
        {
            int s1, n1, s2, n2;
            fifo.prepareToWrite (1, s1, n1, s2, n2);
            if (n1 > 0) messages[static_cast<size_t> (s1)] = m;
            fifo.finishedWrite (n1 + n2);
        }
        bool pop (RawMessage& m)
        {
            int s1, n1, s2, n2;
            fifo.prepareToRead (1, s1, n1, s2, n2);
            if (n1 == 0) return false;
            m = messages[static_cast<size_t> (s1)];
            fifo.finishedRead (n1 + n2);
            return true;
        }
        juce::AbstractFifo                          fifo { kMonitorFifoSize };
        std::array<RawMessage, kMonitorFifoSize>    messages;
    } monitorFifo;

    std::unique_ptr<juce::DatagramSocket> socket;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscInDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * OscOutDeviceNode  (nodeType 11)
 *
 * 1 OSC input port — receives PAX_Value events from upstream graph nodes and
 * sends them as OSC messages to a configured target host:port.
 *
 * Outgoing OSC address is configurable (default "/patchy"). If the incoming
 * PAX_Value::data[] starts with '/', it is used as the OSC address directly
 * (allows upstream Pax to set the address dynamically).
 */
class OscOutDeviceNode : public NodeProcessor,
                         private juce::Thread
{
public:
    explicit OscOutDeviceNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("OscOut_" + nodeId)
    {}

    ~OscOutDeviceNode() override { closeSocket(); }

    void configure (int portToUse, const juce::String& targetHostToUse,
                    const juce::String& addressToUse)
    {
        closeSocket();
        port        = portToUse;
        targetHost  = targetHostToUse;
        oscAddress  = addressToUse.isNotEmpty() ? addressToUse : "/patchy";
        openSocket();
    }

    void openSocket()
    {
        if (port <= 0 || targetHost.isEmpty()) return;
        socket = std::make_unique<juce::DatagramSocket> (false);
        startThread (juce::Thread::Priority::normal);
        juce::Logger::writeToLog ("OscOutDeviceNode: ready -> "
                                  + targetHost + ":" + juce::String (port)
                                  + " " + oscAddress);
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

    int          port = 0;
    juce::String targetHost;
    juce::String oscAddress { "/patchy" };

private:
    void run() override
    {
        static constexpr int kBufCap = 512;
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

            // Use address from data[] if it looks like an OSC path, else oscAddress
            const char* addr = (v.dataSize > 1 && v.data[0] == '/')
                                   ? reinterpret_cast<const char*> (v.data)
                                   : oscAddress.toRawUTF8();

            int msgLen = OscCodec::serialise (addr, v, buf.data(), kBufCap);
            if (msgLen > 0)
                socket->write (targetHost, port, buf.data(), msgLen);
        }
    }

    static constexpr int kFifoSize = 256;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OscOutDeviceNode)
};


// ─────────────────────────────────────────────────────────────────────────────
/**
 * OscDeviceManager
 *
 * Owned by PatchyProcessor. Tracks per-node OSC settings and reapplies them
 * after graph rebuilds — same pattern as UdpDeviceManager.
 */
class OscDeviceManager
{
public:
    struct Settings
    {
        int          port       = 0;
        juce::String targetHost;       // OscOut only
        juce::String oscAddress { "/patchy" };  // OscOut only
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
