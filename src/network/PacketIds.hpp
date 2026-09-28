#pragma once
#ifndef VOLCANO_PACKET_IDS_H
#define VOLCANO_PACKET_IDS_H

#include <cstdint>

namespace Volcano {

// Packet IDs for protocol 775 (Minecraft 26.1) — see PROTOCOL_VERSION in
// NetworkClient.hpp for why this client declares 26.1, not 26.2.
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

    // Unsigned command execution — 0x07 (play.toServer.types.packet_chat_command),
    // just a single string field holding the command text WITHOUT a leading
    // "/". Same "not doing chat signing" reasoning as ChatMessage above
    // applies to why this client uses the unsigned variant over
    // packet_chat_command_signed.
    constexpr int32_t ChatCommand = 0x07;

    // action 0 of play.toServer.types.packet_client_command's varint mapper
    // ("perform_respawn") — the other two (request_stats/request_gamerule_
    // values) aren't used by this client.
    constexpr int32_t ClientCommand = 0x0C;

    // 0x35 per resources/maps/protocol.json (play.toServer.types.packet,
    // mapping "held_item_slot") — reports which hotbar slot is now selected.
    // Single field: i16 slotId (0-8). Named SetHeldItem to mirror
    // PlayS2C::SetHeldItem (0x69), the clientbound counterpart the server
    // sends when it forces a slot change — same field, opposite direction.
    constexpr int32_t SetHeldItem = 0x35;

    // Combat and movement-state reporting — IDs from resources/maps/
    // protocol.json's play.toServer.types.packet mapper, layouts from the
    // matching packet_* entries, and the send order each one needs from
    // vanilla's own client (MultiPlayerGameMode.attack / LocalPlayer.
    // sendPosition in resources/26.2.zip) — see SendAttack/SendSwing and
    // RunPlayLoop's movement report.
    //
    // 26.1 split attacking out of Interact (packet_use_entity, 0x1A, now
    // right-click-only) into its own packet: just the target's varint
    // entity id, nothing else — the server works out crit/sprint/sweep from
    // its own copy of the attacker's state, not from anything sent here.
    constexpr int32_t Attack = 0x01;
    // varint hand (0 = main, 1 = off). Vanilla sends this right after every
    // Attack, and on its own for a swing at air/a block.
    constexpr int32_t SwingArm = 0x3F;
    // varint entityId (always our own); varint action (1 = start_sprinting,
    // 2 = stop_sprinting, ...); varint jumpBoost (horse jumps only, 0 here).
    // The server's sprint flag — what makes a full-strength hit a knockback
    // hit, and a crit impossible — only ever changes through this packet.
    constexpr int32_t PlayerCommand = 0x2A;
    // u8 bitflags: forward, backward, left, right, jump, shift, sprint (bit
    // 0 upward). Shift is how the server learns we're sneaking now; sent
    // whenever the set of held movement keys changes.
    constexpr int32_t PlayerInput = 0x2B;
    // f64 x/y/z, f32 yaw, f32 pitch, u8 MovementFlags — SetPlayerPosition
    // plus rotation. The server aims our knockback hits along our reported
    // yaw (Player.causeExtraKnockback), so it has to be kept current.
    constexpr int32_t SetPlayerPositionAndRotation = 0x1F;
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

    // DEATH_COMBAT_EVENT — sent to a player when they die: varint playerId
    // (always the receiving player themselves) + an anonymousNbt "message"
    // (the death message, e.g. "VoidDev was slain by Zombie").
    constexpr int32_t PlayerCombatKill = 0x44;

    // f32 health; varint food; f32 saturation (9 bytes matches this file's
    // own header note: "0x68 SET_HEALTH = 9"). Sent whenever any of the
    // three changes, including right after login if the player was already
    // dead before this session connected (PlayerCombatKill above only fires
    // at the moment of death, not on a later rejoin) — health <= 0 here is
    // what actually needs to trigger the death screen in that case. All
    // three values are also stored on GlobalState (health/food/saturation)
    // for GUIController::RenderPlayerStatusBars' HUD display.
    constexpr int32_t SetHealth = 0x68;                // SET_HEALTH

    // RESPAWN — sent on every dimension change, not just death/respawn
    // (e.g. a portal). Its SpawnInfo payload (dimension id + name, hashed
    // seed, gamemode, previousGamemode, isDebug, isFlat, an optional death
    // GlobalPos, portalCooldown, seaLevel) plus a trailing u8 copyMetadata
    // bitmask — this client only reads the dimension name off the front of
    // it (for the log line) before resetting world state; see its handler.
    constexpr int32_t Respawn = 0x52;                  // RESPAWN

