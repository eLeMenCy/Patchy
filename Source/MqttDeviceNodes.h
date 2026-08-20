#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <mosquitto.h>
#include <atomic>
#include <array>
#include <memory>
#include <unordered_map>
#include <cerrno>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// libmosquitto global init/cleanup — process-global, NOT per-instance.
// Multiple Patchy instances can share one host process (e.g. one per DAW
// track), so this is reference-counted: the first node to construct calls
// mosquitto_lib_init(), the last one to destruct calls mosquitto_lib_cleanup().
// ─────────────────────────────────────────────────────────────────────────────
class MosquittoLibraryRef
{
public:
    MosquittoLibraryRef()
    {
        if (refCount.fetch_add (1, std::memory_order_relaxed) == 0)
            mosquitto_lib_init();
    }
    ~MosquittoLibraryRef()
    {
        if (refCount.fetch_sub (1, std::memory_order_relaxed) == 1)
            mosquitto_lib_cleanup();
    }
private:
    static std::atomic<int> refCount;
    JUCE_DECLARE_NON_COPYABLE (MosquittoLibraryRef)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MqttSubscribeNode  (nodeType 22)
 *
 * 1 Value Out port. Connects to an MQTT broker, subscribes to one topic,
 * and pushes each received message into the graph as a PAX_Value.
 *
 * Uses libmosquitto's own loop_start() background thread for network I/O —
 * same shape as our own custom socket threads (UdpInDeviceNode etc.), just
 * managed by the library instead of a juce::Thread we own directly.
 *
 * PAX_Value mapping (mirrors the OSC address/value split, same accepted
 * limitation): topic goes in data[] (PAX_DATA_STRING), numeric-parsed
 * payload goes in `value`. A non-numeric payload currently can't be
 * represented alongside the topic in the same collapsed PAX_Value — same
 * known limitation OSC already has with string args overwriting the address.
 * Fine for v1: the planned "MQTT → Value" Phase 4 adapter is scoped to
 * numeric payloads specifically.
 */
class MqttSubscribeNode : public NodeProcessor
{
public:
    explicit MqttSubscribeNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi)
    {
        clientId = "Patchy_" + juce::Uuid().toString().substring (0, 8);
    }

    ~MqttSubscribeNode() override { teardown(); }

    /** Tears down any existing connection and establishes a fresh one with
     *  the given settings. Called on any settings change — same "fully
     *  close then reopen" convention as UDP/OSC In Device configure().
     *
     *  FIXED (2026-08-13): mosquitto_connect_async()'s DNS/hostname
     *  resolution is genuinely blocking (documented upstream limitation),
     *  so the whole connect sequence now runs on a background thread
     *  instead of here on the message thread. The tricky part isn't
     *  starting that thread — it's that JUCE can't forcibly kill a
     *  thread, so if this node is torn down (settings changed again, or
     *  the node deleted) while that thread is still stuck inside the one
     *  blocking mosquitto_connect_async() call, the thread can't notice
     *  and stop until that call finally returns. The old design would
     *  have raced: teardown() destroying the shared mosq handle while the
     *  background thread might still be using it.
     *
     *  Fixed properly via ownership, not timing: `connection` is a
     *  shared_ptr, captured by value into the launched thread's lambda.
     *  The thread never touches `this` at all — only the MqttConnection
     *  object and plain copies of the settings it needs. teardown() just
     *  drops this node's own reference; if the background thread still
     *  holds its own copy, the MqttConnection object — mosq handle,
     *  libmosquitto library ref, and all — stays alive until that thread
     *  finishes and releases its copy too, however long that takes,
     *  entirely independent of whether this node object still exists.
     *  Cleanup then happens automatically in ~MqttConnection(), safely,
     *  off the message thread, on whichever thread drops the last
     *  reference. */
    void configure (const juce::String& host, int port, const juce::String& topicToUse,
                    int qosToUse, const juce::String& user, const juce::String& pass)
    {
        int clampedQos = juce::jlimit (0, 2, qosToUse);
        int clampedPort = port > 0 ? port : 1883;

        // Idempotency guard — skip the teardown+reconnect entirely if nothing
        // actually changed. Belt-and-suspenders alongside the frontend's
        // debounce: avoids any redundant reconnect regardless of what
        // triggered this call. Reads `connection` under the same lock
        // process() uses — see that method's comment for why the lock
        // exists at all.
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            if (connection != nullptr
                && brokerHost == host && brokerPort == clampedPort && topic == topicToUse
                && qos == clampedQos && username == user && password == pass)
                return;
        }

