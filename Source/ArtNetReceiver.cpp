// ArtNetReceiver.cpp — v0.0.916 (2026-09-28). See ArtNetReceiver.h.

#include "ArtNetReceiver.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace ArtNetReceiver
{
namespace
{
    constexpr int kArtNetPort     = 6454;   // same as ArtNetCodec::kPort
    constexpr int kMaxPacket      = 530;    // 18 header + 512 DMX
    constexpr int kRetryMs        = 500;    // v0.0.916 — see PortBindState.h for both timings
    constexpr int kReportMs       = 1000;

    // Note: inside this class, a bare `Listener` would resolve to
    // juce::Thread::Listener (inherited nested class), so
    // ArtNetReceiver::Listener is always written out in full here.
    class ReceiverThread : public juce::Thread
    {
    public:
        ReceiverThread() : juce::Thread ("ArtNetReceiver") { startThread (juce::Thread::Priority::normal); }
        ~ReceiverThread() override { stopThread (1000); }   // run() wakes every <=100 ms

        void add (ArtNetReceiver::Listener* l)
        {
            std::lock_guard<std::mutex> g (listenersLock);
            if (std::find (listeners.begin(), listeners.end(), l) == listeners.end())
                listeners.push_back (l);
        }

        /** Returns true if no listener is left. */
        bool remove (ArtNetReceiver::Listener* l)
        {
            std::lock_guard<std::mutex> g (listenersLock);
            listeners.erase (std::remove (listeners.begin(), listeners.end(), l), listeners.end());
            return listeners.empty();
        }

        std::atomic<bool>         bound { false };
        std::atomic<bool>         failing { false };      // v0.0.916 — bind currently failing
        std::atomic<juce::uint32> failingSince { 0 };

    private:
        void run() override
        {
            std::unique_ptr<juce::DatagramSocket> socket;
            std::array<uint8_t, kMaxPacket> buf {};
            juce::uint32 nextAttempt = 0;
            bool loggedFailure = false;

            while (! threadShouldExit())
            {
                if (socket == nullptr)
                {
                    if (juce::Time::getMillisecondCounter() < nextAttempt)
                    {
                        wait (100);
                        continue;
                    }
                    auto s = std::make_unique<juce::DatagramSocket> (false);
                    // v0.0.916 — port reuse OFF, so another app holding 6454
                    // makes this bind fail visibly ("port 6454 in use" on the
                    // nodes) instead of both "listening" while only one of
                    // them gets the unicast packets.
                    s->setEnablePortReuse (false);
                    if (s->bindToPort (kArtNetPort))
                    {
                        socket = std::move (s);
                        bound.store (true);
                        failing.store (false);
                        loggedFailure = false;
                        juce::Logger::writeToLog ("ArtNetReceiver: listening on :" + juce::String (kArtNetPort)
                                                  + " (shared by all ArtNet In nodes)");
                    }
                    else
                    {
                        if (! loggedFailure)
                        {
                            juce::Logger::writeToLog ("ArtNetReceiver: failed to bind port " + juce::String (kArtNetPort)
                                                      + " (in use by another application?) — retrying quietly");
                            failingSince.store (juce::Time::getMillisecondCounter());
                            failing.store (true);
                        }
                        loggedFailure = true;
                        nextAttempt = juce::Time::getMillisecondCounter() + (juce::uint32) kRetryMs;
                    }
                    continue;
                }

                if (socket->waitUntilReady (true, 100) <= 0)
                    continue;

                juce::String senderHost;
                int senderPort = 0;
                const int n = socket->read (buf.data(), kMaxPacket, false, senderHost, senderPort);
                if (n <= 0)
                    continue;

                std::lock_guard<std::mutex> g (listenersLock);
                for (auto* l : listeners)
                    l->artNetPacketReceived (buf.data(), n);
            }

            socket.reset();
            bound.store (false);
        }

        std::mutex             listenersLock;
        std::vector<ArtNetReceiver::Listener*> listeners;
    };

    std::mutex                      registryLock;   // guards `instance` itself
    std::unique_ptr<ReceiverThread> instance;
}

void subscribe (Listener* listener)
{
    if (listener == nullptr) return;
    std::lock_guard<std::mutex> g (registryLock);
    if (instance == nullptr)
        instance = std::make_unique<ReceiverThread>();
    instance->add (listener);
}

void unsubscribe (Listener* listener)
{
    std::lock_guard<std::mutex> g (registryLock);
    if (instance == nullptr) return;
    if (instance->remove (listener))
        instance.reset();   // last one out: stop thread, close socket
}

bool isBound()
{
    std::lock_guard<std::mutex> g (registryLock);
    return instance != nullptr && instance->bound.load();
}

bool isPortInUse()
{
    std::lock_guard<std::mutex> g (registryLock);
    return instance != nullptr
        && instance->failing.load()
        && juce::Time::getMillisecondCounter() - instance->failingSince.load() >= (juce::uint32) kReportMs;
}
} // namespace ArtNetReceiver
