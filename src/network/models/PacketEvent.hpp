#pragma once
#ifndef VOLCANO_NETWORK_PACKET_EVENT_H
#define VOLCANO_NETWORK_PACKET_EVENT_H

#include <cstdint>
#include <functional>
#include <vector>
#include "../../mods/VolcanoModAPI.h"

namespace Volcano {

// One play-state packet crossing the wire, handed to listeners before the
// client handles it (clientbound) or sends it (serverbound). Ids are the
// PlayS2C / PlayC2S values in PacketIds.hpp; the payload is the packet body
// without its length or id. A listener may edit `payload` or set `cancelled`
// to drop the packet (a dropped clientbound packet is never handled).
struct PacketEvent {
    enum class Direction : uint8_t { CLIENTBOUND, SERVERBOUND };

    Direction direction;
    int32_t packetId;
    std::vector<uint8_t>& payload;
    bool cancelled = false;
};

using PacketListener = std::function<void(PacketEvent&)>;

// Packet tap for the play state. Clientbound listeners run on the network
// thread inside RunPlayLoop; serverbound ones on whichever thread queued the
// packet. Keep listeners short and never block: they sit on the network path.
// Anything not covered here can be hooked at the source (see the per-packet
// handlers in NetworkClient.cpp, kept as exported functions for MinHook).
class VC_API PacketEvents {
public:
    // Returns an id for RemoveListener. Callable from any thread.
    static uint32_t AddListener(PacketListener listener);
    static bool RemoveListener(uint32_t id);

    static bool HasListeners();

    // Runs every listener; returns false if one cancelled the packet.
    static bool Dispatch(PacketEvent& event);

    // The packet ids are only meaningful in the play state; RunPlayLoop turns
    // this on for its duration so login/configuration packets are never tapped.
    static void SetPlayActive(bool active);
    static bool IsPlayActive();
};

} // namespace Volcano

#endif