        teardown();

        brokerHost = host;
        brokerPort = clampedPort;
        topic      = topicToUse;
        qos        = clampedQos;
        username   = user;
        password   = pass;

        if (brokerHost.isEmpty() || topic.isEmpty())
            return;   // Not enough to connect yet — wait for full settings

        auto newConnection = std::make_shared<MqttConnection>();
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            connection = newConnection;
        }

        auto host_ = brokerHost; auto port_ = brokerPort; auto topic_ = topic;
        auto qos_ = qos; auto user_ = username; auto pass_ = password; auto id_ = clientId;

        juce::Thread::launch ([newConnection, host_, port_, topic_, qos_, user_, pass_, id_]
        {
            newConnection->connectAndSubscribe (host_, port_, topic_, qos_, user_, pass_, id_);
        });
    }

    void process (int /*numSamples*/) override
    {
        outputValueCount = 0;

        // Guarded read — configure()/teardown() (message thread) reassign
        // `connection` concurrently with this (audio thread). shared_ptr's
        // own refcount is atomic, but the pointer fields of the shared_ptr
        // variable itself aren't safe to read on one thread while written
        // on another — a brief spinlock around just this copy, not
        // anything inside the connection itself, fixes that without
        // process() ever blocking on anything slow (the lock is only ever
        // held for a pointer copy on either side, never for actual
        // mosquitto work).
        std::shared_ptr<MqttConnection> conn;
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            conn = connection;
        }
        if (conn == nullptr) return;

        MqttConnection::RawMqttMsg msg;
        while (outputValueCount < kMaxValueEvents && conn->fifo.pop (msg))
            outputValues[static_cast<size_t> (outputValueCount++)] = msg.toPaxValue();

        if (outputValueCount > 0)
            recordMidiActivity (outputValueCount);
    }

    juce::String brokerHost;
    int          brokerPort = 1883;
    juce::String topic;
    int          qos = 1;
    juce::String username;
    juce::String password;
    juce::String clientId;             // auto-generated once, stable for the node's lifetime

    /** Safe to read from anywhere — connection outlives both this node and
     *  any background thread still working on it, via shared_ptr. */
    bool isConnected() const
    {
        std::shared_ptr<MqttConnection> conn;
        { const juce::SpinLock::ScopedLockType sl (connectionLock); conn = connection; }
        return conn != nullptr && conn->connected.load();
    }

