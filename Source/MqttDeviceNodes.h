#pragma once
#include "NodeProcessor.h"
#include <juce_core/juce_core.h>
#include <mosquitto.h>
#include <atomic>
#include <array>
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
     *  close then reopen" convention as UDP/OSC In Device configure(). */
    void configure (const juce::String& host, int port, const juce::String& topicToUse,
                    int qosToUse, const juce::String& user, const juce::String& pass)
    {
        int clampedQos = juce::jlimit (0, 2, qosToUse);
        int clampedPort = port > 0 ? port : 1883;

        // Idempotency guard — skip the teardown+reconnect entirely if nothing
        // actually changed. Belt-and-suspenders alongside the frontend's
        // debounce: avoids any redundant reconnect regardless of what
        // triggered this call.
        if (mosq != nullptr
            && brokerHost == host && brokerPort == clampedPort && topic == topicToUse
            && qos == clampedQos && username == user && password == pass)
            return;

        teardown();

        brokerHost = host;
        brokerPort = clampedPort;
        topic      = topicToUse;
        qos        = clampedQos;
        username   = user;
        password   = pass;

        if (brokerHost.isEmpty() || topic.isEmpty())
            return;   // Not enough to connect yet — wait for full settings

        mosq = mosquitto_new (clientId.toRawUTF8(), true, this);
        if (mosq == nullptr)
        {
            juce::Logger::writeToLog ("MqttSubscribeNode: mosquitto_new failed");
            return;
        }

        if (username.isNotEmpty())
            mosquitto_username_pw_set (mosq, username.toRawUTF8(),
                                       password.isNotEmpty() ? password.toRawUTF8() : nullptr);

        mosquitto_message_callback_set (mosq, &MqttSubscribeNode::onMessageTrampoline);
        mosquitto_connect_callback_set (mosq, &MqttSubscribeNode::onConnectTrampoline);

        // Order matters here: on macOS (and Windows), mosquitto_connect_async()
        // fails with MOSQ_ERR_ERRNO/ENOTCONN unless the loop thread is already
        // running when it's called — the reverse of what you'd expect from the
        // function names, and apparently not required on Linux. Known
        // libmosquitto quirk (eclipse-mosquitto#365).
        mosquitto_loop_start (mosq);

        int rc = mosquitto_connect_async (mosq, brokerHost.toRawUTF8(), brokerPort, 60);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            juce::String extra;
            if (rc == MOSQ_ERR_ERRNO)
                extra = juce::String (" (errno ") + juce::String (errno) + ": "
                        + juce::String (strerror (errno)) + ")";
            juce::Logger::writeToLog ("MqttSubscribeNode: connect failed - "
                                       + juce::String (mosquitto_strerror (rc)) + extra
                                       + " [host=" + brokerHost + " port=" + juce::String (brokerPort)
                                       + " clientId=" + clientId + "]");
            mosquitto_loop_stop (mosq, true);   // force — connect never succeeded
            mosquitto_destroy (mosq);
            mosq = nullptr;
            return;
        }

        connected = false;             // set true in onConnect callback once handshake completes
    }

    void process (int /*numSamples*/) override
    {
        outputValueCount = 0;
        RawMqttMsg msg;
        while (outputValueCount < kMaxValueEvents && fifo.pop (msg))
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
    std::atomic<bool> connected { false };

private:
    void teardown()
    {
        if (mosq != nullptr)
        {
            mosquitto_disconnect (mosq);
            mosquitto_loop_stop (mosq, false);
            mosquitto_destroy (mosq);
            mosq = nullptr;
        }
        connected = false;
    }

    // ── Callback trampolines — libmosquitto calls these from its own thread ──
    static void onMessageTrampoline (struct mosquitto*, void* userdata,
                                     const struct mosquitto_message* msg)
    {
        if (auto* self = static_cast<MqttSubscribeNode*> (userdata))
            self->handleMessage (msg);
    }

    static void onConnectTrampoline (struct mosquitto* m, void* userdata, int rc)
    {
        auto* self = static_cast<MqttSubscribeNode*> (userdata);
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

    // ── Routing FIFO — libmosquitto thread (push) / process() (pop) ─────────
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

    MosquittoLibraryRef libRef;   // must be declared before mosq is ever used
    struct mosquitto* mosq = nullptr;

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
 * NOTE: uses manualReset=false on its WaitableEvent, unlike
 * OscOutDeviceNode/UdpOutDeviceNode which still use manualReset=true with
 * no .reset() call anywhere — a latent CPU-spin bug (same root cause as
 * the DmxOutDeviceNode fix). See Architecture.md Phase 6 table — pending
 * follow-up to fix those two, not carried into this new code.
 */
class MqttPublishNode : public NodeProcessor,
                         private juce::Thread
{
public:
    explicit MqttPublishNode (const juce::String& nodeId)
        : NodeProcessor (nodeId, Type::Midi),
          juce::Thread ("MqttPublish_" + nodeId)
    {
        clientId = "Patchy_" + juce::Uuid().toString().substring (0, 8);
    }

    ~MqttPublishNode() override { teardown(); }

    /** Tears down any existing connection and establishes a fresh one with
     *  the given settings — same "fully close then reopen" convention as
     *  MqttSubscribeNode::configure(). */
    void configure (const juce::String& host, int port, const juce::String& topicToUse,
                    int qosToUse, bool retainToUse, const juce::String& user, const juce::String& pass)
    {
        int clampedQos = juce::jlimit (0, 2, qosToUse);
        int clampedPort = port > 0 ? port : 1883;

        if (mosq != nullptr
            && brokerHost == host && brokerPort == clampedPort && topic == topicToUse
            && qos == clampedQos && retain == retainToUse && username == user && password == pass)
            return;

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

        mosq = mosquitto_new (clientId.toRawUTF8(), true, this);
        if (mosq == nullptr)
        {
            juce::Logger::writeToLog ("MqttPublishNode: mosquitto_new failed");
            return;
        }

        if (username.isNotEmpty())
            mosquitto_username_pw_set (mosq, username.toRawUTF8(),
                                       password.isNotEmpty() ? password.toRawUTF8() : nullptr);

        mosquitto_connect_callback_set (mosq, &MqttPublishNode::onConnectTrampoline);

        // Same call-order requirement as MqttSubscribeNode::configure() —
        // loop_start() must come before connect_async() on macOS
        // (known upstream quirk, eclipse-mosquitto#365).
        mosquitto_loop_start (mosq);

        int rc = mosquitto_connect_async (mosq, brokerHost.toRawUTF8(), brokerPort, 60);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            juce::String extra;
            if (rc == MOSQ_ERR_ERRNO)
                extra = juce::String (" (errno ") + juce::String (errno) + ": "
                        + juce::String (strerror (errno)) + ")";
            juce::Logger::writeToLog ("MqttPublishNode: connect failed - "
                                       + juce::String (mosquitto_strerror (rc)) + extra
                                       + " [host=" + brokerHost + " port=" + juce::String (brokerPort)
                                       + " clientId=" + clientId + "]");
            mosquitto_loop_stop (mosq, true);
            mosquitto_destroy (mosq);
            mosq = nullptr;
            return;
        }

        connected = false;   // set true in onConnect callback once handshake completes
        startThread (juce::Thread::Priority::normal);
    }

    void process (int /*numSamples*/) override
    {
        for (int i = 0; i < inputValueCount; ++i)
            sendQueue.push (inputValues[static_cast<size_t> (i)]);

        if (inputValueCount > 0)
        {
            sendQueue.signalDataAvailable();
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
    std::atomic<bool> connected { false };

private:
    void teardown()
    {
        if (isThreadRunning())
        {
            signalThreadShouldExit();
            sendQueue.signalDataAvailable();
            stopThread (1000);
        }
        if (mosq != nullptr)
        {
            mosquitto_disconnect (mosq);
            mosquitto_loop_stop (mosq, false);
            mosquitto_destroy (mosq);
            mosq = nullptr;
        }
        connected = false;
    }

    static void onConnectTrampoline (struct mosquitto*, void* userdata, int rc)
    {
        auto* self = static_cast<MqttPublishNode*> (userdata);
        if (self == nullptr) return;
        if (rc == 0)
        {
            self->connected = true;
            juce::Logger::writeToLog ("MqttPublishNode: connected to "
                                       + self->brokerHost + ":" + juce::String (self->brokerPort));
        }
        else
        {
            self->connected = false;
            juce::Logger::writeToLog ("MqttPublishNode: connect rejected - "
                                       + juce::String (mosquitto_connack_string (rc)));
        }
    }

    void run() override
    {
        while (! threadShouldExit())
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

    MosquittoLibraryRef libRef;   // must be declared before mosq is ever used
    struct mosquitto* mosq = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MqttPublishNode)
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
