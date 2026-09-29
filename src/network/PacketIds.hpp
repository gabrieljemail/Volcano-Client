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

    // The four movement-packet variants, matching the real client's own
    // ServerboundMovePlayerPacket.Pos/PosRot/Rot/StatusOnly split (see
    // LocalPlayer::sendPosition in resources/minecraft-code) — it picks
    // exactly one per tick based on what actually changed, rather than
    // always sending the widest one. Fields per protocol.json:
    //   0x1E position:      x/y/z (f64) + MovementFlags
    //   0x1F position_look: x/y/z (f64) + yaw/pitch (f32, degrees) + MovementFlags
    //   0x20 look:          yaw/pitch (f32, degrees) + MovementFlags
    //   0x21 flying:        MovementFlags alone (vanilla's StatusOnly)
    constexpr int32_t SetPlayerPositionAndRotation = 0x1F;
    constexpr int32_t SetPlayerRotation = 0x20;
    constexpr int32_t SetPlayerMovementFlags = 0x21;

    // Empty-bodied end-of-client-tick marker, sent at the very end of every
    // single client tick — Minecraft.tick()'s own last act (see
    // resources/minecraft-code's Minecraft.java:
    // `connection.send(ServerboundClientTickEndPacket.INSTANCE)`).
    //
    // This is the delimiter the server's per-client-tick movement
    // accounting is built on: ServerGamePacketListenerImpl's
    // receivedMovementThisTick/receivedPositionThisTick flags are reset in
    // handleClientTickEnd, NOT on the server's own tick. A client that
    // never sends this is, from the server's point of view, spending its
    // entire session inside one unbounded client tick — so the second
    // position-bearing movement packet it ever sends hits the
    // "receivedPositionThisTick" branch and is disconnected with "Invalid
    // move player packet received". That is what was kicking this client a
    // second or two into every session regardless of how the reports
    // themselves were paced.
    constexpr int32_t ClientTickEnd = 0x0D;

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

    // Empty-bodied ack a real client sends once it's finished loading the
    // world around the player (see NetworkClient::RunPlayLoop's worldReady
    // edge trigger) — 0x2C per this client's own bundled protocol.json
    // (play.toServer.types.packet mapper: "player_loaded"). Nothing in the
    // base protocol spec requires it, but a client that receives chunks,
    // starts moving, and never sends it looks exactly like a bot to
    // anti-cheat/anti-bot plugins — see the same concern already documented
    // on PluginMessage's brand packet above.
    constexpr int32_t PlayerLoaded = 0x2C;

    // Interaction packets — IDs and field layouts read the same way as the
    // entity/tab-list/inventory IDs above, from this client's own bundled
    // protocol.json's play.toServer.types.packet mapper (lines ~11670-11738)
    // and each packet's own play.toServer.types definition. See
    // InteractionManager/src/interaction/events/*.hpp for what dispatches
    // these.

    // packet_attack: entityId (varint) only — melee-attacking an entity.
    // Distinct from UseEntity below in this protocol version (older docs
    // describe one combined "Interact Entity" packet with an action-type
    // field; 26.1's protocol.json models them as two separate packets).
    constexpr int32_t Attack = 0x01;

    // packet_use_entity: target (varint), hand (varint mapper: 0 main_hand,
    // 1 off_hand), location (lpVec3 — variable width, 1 byte for zero; the interact point relative to
    // the entity's own position), sneaking (bool). Covers vanilla's
    // "interact at" case (right-clicking an entity) — this client has no
    // real hit-point tracking for entities yet (RaycastEntities only
    // returns a distance), so location is always sent as (0,0,0); most
    // interact behaviors (feeding an animal, opening a villager's trades)
    // don't key off it anyway.
    constexpr int32_t UseEntity = 0x1A;

    // packet_block_dig: status (varint, see DigStatus in
    // InteractionTypes.hpp), location (position, packed block pos — see
    // PacketWriter::WritePosition), face (i8, vanilla Direction ordinal —
    // see DirectionToProtocolId), sequence (varint).
    constexpr int32_t BlockDig = 0x29;

    // packet_arm_animation: hand (varint) — the visual swing, sent
    // alongside (not instead of) the actual attack/dig/use/place packet.
    constexpr int32_t ArmAnimation = 0x3F;

    // packet_block_place (this client's "use item on a block" path): hand
    // (varint), location (position — the block being clicked, NOT the one
    // being placed into), direction (varint, vanilla Direction ordinal —
    // which face of `location` was clicked), cursorX/Y/Z (f32, 0..1 within
    // that face), insideBlock (bool), worldBorderHit (bool), sequence
    // (varint).
    constexpr int32_t BlockPlace = 0x42;

    // packet_use_item: hand (varint), sequence (varint), rotation (vec2f —
    // yaw/pitch, degrees). Right-click with no block/entity targeted (eat,
    // drink, throw, draw a bow, ...).
    constexpr int32_t UseItem = 0x43;

    // chunk_batch_received: chunksPerTick (f32) — the ack vanilla's
    // ChunkBatchSender waits on before sending the next batch.
    constexpr int32_t ChunkBatchReceived = 0x0B;
}