    // Not handled yet, listed so the "unhandled packet" log is readable.
    // Ping arrives constantly and is purely a latency probe (Keep Alive is
    // what actually holds the connection open), so ignoring it is safe.
    constexpr int32_t Ping = 0x3D;                     // PING
    constexpr int32_t Commands = 0x10;                 // COMMANDS

    // Tab-list (Player Info) packets — IDs read the same way as the entity
    // IDs above, from this client's own bundled protocol.json's
    // play.toClient.types.packet mapper: 0x45 player_remove, 0x46
    // player_info (the "update" packet; protocol.json doesn't split it into
    // a separately-named add/update packet the way some doc sites do).
    constexpr int32_t PlayerInfoRemove = 0x45;         // PLAYER_INFO_REMOVE
    constexpr int32_t PlayerInfoUpdate = 0x46;         // PLAYER_INFO_UPDATE

    // Inventory sync — IDs read the same way as the entity/tab-list IDs
    // above, from this client's own bundled protocol.json's play.toClient.
    // types.packet mapper: 0x12 window_items, 0x14 set_slot, 0x69
    // held_item_slot. windowId 0 (used by both SetContainerContent and
    // SetContainerSlot) is always the player's own inventory — this client
    // never opens a server-side container (chest, furnace, ...) since it
    // can't interact with blocks yet, so any other windowId is ignored.
    constexpr int32_t SetContainerContent = 0x12; // WINDOW_ITEMS — full inventory resync (e.g. right after spawn).
    constexpr int32_t SetContainerSlot = 0x14;    // SET_SLOT — one slot changed (e.g. picking something up).
    constexpr int32_t SetHeldItem = 0x69;         // HELD_ITEM_SLOT (clientbound) — server forces the selected hotbar slot.

    // UPDATE_TIME — ID read the same way as the entity/tab-list IDs above,
    // from this client's own bundled protocol.json's play.toClient.types.
    // packet mapper: 0x71 update_time. Body layout (packet_update_time) is
    // NOT the classic flat (worldAge:i64, timeOfDay:i64) pair — 26.1
    // refactored this into i64 age + a VarInt-prefixed array of "clock"
    // updates (id, totalTicks, partialTick, rate), only sending entries for
    // clocks that actually changed. See NetworkClient::RunPlayLoop for how
    // this client picks a clock out of that array.
    constexpr int32_t UpdateTime = 0x71;

    // Combat/entity-state packets — IDs read the same way as the entity IDs
    // above (protocol.json's play.toClient.types.packet mapper), handler
    // behavior cross-checked against vanilla's ClientPacketListener in
    // resources/26.2.zip.
    //
    // varint entityId; lpVec3 velocity (blocks/tick — see ReadLpVec3). For
    // our own entity id this IS knockback: the server sends it whenever
    // we're hit (ServerPlayer.hurtMarked) and the client replaces its own
    // velocity with it outright.
    constexpr int32_t SetEntityMotion = 0x65;     // SET_ENTITY_MOTION
    // varint entityId; topBitSetTerminatedArray of (i8 slot, Slot item) —
    // another entity's held items and worn armor (slots 0-7 are
    // EquipmentSlot's ordinals: mainhand, offhand, feet, legs, chest, head,
    // body, saddle).
    constexpr int32_t SetEquipment = 0x66;        // SET_EQUIPMENT
    // varint entityId; u8 action (0 swing main hand, 2 wake up, 3 swing
    // offhand, 4 critical hit, 5 magic critical hit).
    constexpr int32_t Animate = 0x02;             // ANIMATE
    // varint entityId; varint sourceTypeId/causeId/directId; option<vec3f64>
    // sourcePosition. Sent for every entity that takes damage — vanilla
    // starts its red hurt flash off this.
    constexpr int32_t DamageEvent = 0x19;         // DAMAGE_EVENT
    // varint entityId; f32 yaw — the direction a hit came from.
    constexpr int32_t HurtAnimation = 0x2A;       // HURT_ANIMATION
    // i32 entityId (NOT a varint); i8 status — 2 hurt (legacy), 3 death, 29
    // shield block, 30 shield break, 35 totem of undying, ...
    constexpr int32_t EntityEvent = 0x22;         // ENTITY_EVENT
    // varint entityId; entityMetadata (key/type/value entries, 0xFF-terminated).
    constexpr int32_t SetEntityData = 0x63;       // SET_ENTITY_DATA
    // varint entityId; array of (varint attribute, f64 base, modifiers) —
    // only applied for our own entity (see its handler).
    constexpr int32_t UpdateAttributes = 0x83;    // UPDATE_ATTRIBUTES
    // vec3f64 center; f32 radius; i32 blockCount; option<vec3f64>
    // playerKnockback; ... — only read up to playerKnockback (added
    // straight onto our velocity, vanilla's own handleExplosion).
    constexpr int32_t Explosion = 0x24;           // EXPLODE
}

} // namespace Volcano

#endif
