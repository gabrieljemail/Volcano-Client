#pragma once
#ifndef VOLCANO_PACKET_IDS_H
#define VOLCANO_PACKET_IDS_H

#include <cstdint>

namespace Volcano {

// Packet IDs for protocol 776 (Minecraft 26.2).
//
// The Play-state clientbound IDs below are now VERIFIED against ViaVersion's
// ClientboundPackets26_1 enum (an enum constant's ordinal IS its packet ID),
// which Protocol26_1To26_2 reuses unchanged for 26.2 — i.e. the Play packet
// ID table did not move between 26.1 and 26.2. Cross-checked against payload
// sizes observed from the live dev server: 0x0A CHANGE_DIFFICULTY = 2 bytes
// (byte+bool), 0x40 PLAYER_ABILITIES = 9 (byte+2 floats), 0x67
// SET_EXPERIENCE = 6, 0x68 SET_HEALTH = 9, 0x3D PING = 4. Every one matched.
//
// The earlier note here suspected Keep Alive / Chunk Data (0x2C/0x2D) of
// being wrong; they are correct. The large unidentified packet that prompted
// that suspicion (0x10, ~99 KB) is COMMANDS — this dev server's brigadier
// command tree is enormous because of its plugins.

namespace ConfigC2S { // Configuration state, serverbound (client -> server)
    constexpr int32_t ClientInformation = 0x00;
    constexpr int32_t CookieResponse = 0x01;
    constexpr int32_t PluginMessage = 0x02;
    constexpr int32_t AcknowledgeFinishConfiguration = 0x03;
    constexpr int32_t KeepAlive = 0x04;
    constexpr int32_t Pong = 0x05;
    constexpr int32_t ResourcePackResponse = 0x06;
    constexpr int32_t KnownPacks = 0x07;
}

namespace ConfigS2C { // Configuration state, clientbound (server -> client)
    constexpr int32_t CookieRequest = 0x00;
    constexpr int32_t PluginMessage = 0x01;
    constexpr int32_t Disconnect = 0x02;
    constexpr int32_t FinishConfiguration = 0x03;
    constexpr int32_t KeepAlive = 0x04;
    constexpr int32_t Ping = 0x05;
    constexpr int32_t ResetChat = 0x06;
    constexpr int32_t RegistryData = 0x07;
    constexpr int32_t RemoveResourcePack = 0x08;
    constexpr int32_t AddResourcePack = 0x09;
    constexpr int32_t StoreCookie = 0x0A;
    constexpr int32_t Transfer = 0x0B;
    constexpr int32_t FeatureFlags = 0x0C;
    constexpr int32_t UpdateTags = 0x0D;
    constexpr int32_t KnownPacks = 0x0E;
}

namespace PlayC2S { // Play state, serverbound (client -> server)
    constexpr int32_t ConfirmTeleportation = 0x00;
    constexpr int32_t KeepAlive = 0x1C;
    constexpr int32_t SetPlayerPosition = 0x1E;

    // Plain (unsigned) chat message — 0x09 per this client's own bundled
    // resources/minecraft-data/data/pc/26.1/protocol.json
    // (play.toServer.types.packet_chat_message), the same source that
    // supplied the entity-packet IDs above. Its field layout there is:
    // string message; i64 timestamp; i64 salt; option<u8[256]> signature;
    // varint offset; u8[3] acknowledged; u8 checksum. This client never
    // establishes a chat session (no Player Session / signing keys), so it
    // sends every field's "nothing to report" value — salt=0, no
    // signature, offset=0, an all-zero 20-bit "last seen" acknowledgment
    // bitset, checksum=0 (undocumented in protocol.json beyond its type;
    // best-effort, like this client's chat *receiving* code already is —
    // see PlayerChatMessage above). A server enforcing signed chat will
    // reject/kick for this; most offline-mode servers (like this client's
    // own dev target) don't.
    constexpr int32_t ChatMessage = 0x09;
}

namespace PlayS2C { // Play state, clientbound (server -> client)
    constexpr int32_t LoginPlay = 0x31;                // LOGIN
    constexpr int32_t KeepAlive = 0x2C;                // KEEP_ALIVE
    constexpr int32_t ChunkDataAndUpdateLight = 0x2D;  // LEVEL_CHUNK_WITH_LIGHT
    constexpr int32_t SynchronizePlayerPosition = 0x48;// PLAYER_POSITION (also every later teleport)
    constexpr int32_t SetCenterChunk = 0x5E;           // SET_CHUNK_CACHE_CENTER
    constexpr int32_t Disconnect = 0x20;               // DISCONNECT
    // Content (Text Component) + overlay (Boolean: true = action bar,
    // false = chat) — see NetworkClient::RunPlayLoop.
    constexpr int32_t SystemChatMessage = 0x79;        // SYSTEM_CHAT
    // ID verified; the body's signing/filter field order beyond
    // sender+message is still best-effort (parsed from the long-stable
    // pre-776 layout) and wrapped in a try/catch, so a mismatch shows a
    // parse-failure log rather than corrupting anything else. See
    // NetworkClient::RunPlayLoop if chat from other players looks wrong.
    constexpr int32_t PlayerChatMessage = 0x41;        // PLAYER_CHAT

    // Entity lifecycle/movement — IDs read directly from this client's own
    // bundled resources/minecraft-data/data/pc/26.1/protocol.json (the same
    // per-version packet-id mapper prismarine-based tooling generates),
    // rather than cross-referenced against ViaVersion like the block above:
    // its play.toClient.types.packet mapper lists 0x01 spawn_entity, 0x35
    // rel_entity_move, 0x36 entity_move_look, 0x38 entity_look, 0x4d
    // entity_destroy, and 0x7d entity_teleport — and independently agrees
    // with every ID already verified above (0x2c keep_alive, 0x2d map_chunk,
    // 0x31 login, ...), so it's trusted for the ones that weren't
    // individually cross-checked yet.
    constexpr int32_t SpawnEntity = 0x01;              // SPAWN_ENTITY (also used for other players — the dedicated Spawn Player packet was removed).
    constexpr int32_t RelEntityMove = 0x35;            // MOVE_ENTITY_POS: fixed-point position delta only.
    constexpr int32_t EntityMoveLook = 0x36;           // MOVE_ENTITY_POS_ROT: fixed-point position delta + yaw/pitch.
    constexpr int32_t EntityLook = 0x38;               // MOVE_ENTITY_ROT: yaw/pitch only, no position change.
    constexpr int32_t EntityTeleport = 0x7D;           // ENTITY_POSITION_SYNC: absolute position + yaw/pitch.
    constexpr int32_t EntityDestroy = 0x4D;            // REMOVE_ENTITIES.

    // Health/food/saturation — float health, varint food, float saturation
    // (9 bytes matches this file's own header note: "0x68 SET_HEALTH = 9").
    // health <= 0 is the server's death signal (no client-side death
    // detection exists yet, just the HUD display reading these values).
    constexpr int32_t SetHealth = 0x68;                // SET_HEALTH

    // Not handled yet, listed so the "unhandled packet" log is readable.
    // Ping arrives constantly and is purely a latency probe (Keep Alive is
    // what actually holds the connection open), so ignoring it is safe.
    constexpr int32_t Ping = 0x3D;                     // PING
    constexpr int32_t Commands = 0x10;                 // COMMANDS
}

} // namespace Volcano

#endif
