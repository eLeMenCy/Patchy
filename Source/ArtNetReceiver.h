#pragma once
#include <cstdint>

/**
 * ArtNetReceiver.h — v0.0.916 (2026-09-28)
 *
 * ONE process-wide Art-Net receive socket on UDP 6454, shared by every
 * ArtNetInDeviceNode (and, as a bonus, by every Patchy plugin instance
 * loaded in the same DAW process).
 *
 * Why: each ArtNet In node used to open its own socket on the fixed port
 * 6454. With two nodes (any universes), or two Patchy instances, the
 * result depended on bind order: both could log "listening" while macOS
 * delivered unicast packets only to the most recently bound socket, and a
 * node that later reopened (e.g. its settings Reset) failed to bind at all.
 * User-confirmed 2026-09-28. One socket, one bind, and every subscriber
 * receives every packet — each node keeps filtering its own universe.
 *
 * Lifetime: created on the first subscribe(), destroyed (socket closed,
 * thread stopped) when the last subscriber unsubscribes. Subscribers are
 * called on the receiver thread, under the receiver's lock — so once
 * unsubscribe() returns, the listener is guaranteed not to be called again.
 * Keep callbacks short (parse + lock-free fifo push).
 *
 * If 6454 can't be bound (another application holds it), the thread keeps
 * retrying every 0.5 s, so it recovers on its own once the port frees up;
 * isBound() reports the current state, and isPortInUse() whether binding
 * has kept failing for over 1 s (what the nodes show as "port 6454 in use").
 * Port reuse is OFF (v0.0.916) so such a conflict actually fails the bind.
 */
namespace ArtNetReceiver
{
    class Listener
    {
    public:
        virtual ~Listener() = default;
        /** Receiver thread. Raw UDP payload, unparsed. */
        virtual void artNetPacketReceived (const uint8_t* data, int numBytes) = 0;
    };

    void subscribe   (Listener* listener);   // idempotent
    void unsubscribe (Listener* listener);   // idempotent; blocks until no callback is running
    bool isBound();
    bool isPortInUse();   // v0.0.916
}