private:
    /** Everything mosquitto-related for one connection attempt, owned
     *  jointly by this node and (while connecting) a background thread —
     *  see the long comment on configure() above for why this shape
     *  exists. Never touches the owning MqttSubscribeNode directly; the
     *  connect thread only ever has a shared_ptr to this and plain value
     *  copies of the settings, and the mosquitto callbacks below only
     *  ever see `this` (the MqttConnection), passed as userdata. */
    struct MqttConnection
    {
        struct RawMqttMsg
        {
            juce::String topic;
            float        payloadFloat = 0.0f;
            bool         payloadIsNumeric = false;

            PAX_Value toPaxValue() const
            {
                PAX_Value v {};
                v.type = PAX_TYPE_MQTT;
                v.key  = static_cast<uint32_t> (topic.hashCode());

                // Topic always goes in data[] — same convention as OSC's address.
                v.dataType = PAX_DATA_STRING;
                auto topicUtf8 = topic.toRawUTF8();
                auto len = juce::jmin ((int) sizeof (v.data) - 1, (int) strlen (topicUtf8));
                memcpy (v.data, topicUtf8, static_cast<size_t> (len));
                v.data[len] = 0;
                v.dataSize = static_cast<uint16_t> (len);

                // Numeric payload also goes in value (separate field, no conflict
                // with the topic string above) — 0 if payload wasn't numeric.
                v.value = payloadIsNumeric ? payloadFloat : 0.0f;
                return v;
            }
        };

        ~MqttConnection()
        {
            if (mosq != nullptr)
            {
                mosquitto_disconnect (mosq);
                mosquitto_loop_stop (mosq, true);   // force — may still be connecting
                mosquitto_destroy (mosq);
            }
        }

        /** Runs entirely on the background thread launched from configure().
         *  Exact same sequence as the old synchronous configure() body —
         *  only where it runs has changed. */
        void connectAndSubscribe (const juce::String& host, int port, const juce::String& topicToUse,
                                  int qosToUse, const juce::String& user, const juce::String& pass,
                                  const juce::String& clientId)
        {
            topic = topicToUse;
            qos   = qosToUse;

            mosq = mosquitto_new (clientId.toRawUTF8(), true, this);
            if (mosq == nullptr)
            {
                juce::Logger::writeToLog ("MqttSubscribeNode: mosquitto_new failed");
                return;
            }

            if (user.isNotEmpty())
                mosquitto_username_pw_set (mosq, user.toRawUTF8(),
                                           pass.isNotEmpty() ? pass.toRawUTF8() : nullptr);

            mosquitto_message_callback_set (mosq, &MqttConnection::onMessageTrampoline);
            mosquitto_connect_callback_set (mosq, &MqttConnection::onConnectTrampoline);

            // Order matters here: on macOS (and Windows), mosquitto_connect_async()
            // fails with MOSQ_ERR_ERRNO/ENOTCONN unless the loop thread is already
            // running when it's called — the reverse of what you'd expect from the
            // function names, and apparently not required on Linux. Known
            // libmosquitto quirk (eclipse-mosquitto#365).
            mosquitto_loop_start (mosq);

            int rc = mosquitto_connect_async (mosq, host.toRawUTF8(), port, 60);
            if (rc != MOSQ_ERR_SUCCESS)
            {
                juce::String extra;
                if (rc == MOSQ_ERR_ERRNO)
                    extra = juce::String (" (errno ") + juce::String (errno) + ": "
                            + juce::String (strerror (errno)) + ")";
                juce::Logger::writeToLog ("MqttSubscribeNode: connect failed - "
                                           + juce::String (mosquitto_strerror (rc)) + extra
                                           + " [host=" + host + " port=" + juce::String (port)
                                           + " clientId=" + clientId + "]");
                mosquitto_loop_stop (mosq, true);   // force — connect never succeeded
                mosquitto_destroy (mosq);
                mosq = nullptr;
                return;
            }

            connected = false;   // set true in onConnect callback once handshake completes
        }

        void handleMessage (const struct mosquitto_message* msg)
        {
            if (msg == nullptr || msg->topic == nullptr) return;

            RawMqttMsg m;
            m.topic = juce::String (msg->topic);

            if (msg->payload != nullptr && msg->payloadlen > 0)
            {
                juce::String payloadStr = juce::String::fromUTF8 (
                    static_cast<const char*> (msg->payload), msg->payloadlen);
                m.payloadFloat  = payloadStr.getFloatValue();
                m.payloadIsNumeric = payloadStr.trim().containsOnly ("0123456789.-+eE")
                                     && payloadStr.trim().isNotEmpty();
            }

            juce::Logger::writeToLog ("MqttSubscribeNode: received \"" + m.topic + "\" ("
                                       + juce::String (msg->payloadlen) + " bytes)");
            fifo.push (m);
        }

        // ── Callback trampolines — libmosquitto calls these from its own
        // thread. userdata is this MqttConnection, never the owning node —
        // see the class comment above for why. ────────────────────────────
        static void onMessageTrampoline (struct mosquitto*, void* userdata,
                                         const struct mosquitto_message* msg)
        {
            if (auto* self = static_cast<MqttConnection*> (userdata))
                self->handleMessage (msg);
        }

        static void onConnectTrampoline (struct mosquitto* m, void* userdata, int rc)
        {
            auto* self = static_cast<MqttConnection*> (userdata);
            if (self == nullptr) return;
            if (rc == 0)
            {
                self->connected = true;
                juce::Logger::writeToLog ("MqttSubscribeNode: connected, subscribing to \""
                                           + self->topic + "\" (qos " + juce::String (self->qos) + ")");
                int subRc = mosquitto_subscribe (m, nullptr, self->topic.toRawUTF8(), self->qos);
                if (subRc != MOSQ_ERR_SUCCESS)
                    juce::Logger::writeToLog ("MqttSubscribeNode: subscribe failed - "
                                               + juce::String (mosquitto_strerror (subRc)));
            }
            else
            {
                self->connected = false;
                juce::Logger::writeToLog ("MqttSubscribeNode: connect rejected - "
                                           + juce::String (mosquitto_connack_string (rc)));
            }
        }

        // ── Routing FIFO — libmosquitto thread (push) / process() (pop) ──
        static constexpr int kFifoSize = 128;
        struct MessageFifo
        {
            void push (const RawMqttMsg& m)
            {
                int s1, n1, s2, n2;
                fifo.prepareToWrite (1, s1, n1, s2, n2);
                if (n1 > 0) messages[static_cast<size_t> (s1)] = m;
                fifo.finishedWrite (n1 + n2);
            }
            bool pop (RawMqttMsg& m)
            {
                int s1, n1, s2, n2;
                fifo.prepareToRead (1, s1, n1, s2, n2);
                if (n1 == 0) return false;
                m = messages[static_cast<size_t> (s1)];
                fifo.finishedRead (n1 + n2);
                return true;
            }
            juce::AbstractFifo                     fifo { kFifoSize };
            std::array<RawMqttMsg, kFifoSize>      messages;
        } fifo;

        MosquittoLibraryRef libRef;   // must be declared before mosq is ever used —
                                      // and must live here, not on the node, since a
                                      // background thread's use of mosq can now
                                      // outlive the node itself
        struct mosquitto* mosq = nullptr;
        juce::String      topic;
        int               qos = 1;
        std::atomic<bool> connected { false };
    };

    void teardown()
    {
        // Just drop our own reference, under the same lock process() uses
        // to read it — see configure()'s comment for why this is safe
        // even if a background thread is still connecting: dropping our
        // reference here doesn't wait for or destroy anything the thread
        // might still be using, it just stops pointing at it.
        const juce::SpinLock::ScopedLockType sl (connectionLock);
        connection.reset();
    }

    juce::SpinLock connectionLock;
    std::shared_ptr<MqttConnection> connection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MqttSubscribeNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MqttPublishNode  (nodeType 23)
 *
 * 1 Value In port. Publishes each incoming PAX_Value to an MQTT broker.
 *
 * Topic: data[] overrides the configured topic when non-empty — mirrors
 * OSC Out's address-override convention, and composes naturally with
 * MqttSubscribeNode (topic-in-data[] on receive -> topic-in-data[] on
 * republish), so chaining Subscribe straight into Publish gives a working
 * "relay to the same topic" with zero special-casing.
 *
 * Payload: always the numeric `value`, formatted as plain decimal text
 * (e.g. "23.5") — matches common MQTT convention (Node-RED, Home
 * Assistant, etc. publish plain numeric strings, not JSON-wrapped). Same
 * v1 scoping as MqttSubscribeNode: numeric payloads only.
 *
 * Uses its own dedicated send thread + FIFO — same shape as
 * OscOutDeviceNode/UdpOutDeviceNode — rather than calling
 * mosquitto_publish() directly from process() (audio thread), since
 * libmosquitto's internal locking isn't guaranteed real-time-safe.
 *
 * NOTE: uses manualReset=false on its WaitableEvent, matching the same
 * fix later applied to OscOutDeviceNode/UdpOutDeviceNode (2026-08-13,
 * see Architecture.md Phase 6 table) — those two originally shipped with
 * manualReset=true and no .reset() call anywhere, a latent CPU-spin bug
 * (same root cause the DmxOutDeviceNode fix addressed first). This class
 * got manualReset=false correct from the start; it just wasn't yet fixed
 * in the two older files when this comment was first written.
 */