namespace PlayS2C { // Play state, clientbound (server -> client)
    constexpr int32_t LoginPlay = 0x31;                // LOGIN
    constexpr int32_t KeepAlive = 0x2C;                // KEEP_ALIVE
    constexpr int32_t ChunkDataAndUpdateLight = 0x2D;  // LEVEL_CHUNK_WITH_LIGHT
    constexpr int32_t SynchronizePlayerPosition = 0x48;// PLAYER_POSITION (also every later teleport)
    constexpr int32_t SetCenterChunk = 0x5E;           // SET_CHUNK_CACHE_CENTER
    constexpr int32_t Disconnect = 0x20;               // DISCONNECT

    // BLOCK_UPDATE — a single block changed (0x08 per this client's own
    // bundled protocol.json, play.toClient.types.packet mapper:
    // "block_change" -> packet_block_change). Fields: location (position,
    // packed block pos — see PacketReader::ReadPosition), type (varint, the
    // new protocol block-state id). Used both for changes this client had
    // no part in (another player, redstone, ...) and to correct a locally-
    // mispredicted edit of its own (see BreakBlockEvent) — this client
    // sends no signal distinguishing "here's what changed" from "here's
    // what you got wrong," so both are handled identically: just apply the
    // given state. See NetworkInbox::pendingBlockEdits for how this hands
    // off to World::SetBlock on the main thread.
    constexpr int32_t BlockUpdate = 0x08;

    // GAME_EVENT — ID read the same way as BlockUpdate above, from this
    // client's own bundled protocol.json's play.toClient.types.packet
    // mapper: 0x26 game_state_change -> packet_game_state_change. Fields:
    // reason (u8 mapper — 3 = change_game_mode, the only one this client
    // reads), gameMode (f32, truncates cleanly to 0-3 — see PlayS2C::
    // LoginPlay's own SpawnInfo.gamemode for the same 4 values). Sent when
    // an op or command changes the local player's gamemode mid-session;
    // GlobalState::localGameMode is otherwise only set once, from LoginPlay.
    constexpr int32_t GameStateChange = 0x26;

    // PLAYER_ABILITIES — flags (i8: 0x01 invulnerable, 0x02 flying,
    // 0x04 canFly, 0x08 instabuild), flyingSpeed (f32), walkingSpeed (f32).
    // Payload size cross-checked against the live dev server: 9 bytes, this
    // file's header note already flagged it verified. instabuild, not raw
    // gamemode, is what vanilla itself checks for insta-mine (Player's own
    // getDestroySpeed reads getAbilities().instabuild) — see GlobalState::
    // canInstabuild.
    constexpr int32_t PlayerAbilities = 0x40;

    // UPDATE_LIGHT — 0x30 per bundled protocol.json ("update_light"). Same
    // light layout as the tail of map_chunk, but chunk X/Z are varints here
    // (i32 in map_chunk).
    constexpr int32_t UpdateLight = 0x30;

    // chunk_batch_start (empty) / chunk_batch_finished (batchSize varint).
    // Finished must be answered with PlayC2S::ChunkBatchReceived, or a
    // vanilla/Fabric server stops streaming chunks after the first batch.
    // damage_event — entityId (varint), then source type/cause/direct ids and
    // an optional source position, none of which the hit flash needs.
    constexpr int32_t DamageEvent = 0x19;

    constexpr int32_t ChunkBatchFinished = 0x0B;
    constexpr int32_t ChunkBatchStart = 0x0C;

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
}

} // namespace Volcano

#endif