class MqttPublishNode : public NodeProcessor
{
public:
    explicit MqttPublishNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi)
    {
        clientId = "Patchy_" + juce::Uuid().toString().substring (0, 8);
    }

    ~MqttPublishNode() override { teardown(); }

    /** Tears down any existing connection and establishes a fresh one with
     *  the given settings — same "fully close then reopen" convention as
     *  MqttSubscribeNode::configure().
     *
     *  FIXED (2026-08-13): same fix, same reasoning as
     *  MqttSubscribeNode::configure() — see that class's long comment.
     *  The one difference here: this class used to own its send thread
     *  directly (`private juce::Thread`), started only after a successful
     *  synchronous connect. That inheritance is gone now — the connect
     *  sequence and the send loop both run inside PublishConnection::run()
     *  on a thread launched via juce::Thread::launch(), for the same
     *  ownership-safety reason Subscribe moved to a shared_ptr'd
     *  connection object: a `private juce::Thread` member is still owned
     *  by this node, so stopThread() in teardown() would still race
     *  against a thread stuck inside the one blocking
     *  mosquitto_connect_async() call. */
    void configure (const juce::String& host, int port, const juce::String& topicToUse,
                    int qosToUse, bool retainToUse, const juce::String& user, const juce::String& pass)
    {
        int clampedQos = juce::jlimit (0, 2, qosToUse);
        int clampedPort = port > 0 ? port : 1883;

        // Reads `connection` under the same lock process() uses — see
        // that method's comment for why the lock exists at all.
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            if (connection != nullptr
                && brokerHost == host && brokerPort == clampedPort && topic == topicToUse
                && qos == clampedQos && retain == retainToUse && username == user && password == pass)
                return;
        }

        teardown();

        brokerHost = host;
        brokerPort = clampedPort;
        topic      = topicToUse;
        qos        = clampedQos;
        retain     = retainToUse;
        username   = user;
        password   = pass;

        if (brokerHost.isEmpty() || topic.isEmpty())
            return;   // Not enough to connect yet — wait for full settings

        auto newConnection = std::make_shared<PublishConnection>();
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            connection = newConnection;
        }

        auto host_ = brokerHost; auto port_ = brokerPort; auto topic_ = topic; auto qos_ = qos;
        auto retain_ = retain; auto user_ = username; auto pass_ = password; auto id_ = clientId;

        juce::Thread::launch ([newConnection, host_, port_, topic_, qos_, retain_, user_, pass_, id_]
        {
            newConnection->run (host_, port_, topic_, qos_, retain_, user_, pass_, id_);
        });
    }

    void process (int /*numSamples*/) override
    {
        // Guarded read — configure()/teardown() (message thread) reassign
        // `connection` concurrently with this (audio thread). See
        // MqttSubscribeNode::process()'s comment for the full reasoning;
        // same pattern here.
        std::shared_ptr<PublishConnection> conn;
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            conn = connection;
        }
        // If never successfully configured (empty host/topic), there's
        // nowhere to push — silently drop, matching "not connected, don't
        // pretend to be sending" rather than the old behaviour of always
        // pushing into a FIFO nothing was ever draining.
        if (conn == nullptr) return;

        for (int i = 0; i < inputValueCount; ++i)
            conn->sendQueue.push (inputValues[static_cast<size_t> (i)]);

        if (inputValueCount > 0)
        {
            conn->sendQueue.signalDataAvailable();
            recordMidiActivity (inputValueCount);
        }
    }

    juce::String brokerHost;
    int          brokerPort = 1883;
    juce::String topic;
    int          qos = 1;
    bool         retain = false;
    juce::String username;
    juce::String password;
    juce::String clientId;

    bool isConnected() const
    {
        std::shared_ptr<PublishConnection> conn;
        { const juce::SpinLock::ScopedLockType sl (connectionLock); conn = connection; }
        return conn != nullptr && conn->connected.load();
    }

private:
    /** Everything mosquitto-related for one connection attempt plus its
     *  ongoing send loop, owned jointly by this node and (for as long as
     *  its background thread runs) that thread — see configure()'s and
     *  MqttSubscribeNode::MqttConnection's comments for why this shape
     *  exists. Never touches the owning MqttPublishNode directly. */
    struct PublishConnection
    {
        ~PublishConnection()
        {
            if (mosq != nullptr)
            {
                mosquitto_disconnect (mosq);
                mosquitto_loop_stop (mosq, true);   // force — may still be connecting/running
                mosquitto_destroy (mosq);
            }
        }

        /** Runs entirely on the background thread launched from
         *  configure(): connects, then falls straight into the same send
         *  loop that used to be run() — just started right after a
         *  successful (async) connect instead of gated behind a
         *  synchronous one. Exits when shouldStop is set (teardown()) —
         *  checked at the top of every loop iteration, same granularity
         *  the old threadShouldExit() check had. */
        void run (const juce::String& host, int port, const juce::String& topicToUse,
                  int qosToUse, bool retainToUse, const juce::String& user, const juce::String& pass,
                  const juce::String& clientId)
        {
            topic      = topicToUse;
            qos        = qosToUse;
            retain     = retainToUse;
            hostForLog = host;
            portForLog = port;

            mosq = mosquitto_new (clientId.toRawUTF8(), true, this);
            if (mosq == nullptr)
            {
                juce::Logger::writeToLog ("MqttPublishNode: mosquitto_new failed");
                return;
            }

            if (user.isNotEmpty())
                mosquitto_username_pw_set (mosq, user.toRawUTF8(),
                                           pass.isNotEmpty() ? pass.toRawUTF8() : nullptr);

            mosquitto_connect_callback_set (mosq, &PublishConnection::onConnectTrampoline);

            // Same call-order requirement as MqttSubscribeNode — loop_start()
            // must come before connect_async() on macOS (known upstream
            // quirk, eclipse-mosquitto#365).
            mosquitto_loop_start (mosq);

            int rc = mosquitto_connect_async (mosq, host.toRawUTF8(), port, 60);
            if (rc != MOSQ_ERR_SUCCESS)
            {
                juce::String extra;
                if (rc == MOSQ_ERR_ERRNO)
                    extra = juce::String (" (errno ") + juce::String (errno) + ": "
                            + juce::String (strerror (errno)) + ")";
                juce::Logger::writeToLog ("MqttPublishNode: connect failed - "
                                           + juce::String (mosquitto_strerror (rc)) + extra
                                           + " [host=" + host + " port=" + juce::String (port)
                                           + " clientId=" + clientId + "]");
                mosquitto_loop_stop (mosq, true);
                mosquitto_destroy (mosq);
                mosq = nullptr;
                return;
            }

            connected = false;   // set true in onConnect callback once handshake completes

            while (! shouldStop.load())
            {
                PAX_Value v {};
                if (! sendQueue.pop (v))
                {
                    sendQueue.waitForData (100);
                    continue;
                }
                if (mosq == nullptr) continue;

                // Topic: data[] overrides the configured topic when non-empty —
                // mirrors OSC Out's address-override convention.
                juce::String pubTopic = topic;
                if (v.dataType == PAX_DATA_STRING && v.dataSize > 0)
                    pubTopic = juce::String (juce::CharPointer_UTF8 ((const char*) v.data));

                if (pubTopic.isEmpty()) continue;

                // Payload: numeric value as plain decimal text — same v1
                // scoping as MqttSubscribeNode (numeric payloads only).
                juce::String payload (v.value, 6);
                auto payloadUtf8 = payload.toRawUTF8();

                mosquitto_publish (mosq, nullptr, pubTopic.toRawUTF8(),
                                   (int) strlen (payloadUtf8), payloadUtf8, qos, retain);
            }
        }

        static void onConnectTrampoline (struct mosquitto*, void* userdata, int rc)
        {
            auto* self = static_cast<PublishConnection*> (userdata);
            if (self == nullptr) return;
            if (rc == 0)
            {
                self->connected = true;
                juce::Logger::writeToLog ("MqttPublishNode: connected to "
                                           + self->hostForLog + ":" + juce::String (self->portForLog));
            }
            else
            {
                self->connected = false;
                juce::Logger::writeToLog ("MqttPublishNode: connect rejected - "
                                           + juce::String (mosquitto_connack_string (rc)));
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
            juce::WaitableEvent               event { false };   // auto-reset — see class comment
        } sendQueue;

        MosquittoLibraryRef libRef;   // must be declared before mosq is ever used —
                                      // and must live here, not on the node, since a
                                      // background thread's use of mosq can now
                                      // outlive the node itself
        struct mosquitto* mosq = nullptr;
        juce::String      topic;
        int               qos = 1;
        bool              retain = false;
        juce::String      hostForLog;   // set alongside topic/qos/retain, used only for the
        int               portForLog = 0;   // onConnectTrampoline log line
        std::atomic<bool> connected  { false };
        std::atomic<bool> shouldStop { false };
    };

    void teardown()
    {
        // Grab our own copy under the lock first, so the shouldStop/signal
        // below (and the reset) don't race process() copying the same
        // pointer concurrently. Signalling shouldStop here doesn't wait
        // for the thread to actually see it — if it's stuck inside the
        // one blocking mosquitto_connect_async() call, it won't even
        // check shouldStop until that returns, however long that takes.
        // That's fine: dropping our reference is what makes it safe
        // regardless — the thread's own copy keeps PublishConnection (and
        // its mosq handle) alive until the thread itself actually
        // finishes and releases it, entirely independent of this node.
        std::shared_ptr<PublishConnection> old;
        {
            const juce::SpinLock::ScopedLockType sl (connectionLock);
            old = connection;
            connection.reset();
        }
        if (old != nullptr)
        {
            old->shouldStop = true;
            old->sendQueue.signalDataAvailable();
        }
    }

    juce::SpinLock connectionLock;
    std::shared_ptr<PublishConnection> connection;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MqttPublishNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MqttMonitorEvent / MqttMonitorBuffer / MqttMonitorNode  (nodeType 24)
 *
 * MQTT In + MQTT Out (pass-through, display only) — same shape as
 * DmxMonitorNode/ArtNetMonitorNode, not OscMonitorNode/UdpMonitorNode.
 *
 * Deliberately simpler than OSC/UDP Monitor's raw-capture-bypass design:
 * those needed to tap raw protocol bytes independently of the collapsed
 * PAX_Value because PAX_Value only keeps the FIRST typed arg, losing real
 * data on multi-arg OSC messages. MQTT has no equivalent problem — our own
 * design already scopes MQTT to one topic + one numeric payload per
 * message (see MqttSubscribeNode/MqttPublishNode's locked decisions), so
 * the collapsed PAX_Value (topic in data[], payload in value) already IS
 * the full picture, regardless of what feeds it. No raw-tap FIFO, no
 * ProcessingGraph routing branch needed — process() just reads its own
 * inputValues directly, same as DmxMonitorNode/ArtNetMonitorNode. No
 * source-node attribution either, for the same reason those two don't
 * have it: a topic+payload log is fully self-descriptive without needing
 * to know which upstream node produced it.
 */

struct MqttMonitorEvent
{
    int64_t      timestampMs = 0;
    juce::String topic;
    juce::String payload;   // formatted numeric value, e.g. "23.500000"
};

struct MqttMonitorBatch
{
    juce::String                   nodeId;
    std::vector<MqttMonitorEvent>  events;
};

struct MqttMonitorBuffer
{
    static constexpr int kRingSize = 512;

    void push (const MqttMonitorEvent& ev)
    {
        int writePos = (ringWritePos.load() + 1) % kRingSize;
        ring[static_cast<size_t> (writePos)] = ev;
        ringWritePos.store (writePos);
    }

    std::vector<MqttMonitorEvent> drain()
    {
        std::vector<MqttMonitorEvent> result;
        int readPos  = ringReadPos.load();
        int writePos = ringWritePos.load();
        while (readPos != writePos)
        {
            readPos = (readPos + 1) % kRingSize;
            result.push_back (ring[static_cast<size_t> (readPos)]);
            ringReadPos.store (readPos);
        }
        return result;
    }

    std::array<MqttMonitorEvent, kRingSize> ring;
    std::atomic<int> ringWritePos { 0 };
    std::atomic<int> ringReadPos  { 0 };
};

class MqttMonitorNode : public NodeProcessor
{
public:
    MqttMonitorNode (const juce::String& nodeId, MqttMonitorBuffer* sharedBuffer)
        : NodeProcessor (nodeId, Type::Midi), buffer (sharedBuffer) {}

    void process (int /*numSamples*/) override
    {
        outputValueCount = inputValueCount;
        for (int i = 0; i < inputValueCount; ++i)
            outputValues[static_cast<size_t> (i)] = inputValues[static_cast<size_t> (i)];

        if (inputValueCount > 0)
            recordMidiActivity (inputValueCount);

        if (buffer == nullptr) return;
        const int64_t now = juce::Time::currentTimeMillis();
        for (int i = 0; i < inputValueCount; ++i)
        {
            const auto& v = inputValues[static_cast<size_t> (i)];
            if (v.dataType != PAX_DATA_STRING || v.dataSize == 0) continue;   // no topic, skip

            MqttMonitorEvent ev;
            ev.timestampMs = now;
            ev.topic       = juce::String (juce::CharPointer_UTF8 ((const char*) v.data));
            ev.payload     = juce::String (v.value, 6);
            buffer->push (ev);
        }
    }

private:
    MqttMonitorBuffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MqttMonitorNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MqttConsoleNode  (nodeType 25)
 *
 * MQTT Out only — pure value source, same shape as DmxConsoleNode/
 * ArtNetConsoleNode: no broker connection of its own, that stays on a
 * downstream MqttPublishNode. Manual topic+payload composer with an
 * explicit Send trigger (button or Enter — both call sendNow()), not a
 * continuously-held state like DMX's faders. One PAX_Value is emitted for
 * exactly one process() block per send, then cleared — a discrete event,
 * not a persistent value like a fader position. No undo/redo for sends
 * themselves (can't un-send a message that already went out), matching
 * that a "send" is an action, not state.
 */
class MqttConsoleNode : public NodeProcessor
{
public:
    explicit MqttConsoleNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi) {}

    /** Called from PatchyProcessor in response to a UI Send action. Queues
     *  one value to be emitted on the next process() block. */
    void sendNow (const juce::String& topic, float payload)
    {
        if (topic.isEmpty()) return;

        PAX_Value v {};
        v.type = PAX_TYPE_MQTT;
        v.key  = static_cast<uint32_t> (topic.hashCode());

        // Topic in data[] — same convention as MqttSubscribeNode/OSC's address.
        v.dataType = PAX_DATA_STRING;
        auto topicUtf8 = topic.toRawUTF8();
        auto len = juce::jmin ((int) sizeof (v.data) - 1, (int) strlen (topicUtf8));
        memcpy (v.data, topicUtf8, static_cast<size_t> (len));
        v.data[len] = 0;
        v.dataSize = static_cast<uint16_t> (len);

        v.value = payload;

        pendingValue = v;
        pendingSend.store (true);
    }

    void process (int /*numSamples*/) override
    {
        outputValueCount = 0;
        if (pendingSend.exchange (false))
        {
            outputValues[0] = pendingValue;
            outputValueCount = 1;
            recordMidiActivity (1);
        }
    }

private:
    PAX_Value          pendingValue {};
    std::atomic<bool>  pendingSend { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MqttConsoleNode)
};

// ─────────────────────────────────────────────────────────────────────────────
/**
 * MqttDeviceManager
 *
 * Owned by PatchyProcessor. Tracks per-node MQTT settings and reapplies them
 * after graph rebuilds — same pattern as UdpDeviceManager/OscDeviceManager.
 */
class MqttDeviceManager
{
public:
    struct Settings
    {
        juce::String host;
        int          port = 1883;
        juce::String topic;
        int          qos  = 1;
        bool         retain = false;   // Publish only
        juce::String username;
        juce::String password;
    };

    void storeSettings (const juce::String& nodeId, const Settings& s) { settings[nodeId] = s; }

    const Settings* getSettings (const juce::String& nodeId) const
    {
        auto it = settings.find (nodeId);
        return it != settings.end() ? &it->second : nullptr;
    }

    bool applyToGraph (const juce::String& nodeId, class ProcessingGraph& graph);
    void applyAllSettings (class ProcessingGraph& graph);

private:
    std::unordered_map<juce::String, Settings> settings;
};
