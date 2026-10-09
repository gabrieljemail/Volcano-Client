#include "NetworkClient.hpp"
#include "VarInt.hpp"
#include "PacketIds.hpp"
#include "TextComponent.hpp"
#include "models/ChatEvent.hpp"
#include "../Logger.hpp"
#include "../renderer/terrain/ChunkParser.hpp"
#include "../renderer/terrain/models/BlockRegistry.hpp"
#include "../renderer/gui/models/Chat.hpp"
#include "../renderer/entity/models/Entity.hpp"
#include "../renderer/entity/models/EntityRegistry.hpp"
#include <glm/gtc/constants.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace Volcano {

namespace {

// Matches Java's UUID.nameUUIDFromBytes("OfflinePlayer:<name>"), which is
// what vanilla uses to derive an offline-mode account's UUID: MD5 of the
// UTF-8 name bytes, then force the version (3) and variant bits per RFC
// 4122 §4.3. An offline-mode server recomputes/overrides this from the
// submitted username anyway, but the Login Start packet still requires a
// well-formed 16-byte UUID field to be present.
std::array<uint8_t, 16> OfflineUuidFromUsername(const std::string& username)
{
    std::string input = "OfflinePlayer:" + username;

    std::array<uint8_t, 16> digest{};
    unsigned int digestLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest.data(), &digestLen);
    EVP_MD_CTX_free(ctx);

    digest[6] = (digest[6] & 0x0Fu) | 0x30u; // version 3
    digest[8] = (digest[8] & 0x3Fu) | 0x80u; // RFC 4122 variant
    return digest;
}

// Minecraft packs yaw/pitch as a signed byte spanning a full 360 degrees
// (256 steps per turn). entity.vert/entity_textured.vert's shared yaw
// rotation matrix computes world forward as (sin(yaw), 0, cos(yaw)) for a
// mesh's local +Z — the mirror image of Minecraft's own convention
// (forward = (-sin(yaw), 0, cos(yaw)), verified against
// Entity.calculateViewVector in resources/minecraft-code). Every shape's
// mesh (QuadrupedModel, PigModel, CowModel, and EntityJsonModel's humanoid
// path via its own 180-degree flip) is authored consistently against that
// shader convention, not vanilla's real one, so patching the shader (or any
// one mesh) would just move the mirroring elsewhere. Negating here instead
// feeds the whole already-self-consistent rendering pipeline the angle that
// makes its output match vanilla's real facing direction, without touching
// the shader or any mesh.
float AngleByteToRadians(int8_t angleByte)
{
    return static_cast<float>(-angleByte) * (glm::pi<float>() / 128.0f);
}

// Minecraft's LpVec3 — the compact velocity encoding Spawn Entity and Set
// Entity Motion use. Decoded exactly as net.minecraft.network.LpVec3.read
// does (resources/26.2.zip): a single 0 byte means zero; otherwise a 6-byte
// little-endian-ish pack of three 15-bit components plus a 2-bit scale, and
// when bit 2 of the first byte is set, a trailing varint holding the rest of
// the scale. Values are in blocks/tick.
glm::vec3 ReadLpVec3(PacketReader& reader)
{
    uint8_t lowest = reader.ReadByte();
    if (lowest == 0) return glm::vec3(0.0f);

    uint8_t middle = reader.ReadByte();
    uint64_t highest = static_cast<uint32_t>(reader.ReadInt());
    uint64_t packed = (highest << 16) | (static_cast<uint64_t>(middle) << 8) | lowest;

    uint64_t scale = lowest & 3u;
    if ((lowest & 4u) != 0) scale |= static_cast<uint64_t>(static_cast<uint32_t>(reader.ReadVarInt())) << 2;

    auto unpack = [](uint64_t value) {
        return std::min(static_cast<double>(value & 32767u), 32766.0) * 2.0 / 32766.0 - 1.0;
    };
    double s = static_cast<double>(scale);
    return glm::vec3(static_cast<float>(unpack(packed >> 3) * s),
                     static_cast<float>(unpack(packed >> 18) * s),
                     static_cast<float>(unpack(packed >> 33) * s));
}

// Attribute ids as Update Attributes sends them — the attribute registry in
// id order (minecraft-data's attributes.json). NOT protocol.json's
// entity_update_attributes key mapper: that list is stale and lacks
// mining_efficiency/movement_efficiency/oxygen_bonus/sneaking_speed, which
// shifted every id after 19 — movement_speed's id then read
// mining_efficiency (base 0), zeroing ground acceleration. The names are the
// same keys PlayerAttributes is read with elsewhere ("generic.attack_speed", ...).
constexpr std::array<const char*, 35> ATTRIBUTE_NAMES = {
    "generic.armor", "generic.armor_toughness", "generic.attack_damage", "generic.attack_knockback",
    "generic.attack_speed", "player.block_break_speed", "player.block_interaction_range", "burning_time",
    "camera_distance", "explosion_knockback_resistance", "player.entity_interaction_range",
    "generic.fall_damage_multiplier", "generic.flying_speed", "generic.follow_range", "generic.gravity",
    "generic.jump_strength", "generic.knockback_resistance", "generic.luck", "generic.max_absorption",
    "generic.max_health", "player.mining_efficiency", "generic.movement_efficiency", "generic.movement_speed",
    "generic.oxygen_bonus", "generic.safe_fall_distance", "generic.scale", "player.sneaking_speed",
    "zombie.spawn_reinforcements", "generic.step_height", "submerged_mining_speed", "sweeping_damage_ratio",
    "tempt_range", "water_movement_efficiency", "waypoint_transmit_range", "waypoint_receive_range",
};

// Update Entity Position packets encode movement as a fixed-point delta:
// 1/4096th of a block per unit, in a signed short (matches vanilla's own
// (currentX*32 - prevX*32)*128 encoding, which inverts to value/4096.0).
float FixedDeltaToBlocks(int16_t delta)
{
    return static_cast<float>(delta) / 4096.0f;
}

// Debug trace for entity position bugs. Turn on with "Debug.EntityTrace": true
// in settings.json; each line is one entity update with the raw packet bytes,
// the parsed numbers, the entity's position before/after, and the local
// player's position and camera yaw at that moment.
bool EntityTraceEnabled(GlobalState* state)
{
    static const bool enabled = std::get<bool>(state->config->Get("Debug.EntityTrace", false));
    return enabled;
}

void TraceEntity(GlobalState* state, const char* label, int32_t packetId,
                 const std::vector<uint8_t>& payload, uint32_t entityId,
                 const std::string& parsed, glm::vec3 before, glm::vec3 after)
{
    if (!EntityTraceEnabled(state)) return;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << "[ENTDBG] " << label << " id=0x" << std::hex << packetId
        << std::dec << " entity=" << entityId << " parsed{" << parsed << "} before=("
        << before.x << "," << before.y << "," << before.z << ") after=("
        << after.x << "," << after.y << "," << after.z << ")";
    if (state->player)
    {
        glm::vec3 p = state->player->GetPosition();
        oss << " player=(" << p.x << "," << p.y << "," << p.z << ") camYaw=" << state->player->camera.GetYaw();
    }
    oss << " hex=" << std::hex << std::setfill('0');
    for (uint8_t byte : payload) oss << std::setw(2) << static_cast<int>(byte);
    Log::Info(oss.str());
}

std::string FormatUuid(const std::array<uint8_t, 16>& uuid)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < uuid.size(); i++) {
        oss << std::setw(2) << static_cast<int>(uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) oss << '-';
    }
    return oss.str();
}

// One parsed protocol Slot (see resources/minecraft-data/.../protocol.json's
// "Slot" type) plus whether the reader's position can still be trusted for
// whatever comes after it in the packet.
struct ParsedSlot {
    ItemStack item;
    // False once this Slot carried any added/removed data component — this
    // client only tracks id+count (see ItemStack's own comment), and the
    // post-1.20.5 component encoding has no generic length prefix around a
    // component's payload, so there's no way to skip an unrecognized one
    // without a decoder for its exact shape (110+ component types as of
    // this client's pinned minecraft-data version — see protocol.json's
    // SlotComponentType mapper). item.itemId/item.count above are still
    // correct either way, since they're read before any component bytes;
    // this only means a caller reading more than one Slot (an array) must
    // stop after this one rather than trying to find the next entry.
    bool complete = true;
};

ParsedSlot ReadItemStack(PacketReader& reader)
{
    ParsedSlot result;

    int32_t count = reader.ReadVarInt();
    if (count == 0) return result; // Empty slot — no further bytes for this Slot at all.

    result.item.itemId = reader.ReadVarInt();
    result.item.count = static_cast<uint8_t>(std::clamp(count, 0, 255));

    int32_t addedComponentCount = reader.ReadVarInt();
    int32_t removedComponentCount = reader.ReadVarInt();
    result.complete = (addedComponentCount == 0 && removedComponentCount == 0);
    return result;
}

// The Set Entity Data fields tracked on a PlayerEntity, when present in
// one packet — see ReadPlayerEntityData.
struct PlayerDataUpdate {
    std::optional<uint8_t> sharedFlags; // Index 0 (Entity.DATA_SHARED_FLAGS_ID).
    std::optional<float> health;        // Index 9 (LivingEntity.DATA_HEALTH_ID).
};

// Walks a Set Entity Data entry list (u8 index, varint type, value; 0xFF
// ends it) picking out the player fields above. Indices are from the
// define order in vanilla's Entity/LivingEntity (resources/26.2.zip):
// Entity 0-7, then LivingEntity 8-14 starting with its flags and health.
// Every value type is skipped by its protocol.json encoding; a type with no
// fixed shape to skip (particles, profiles, ...) ends the walk early —
// entries arrive in index order and the ones read here come first, so that
// only ever loses fields this doesn't track.
PlayerDataUpdate ReadPlayerEntityData(PacketReader& reader)
{
    constexpr uint8_t SHARED_FLAGS_INDEX = 0, HEALTH_INDEX = 9, END_OF_DATA = 0xFF;
    PlayerDataUpdate update;

    for (;;) {
        uint8_t index = reader.ReadByte();
        if (index == END_OF_DATA) break;
        int32_t type = reader.ReadVarInt();

        switch (type) {
        case 0: { // byte
            uint8_t value = reader.ReadByte();
            if (index == SHARED_FLAGS_INDEX) update.sharedFlags = value;
            break;
        }
        case 3: { // float
            float value = reader.ReadFloat();
            if (index == HEALTH_INDEX) update.health = value;
            break;
        }
        case 1: case 12: case 14: case 15: case 19: case 20: case 42: // varint-encoded (int, direction, block state(s), optvarint, pose, humanoid_arm)
        case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: // registry variants
        case 35: case 36: case 37: case 38: // sniffer/armadillo/copper golem states
            reader.ReadVarInt();
            break;
        case 2: reader.ReadVarLong(); break;
        case 4: reader.ReadString(); break;
        case 5: ReadTextComponent(reader); break;
        case 6: if (reader.ReadBool()) ReadTextComponent(reader); break;
        case 7: if (!ReadItemStack(reader).complete) return update; break;
        case 8: reader.ReadBool(); break;
        case 9: reader.Skip(12); break; // rotations: 3x f32
        case 10: reader.Skip(8); break; // block_pos: packed i64
        case 11: if (reader.ReadBool()) reader.Skip(8); break;
        case 13: if (reader.ReadBool()) reader.Skip(16); break; // optional_uuid
        case 18: reader.ReadVarInt(); reader.ReadVarInt(); reader.ReadVarInt(); break; // villager_data
        case 39: reader.Skip(12); break; // vector3: 3x f32
        case 40: reader.Skip(16); break; // quaternion: 4x f32
        default:
            return update; // particle(s), global pos, painting variant, profile — see above.
        }
    }
    return update;
}

} // namespace

bool NetworkClient::ConnectAndLogin(const std::string& host, uint16_t port, const std::string& username)
{
    try {
        Log::Info("Connecting to " + host + ":" + std::to_string(port) + "...");
        connection.Connect(host, port);

        // Handshake (-> Login state).
        PacketWriter handshake;
        handshake.WriteVarInt(PROTOCOL_VERSION);
        handshake.WriteString(host);
        handshake.WriteUShortBE(port);
        handshake.WriteVarInt(2); // next state: Login
        connection.SendPacket(0x00, handshake.Data());

        // Login Start.
        auto uuid = OfflineUuidFromUsername(username);
        PacketWriter loginStart;
        loginStart.WriteString(username);
        loginStart.WriteBytes(uuid.data(), uuid.size());
        connection.SendPacket(0x00, loginStart.Data());

        Log::Info("Sent Handshake + Login Start as '" + username
                  + "' (offline UUID " + FormatUuid(uuid) + "). Waiting for response...");

        // Set Compression (0x03) can arrive before Login Success once the
        // server has compression enabled (the common case) — handle it and
        // keep reading until we hit an actual Login Success/Disconnect.
        for (;;) {
            std::vector<uint8_t> payload;
            int32_t packetId = connection.ReadPacket(payload);
            PacketReader reader(payload.data(), payload.size());

            if (packetId == 0x03) {
                int32_t compressionThreshold = reader.ReadVarInt();
                connection.EnableCompression(compressionThreshold);
                Log::Info("Set Compression: threshold " + std::to_string(compressionThreshold));
                continue;
            }

            if (packetId == 0x02) {
                std::array<uint8_t, 16> serverUuid{};
                reader.ReadBytes(serverUuid.data(), serverUuid.size());
                std::string serverUsername = reader.ReadString();
                Log::Info("Login Success: " + serverUsername
                          + " (" + FormatUuid(serverUuid) + ")");

                // Login Acknowledged (Login state, serverbound, empty body) —
                // required since the Configuration state was introduced: the
                // server stays in Login state, and any Configuration packet
                // we send is misinterpreted (or the connection is dropped
                // outright), until the client explicitly acks Login Success.
                PacketWriter loginAcknowledged;
                connection.SendPacket(0x03, loginAcknowledged.Data());
                Log::Info("Sent Login Acknowledged. Configuration state handling comes next.");
                return true;
            }

            if (packetId == 0x00) {
                std::string reason = reader.ReadString();
                Log::Error("Server disconnected during login: " + reason);
                lastError = reason;
                return false;
            }

            {
                std::ostringstream oss;
                oss << "[NET] Unexpected packet ID 0x" << std::hex << packetId << std::dec
                    << " while waiting for Login Success (likely an Encryption "
                    << "Request packet — not handled yet).";
                Log::Error(oss.str());
                lastError = "Server requires online-mode authentication (not supported yet).";
            }
            return false;
        }
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Connection failed: ") + e.what());
        lastError = e.what();
        return false;
    }
}

void NetworkClient::RunSession(GlobalState* state, std::stop_token stopToken)
{
    try {
        if (!RunConfiguration(state)) return;
        RunPlayLoop(state, stopToken);
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Session error: ") + e.what());
        lastError = e.what();
    }
}

bool NetworkClient::RunConfiguration(GlobalState* state)
{
    // Client Information — locale/view-distance/etc, the server uses this
    // (along with the player's position, sent once Play starts) to decide
    // what to stream. See PacketIds.hpp for why some field additions in
    // newer protocol versions may not be represented here yet.
    uint32_t renderDistance = std::get<uint32_t>(state->config->Get("Graphics.RenderDistance", uint32_t{8}));
    uint8_t renderDistanceByte = static_cast<uint8_t>(std::clamp<uint32_t>(renderDistance, 2, 32));

    PacketWriter clientInfo;
    clientInfo.WriteString("en_us");
    clientInfo.WriteBytes(&renderDistanceByte, 1); // view distance, signed byte
    clientInfo.WriteVarInt(0);   // chat mode: enabled
    clientInfo.WriteBool(true);  // chat colors
    clientInfo.WriteBytes(reinterpret_cast<const uint8_t*>("\x7F"), 1); // displayed skin parts: all
    clientInfo.WriteVarInt(1);   // main hand: right
    clientInfo.WriteBool(false); // enable text filtering
    clientInfo.WriteBool(true);  // allow server listings
    clientInfo.WriteVarInt(0);   // particle status: all
    {
        std::ostringstream diagBytes;
        diagBytes << "[DIAG] Client Information bytes: ";
        for (uint8_t b : clientInfo.Data()) diagBytes << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
        Log::Debug(diagBytes.str());
    }
    if (!std::getenv("NETDIAG_SKIP_SEND")) {
        connection.SendPacket(ConfigC2S::ClientInformation, clientInfo.Data());
        Log::Debug("[DIAG] Client Information sent.");

        // Every real client sends this right after Client Information —
        // some servers' anti-bot plugins use its absence as a bot signal
        // and silently hide the connection from the tab list/`/list` (still
        // fully joined and receiving world state) rather than kicking it,
        // which looks exactly like a server-side mystery until you notice
        // the missing brand. Identifies honestly as "volcano-client" rather
        // than spoofing "vanilla" — a server admin who sees "vanilla" will
        // reasonably expect actually-vanilla client behavior (signed chat,
        // full inventory/interaction support, ...), which this client
        // doesn't provide yet.
        PacketWriter brand;
        brand.WriteString("minecraft:brand");
        brand.WriteString("volcano-client");
        connection.SendPacket(ConfigC2S::PluginMessage, brand.Data());
        Log::Debug("[DIAG] Brand plugin message sent.");
    } else {
        Log::Debug("[DIAG] Skipping Client Information send (NETDIAG_SKIP_SEND set).");
    }

    // Each packet below updates GlobalState::connectionDetail so the loading
    // screen can say what Configuration is up to — most of these steps take
    // milliseconds, but a big modded server's registry/tag sync can visibly
    // take seconds.
    state->SetConnectionPhase(ConnectionPhase::Configuring, "Sending client information");
    int registryCount = 0;

    for (;;) {
        std::vector<uint8_t> payload;
        int32_t packetId = connection.ReadPacket(payload);
        PacketReader reader(payload.data(), payload.size());

        if (packetId == ConfigS2C::FinishConfiguration) {
            PacketWriter ack;
            connection.SendPacket(ConfigC2S::AcknowledgeFinishConfiguration, ack.Data());
            Log::Info("Configuration finished, entering Play state.");
            state->SetConnectionPhase(ConnectionPhase::LoadingTerrain);
            return true;
        }

        if (packetId == ConfigS2C::RegistryData) {
            // Body starts with the registry's identifier; the entries after
            // it aren't needed here (see the catch-all comment below).
            registryCount++;
            std::string registryId = reader.ReadString();
            state->SetConnectionDetail("Receiving registries (" + std::to_string(registryCount) + "): " + registryId);
            continue;
        }

        if (packetId == ConfigS2C::UpdateTags) {
            state->SetConnectionDetail("Updating tags");
            continue;
        }

        if (packetId == ConfigS2C::FeatureFlags) {
            state->SetConnectionDetail("Receiving feature flags");
            continue;
        }

        if (packetId == ConfigS2C::AddResourcePack) {
            state->SetConnectionDetail("Server resource pack offered (ignored)");
            continue;
        }

        if (packetId == ConfigS2C::KnownPacks) {
            state->SetConnectionDetail("Negotiating known packs");
            // Same array schema in both directions — echo the payload back
            // verbatim rather than parsing/re-encoding it.
            connection.SendPacket(ConfigC2S::KnownPacks, payload);
            continue;
        }

        if (packetId == ConfigS2C::CookieRequest) {
            std::string key = reader.ReadString();
            PacketWriter response;
            response.WriteString(key);
            response.WriteBool(false); // no payload
            connection.SendPacket(ConfigC2S::CookieResponse, response.Data());
            continue;
        }

        if (packetId == ConfigS2C::Ping) {
            // Fixed 4-byte int, NOT a VarInt — unlike almost every other
            // numeric protocol field. Writing/reading it as a VarInt here
            // produced a malformed Pong body the server's Netty decoder
            // couldn't parse, which the server reports back as
            // "Failed to decode packet 'serverbound/minecraft:pong'" and
            // disconnects for.
            int32_t id = reader.ReadInt();
            PacketWriter pong;
            pong.WriteInt(id);
            connection.SendPacket(ConfigC2S::Pong, pong.Data());
            continue;
        }

        if (packetId == ConfigS2C::Disconnect) {
            // reason is `anonymousNbt` per this client's own bundled
            // protocol.json, not a plain string — ReadString() here used to
            // misparse the NBT tag bytes as a length-prefixed string,
            // producing garbage (or throwing) instead of the real reason.
            std::string reason = PlainText(ReadTextComponent(reader));
            Log::Error("Server disconnected during configuration: " + reason);
            lastError = reason;
            return false;
        }

        // Registry Data, Update Tags, Feature Flags, resource pack packets,
        // plugin messages, etc. — nothing here we need to act on, and
        // Connection::ReadPacket already framed the whole payload so it's
        // safe to just move on to the next packet.
    }
}

void NetworkClient::SendPlayerPosition(glm::vec3 position, float yaw, float pitch, bool onGround)
{
    // Every outgoing position funnels through here, so this is the one place
    // that can catch a non-finite value regardless of which caller produced
    // it — see the "Invalid move player packet received" server kick this
    // was added to chase down; a vanilla server disconnects the instant
    // x/y/z stops being finite, but gives no hint which of this client's own
    // writers put it there.
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) {
        std::ostringstream oss;
        oss << "[NET] About to send non-finite player position: (" << position.x << ", "
            << position.y << ", " << position.z << ") onGround: " << (onGround ? "true" : "false");
        Log::Error(oss.str());
    }

    PacketWriter writer;
    writer.WriteDouble(static_cast<double>(position.x));
    writer.WriteDouble(static_cast<double>(position.y));
    writer.WriteDouble(static_cast<double>(position.z));
    writer.WriteFloat(yaw);
    writer.WriteFloat(pitch);
    // MovementFlags is a u8 bitflag — bit 0 onGround, bit 1
    // hasHorizontalCollision (protocol.json's MovementFlags type). This call
    // is only ever the one-shot post-teleport echo (see RunPlayLoop's
    // SynchronizePlayerPosition handler) — never grounded and never
    // horizontally colliding the instant after a teleport, same as vanilla.
    uint8_t flags = onGround ? 0x01 : 0x00;
    writer.WriteBytes(&flags, 1);
    connection.SendPacket(PlayC2S::SetPlayerPositionAndRotation, writer.Data());
}

void NetworkClient::RunPlayLoop(GlobalState* state, std::stop_token stopToken)
{
    // Bounds how long WaitForReadable() below can block with nothing to
    // read, so this loop can drive TickLoop::Tick() on its own steady
    // cadence even when the server goes quiet between packets (idle time
    // between Keep Alives can be many seconds) — see the tick-loop plan's
    // "blocker" section.
    constexpr auto kReadPollInterval = std::chrono::milliseconds(15);

    while (!stopToken.stop_requested() && !state->shouldClose) {
        // TickLoop::Tick() (and with it, sending PlayC2S::PlayerLoaded and
        // the per-tick position/rotation/sprint/input report) used to happen
        // right here, once per RunPlayLoop iteration — moved to its own
        // TickThread, driven by a steady wall-clock timer instead of this
        // loop's own pacing. See TickThread's class comment for why: this
        // loop's ReadPacket() call below blocks synchronously on however
        // long a packet takes to fully arrive (routinely several hundred ms
        // for a large one on a slow link), which used to freeze ticking —
        // and therefore the position report's cadence — right along with it.

        // Drain anything other threads queued (chat, commands, respawn —
        // see SendChatMessage). Must happen on this thread and outside
        // WaitForReadable below, since that call cancels socket operations
        // and a write racing it can be left half-written — see
        // Connection::SendPacket's own comment.
        connection.FlushOutbound();

        if (!connection.WaitForReadable(kReadPollInterval)) {
            continue; // Nothing arrived within the poll window — loop back and tick again.
        }

        std::vector<uint8_t> payload;
        int32_t packetId = connection.ReadPacket(payload);
        PacketReader reader(payload.data(), payload.size());

        if (packetId == PlayS2C::LoginPlay) {
            try {
                int32_t entityId = reader.ReadInt();
                state->localEntityId.store(entityId); // PlayC2S::PlayerCommand has to name this.
                reader.ReadBool(); // isHardcore
                int32_t worldCount = reader.ReadVarInt();
                for (int32_t i = 0; i < worldCount; i++) reader.ReadString(); // worldNames
                reader.ReadVarInt(); // maxPlayers
                reader.ReadVarInt(); // viewDistance
                reader.ReadVarInt(); // simulationDistance
                reader.ReadBool();   // reducedDebugInfo
                reader.ReadBool();   // enableRespawnScreen
                reader.ReadBool();   // doLimitedCrafting
                reader.ReadVarInt(); // worldState.dimension
                std::string dimensionName = reader.ReadString(); // worldState.name
                {
                    std::lock_guard<std::mutex> lock(state->currentDimensionMutex);
                    state->currentDimension = dimensionName;
                }
                reader.ReadLong();   // worldState.hashedSeed
                uint8_t gameMode = reader.ReadByte(); // worldState.gamemode — see GlobalState::localGameMode.
                state->localGameMode.store(gameMode, std::memory_order_relaxed);
                // Rest of SpawnInfo (previousGamemode, isDebug, isFlat, death,
                // portalCooldown, seaLevel) and enforcesSecureChat aren't
                // needed — nothing else in this packet is read.
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Login Play (gamemode keeps its previous value): ") + e.what());
            }
            Log::Info("Entered Play state.");
            continue;
        }

        if (packetId == PlayS2C::GameStateChange) {
            try {
                uint8_t reason = reader.ReadByte();
                float value = reader.ReadFloat();
                if (reason == 3) { // change_game_mode
                    state->localGameMode.store(static_cast<uint8_t>(value), std::memory_order_relaxed);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Game Event packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::UpdateLight) {
            try {
                PendingLightUpdate update = ChunkParser::ParseLightUpdatePacket(reader);
                std::lock_guard lock(state->networkInbox.mutex);
                state->networkInbox.pendingLightUpdates.push_back(std::move(update));
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Update Light packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::PlayerAbilities) {
            try {
                uint8_t flags = reader.ReadByte();
                state->canInstabuild.store((flags & 0x08) != 0, std::memory_order_relaxed);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Player Abilities packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::Respawn) {
            try {
                reader.ReadVarInt(); // dimension id — unused, the name below is enough for the log line.
                std::string dimensionName = reader.ReadString();
                {
                    std::lock_guard<std::mutex> lock(state->currentDimensionMutex);
                    state->currentDimension = dimensionName;
                }
                Log::Info("Respawn: switching to dimension " + dimensionName);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Respawn (resetting world state anyway): ") + e.what());
            }

            // Sent on every dimension change, not just death — the old
            // dimension's chunks/entities have to go regardless of whether
            // this parsed cleanly, or they'd keep rendering forever
            // alongside whatever streams in for the new one ("ghost chunk
            // geometry"). The rest of the packet (gamemode, death location,
            // ...) isn't read; a fresh Player Position packet always follows
            // and re-arms spawnPosition/worldReady the same way the initial
            // join does — see MeshingThread's own comment on that dance.
            state->ResetWorldState();

            // A respawn is a fresh player entity server-side: not sprinting,
            // no keys held. TickLoop resets its own side the same way once
            // the new spawn position lands (SyncToPlayerPosition) — that
            // naturally makes TickThread's next report a stop_sprinting/
            // cleared-input diff against whatever it last told the server.
            continue;
        }

        if (packetId == PlayS2C::PlayerCombatKill) {
            reader.ReadVarInt(); // playerId — always the receiving player themselves.
            std::string message = PlainText(ReadTextComponent(reader));
            state->ReportDeath(message);
            continue;
        }

        if (packetId == PlayS2C::SetHealth) {
            float health = reader.ReadFloat();
            int32_t food = reader.ReadVarInt();
            float saturation = reader.ReadFloat();

            // Stored for GUIController::RenderPlayerStatusBars' HUD display
            // — same manual-mutex-next-to-data convention as entities/
            // entitiesMutex (see GlobalState's own comment on health/food/
            // saturation/healthMutex).
            {
                std::lock_guard<std::mutex> lock(state->healthMutex);
                state->health = health;
                state->food = food;
                state->saturation = saturation;
            }

            // Catches joining a server while already dead from a previous
            // session — PlayerCombatKill above only fires at the actual
            // moment of death, never on a later rejoin, so without this the
            // death screen would just never appear (stuck with no chunks
            // loading and chat disabled, both the server's own normal
            // behavior for a not-yet-respawned player). Don't stomp a
            // real death message a PlayerCombatKill already supplied,
            // in case both arrive around the same live death.
            if (health <= 0.0f) {
                std::string existing;
                {
                    std::lock_guard<std::mutex> lock(state->deathMutex);
                    existing = state->deathMessage;
                }
                state->ReportDeath(existing.empty() ? "You died." : existing);
            }
            continue;
        }

        if (packetId == PlayS2C::OpenWindow) {
            try {
                int32_t windowId = reader.ReadVarInt();
                int32_t windowType = reader.ReadVarInt();
                std::string title = PlainText(ReadTextComponent(reader));
                Log::Info("Open window id=" + std::to_string(windowId) + " type=" + std::to_string(windowType)
                    + " title=\"" + title + "\"");
                if (windowId != 0) {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    state->inventory.OpenServerWindow(windowId, windowType, std::move(title));
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Open Window: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::CloseWindow) {
            try {
                int32_t windowId = reader.ReadVarInt();
                std::lock_guard<std::mutex> lock(state->inventory.mutex);
                Log::Info("Server closed window id=" + std::to_string(windowId)
                    + " (active=" + std::to_string(state->inventory.activeWindowId) + ")");
                if (windowId == state->inventory.activeWindowId)
                    state->inventory.CloseServerWindow();
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Close Window: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SetContainerContent) {
            try {
                int32_t windowId = reader.ReadVarInt();
                int32_t stateId = reader.ReadVarInt();
                int32_t itemCount = reader.ReadVarInt();

                bool activeContainer = false;
                {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    activeContainer = windowId != 0 && windowId == state->inventory.activeWindowId;
                }
                if (windowId != 0 && !activeContainer) continue;

                bool appliedAll = true;
                std::vector<ItemStack> items;
                items.reserve(static_cast<size_t>(std::max(itemCount, 0)));
                for (int32_t i = 0; i < itemCount; i++) {
                    ParsedSlot slot = ReadItemStack(reader);
                    items.push_back(slot.item);
                    if (!slot.complete) {
                        appliedAll = false;
                        Log::Info("Set Container Content: slot " + std::to_string(i)
                            + " carries item data components this client doesn't decode yet — "
                            + std::to_string(itemCount - i - 1) + " remaining slot(s) in this packet "
                            + "not applied; a later Set Container Slot update will still catch up "
                            + "individual changes.");
                        break;
                    }
                }
                ItemStack cursorItem;
                if (appliedAll) cursorItem = ReadItemStack(reader).item;
                {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    if (windowId == 0) {
                        state->inventory.playerStateId = stateId;
                        for (size_t i = 0; i < items.size(); i++)
                            state->inventory.SetSlot(static_cast<int32_t>(i), items[i]);
                        if (appliedAll) state->inventory.carriedItem = cursorItem;
                    } else if (windowId == state->inventory.activeWindowId) {
                        state->inventory.activeWindowStateId = stateId;
                        if (appliedAll) {
                            state->inventory.SetWindowContents(items, cursorItem);
                        } else {
                            // An item with data components cut the parse
                            // short. Still size the container from the
                            // packet's slot count (so its layout and click
                            // indices stay right) and apply the slots that
                            // did parse; later Set Container Slot packets
                            // fill in the rest.
                            constexpr int32_t PLAYER_SLOTS = 36;
                            size_t containerCount = itemCount > PLAYER_SLOTS ? static_cast<size_t>(itemCount - PLAYER_SLOTS) : 0;
                            state->inventory.containerSlots.assign(containerCount, ItemStack{});
                            for (size_t i = 0; i < items.size(); i++)
                                state->inventory.SetWindowSlot(static_cast<int32_t>(i), items[i]);
                        }
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Container Content: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SetContainerSlot) {
            try {
                int32_t windowId = reader.ReadVarInt();
                int32_t stateId = reader.ReadVarInt();
                int32_t slotIndex = reader.ReadShort(); // i16 per protocol.json's packet_set_slot — NOT a varint, unlike every index/count field around it.
                // Last field in this packet, so ParsedSlot::complete doesn't
                // matter here the way it does for Set Container Content's
                // array — nothing reads past it either way.
                ParsedSlot slot = ReadItemStack(reader);

                std::lock_guard<std::mutex> lock(state->inventory.mutex);
                if (windowId == -1 && slotIndex == -1) {
                    state->inventory.carriedItem = slot.item;
                } else if (windowId == -2) {
                    state->inventory.SetSlot(slotIndex, slot.item);
                } else if (windowId == 0) {
                    state->inventory.playerStateId = stateId;
                    state->inventory.SetSlot(slotIndex, slot.item);
                } else if (windowId == state->inventory.activeWindowId) {
                    state->inventory.activeWindowStateId = stateId;
                    state->inventory.SetWindowSlot(slotIndex, slot.item);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Container Slot: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SetHeldItem) {
            try {
                int32_t slot = reader.ReadVarInt();
                if (slot >= 0 && slot < static_cast<int32_t>(InventoryManager::HOTBAR_SIZE)) {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    state->inventory.selectedHotbarSlot = static_cast<uint8_t>(slot);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Held Item: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::UpdateTime) {
            reader.ReadLong(); // age (world age, ticks since creation) — not needed for the sky-color gradient.
            int32_t clockCount = reader.ReadVarInt();
            for (int32_t i = 0; i < clockCount; i++) {
                int32_t clockId = reader.ReadVarInt();
                int64_t totalTicks = reader.ReadVarLong();
                reader.ReadFloat(); // partialTick, unused — this client re-samples dayTimeTicks fresh every frame rather than locally extrapolating between updates.
                reader.ReadFloat(); // rate, unused for the same reason.

                // Clock id 0 is assumed to be the day/night cycle clock —
                // undocumented in protocol.json beyond the field names, so
                // this is a best-effort guess like several other packets in
                // this file (see e.g. PlayerChatMessage's own comment). If a
                // server's clock 0 means something else, the sky gradient
                // will just track the wrong value rather than break anything.
                if (clockId == 0) {
                    int64_t dayTime = totalTicks % 24000;
                    if (dayTime < 0) dayTime += 24000;
                    state->dayTimeTicks.store(dayTime);
                }
            }
            continue;
        }

        if (packetId == PlayS2C::DamageEvent) {
            uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
            std::lock_guard<std::mutex> lock(state->entitiesMutex);
            auto it = state->entities.find(entityId);
            if (it != state->entities.end()) it->second.lastHurtTime = std::chrono::steady_clock::now();
            continue;
        }

        if (packetId == PlayS2C::ChunkBatchStart) {
            continue;
        }

        if (packetId == PlayS2C::ChunkBatchFinished) {
            // Vanilla's client reports its measured meshing rate here; a
            // fixed rate is fine since MeshingThread just queues the backlog.
            PacketWriter ack;
            ack.WriteFloat(20.0f);
            connection.SendPacket(PlayC2S::ChunkBatchReceived, ack.Data());
            continue;
        }

        if (packetId == PlayS2C::KeepAlive) {
            int64_t id = reader.ReadLong();
            PacketWriter response;
            response.WriteLong(id);
            connection.SendPacket(PlayC2S::KeepAlive, response.Data());
            continue;
        }

        if (packetId == PlayS2C::SynchronizePlayerPosition) {
            int32_t teleportId = reader.ReadVarInt();
            double x = reader.ReadDouble();
            double y = reader.ReadDouble();
            double z = reader.ReadDouble();

            // Trailing fields: velocity, rotation, and "relative" flags. A
            // flagged axis is an offset from where the client already is,
            // not an absolute position (vanilla: PositionMoveRotation::
            // calculateAbsolute) — treating it as absolute teleports the
            // player to the wrong place.
            double deltaX = 0.0, deltaY = 0.0, deltaZ = 0.0;
            float syncYaw = 0.0f, syncPitch = 0.0f;
            int32_t relativeFlags = 0;
            try {
                deltaX = reader.ReadDouble();
                deltaY = reader.ReadDouble();
                deltaZ = reader.ReadDouble();
                syncYaw = reader.ReadFloat();
                syncPitch = reader.ReadFloat();
                relativeFlags = reader.ReadInt();
            } catch (const std::exception&) {
                // Shorter packet than expected: treat everything as absolute.
            }

            glm::vec3 clientPosition = state->player ? state->player->GetPosition() : glm::vec3(0.0f);
            double serverX = x, serverY = y, serverZ = z;
            if (relativeFlags & 0x1) x += clientPosition.x;
            if (relativeFlags & 0x2) y += clientPosition.y;
            if (relativeFlags & 0x4) z += clientPosition.z;
            double offX = x - clientPosition.x, offY = y - clientPosition.y, offZ = z - clientPosition.z;

            PacketWriter confirm;
            confirm.WriteVarInt(teleportId);
            connection.SendPacket(PlayC2S::ConfirmTeleportation, confirm.Data());

            // Reporting our position is what makes servers that don't push
            // chunks unprompted start streaming them; TickThread's own
            // per-tick report keeps the server current after this first one.
            // onGround=false: right after a teleport, TickLoop hasn't run a
            // fixed step from the new position yet, so there's no real
            // grounded state to report — same as vanilla's own behavior
            // immediately following a teleport confirmation.
            //
            // yaw/pitch come from the camera, not this packet's own trailing
            // velocity/yaw/pitch/flags fields (left unread — see ParsedSlot's
            // sibling comment elsewhere on reading only what's needed): a
            // server-dictated facing on spawn is a real gap, but a
            // NetDiag-null state->player is guarded the same way TickThread
            // guards it, so this stays 0 there rather than crash. Negated
            // the same way TickThread negates its own per-tick report — the
            // camera's yaw/pitch turn the opposite way from Minecraft's own
            // convention (see AngleByteToRadians).
            float yaw = state->player ? std::remainder(-state->player->camera.GetYaw(), 360.0f) : 0.0f;
            float pitch = state->player ? -state->player->camera.GetPitch() : 0.0f;
            SendPlayerPosition(glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)), yaw, pitch, false);

            {
                std::lock_guard lock(state->networkInbox.mutex);
                state->networkInbox.spawnPosition = glm::vec3(
                    static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            }
            {
                std::ostringstream oss;
                oss << "[NET] Synchronized player position: ("
                    << x << ", " << y << ", " << z << ")"
                    << " | client was off by (" << offX << ", " << offY << ", " << offZ << ")"
                    << " | server sent (" << serverX << ", " << serverY << ", " << serverZ << ")"
                    << " velocity (" << deltaX << ", " << deltaY << ", " << deltaZ << ")"
                    << " relative flags 0x" << std::hex << relativeFlags;
                Log::Info(oss.str());
                (void)syncYaw;
                (void)syncPitch;
            }
            continue;
        }

        if (packetId == PlayS2C::ChunkDataAndUpdateLight) {
            try {
                auto chunk = ChunkParser::ParseChunkDataPacket(reader);
                std::lock_guard lock(state->networkInbox.mutex);
                state->networkInbox.chunks.push(std::move(chunk));
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse chunk data packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::UnloadChunk) {
            try {
                int32_t chunkZ = reader.ReadInt();
                int32_t chunkX = reader.ReadInt();

                std::lock_guard lock(state->networkInbox.mutex);
                // Drop any not-yet-meshed copy of this chunk still queued,
                // so it can't be inserted after the unload is processed.
                auto& queue = state->networkInbox.chunks;
                for (size_t i = 0, n = queue.size(); i < n; i++) {
                    std::unique_ptr<Chunk> queued = std::move(queue.front());
                    queue.pop();
                    if (queued->getX() != chunkX || queued->getZ() != chunkZ) queue.push(std::move(queued));
                }
                state->networkInbox.pendingUnloads.emplace_back(chunkX, chunkZ);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse unload chunk packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::MultiBlockChange) {
            try {
                int64_t packed = reader.ReadLong();
                int32_t sectionX = static_cast<int32_t>(packed >> 42);
                int32_t sectionZ = static_cast<int32_t>((packed << 22) >> 42);
                int32_t sectionY = static_cast<int32_t>((packed << 44) >> 44);

                int32_t count = reader.ReadVarInt();
                std::vector<PendingBlockEdit> edits;
                edits.reserve(static_cast<size_t>(std::max(count, 0)));
                for (int32_t i = 0; i < count; i++) {
                    int64_t record = static_cast<int64_t>(reader.ReadVarLong());
                    PendingBlockEdit edit;
                    edit.position = glm::ivec3(
                        sectionX * 16 + static_cast<int32_t>((record >> 8) & 15),
                        sectionY * 16 + static_cast<int32_t>(record & 15),
                        sectionZ * 16 + static_cast<int32_t>((record >> 4) & 15));
                    edit.block = BlockRegistry::ResolveStateId(static_cast<int32_t>(record >> 12));
                    edits.push_back(edit);
                }

                std::lock_guard lock(state->networkInbox.mutex);
                for (const PendingBlockEdit& edit : edits) state->networkInbox.pendingBlockEdits.push_back(edit);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse multi block change packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::BlockUpdate) {
            try {
                int32_t worldX, worldY, worldZ;
                reader.ReadPosition(worldX, worldY, worldZ);
                int32_t stateId = reader.ReadVarInt();

                PendingBlockEdit edit;
                edit.position = glm::ivec3(worldX, worldY, worldZ);
                edit.block = BlockRegistry::ResolveStateId(stateId);

                std::lock_guard lock(state->networkInbox.mutex);
                state->networkInbox.pendingBlockEdits.push_back(edit);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse block update packet: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SpawnEntity) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                std::array<uint8_t, 16> uuid{};
                reader.ReadBytes(uuid.data(), uuid.size());
                uint16_t entityType = static_cast<uint16_t>(reader.ReadVarInt());
                double x = reader.ReadDouble();
                double y = reader.ReadDouble();
                double z = reader.ReadDouble();
                // [velocity: lpVec3][pitch: i8][yaw: i8][headYaw: i8]
                // [objectData: varint], in that order per resources/maps/
                // protocol.json. The velocity's width varies (1 byte when
                // zero, 6+ otherwise) — ReadLpVec3 decodes it exactly, which
                // is what used to make every Spawn Entity packet fail to
                // parse back when it was skipped as a fixed 6 bytes (49 out
                // of 49 threw "Unexpected end of packet" in a 20s session,
                // since the overwhelming majority of spawns carry no
                // velocity at all and are short of a fixed 6-byte skip).
                ReadLpVec3(reader); // velocity, unused — remote entities are driven by position updates.
                reader.ReadByte(); // pitch, unused.
                int8_t yawByte = static_cast<int8_t>(reader.ReadByte());
                reader.ReadByte(); // headYaw, unused.
                int32_t objectData = reader.ReadVarInt(); // falling_block: block state id.

                Entity entity;
                entity.id = entityId;
                entity.type = entityType;
                entity.position = entity.previousPosition = glm::vec3(
                    static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                TraceEntity(state, "SPAWN", packetId, payload, entityId,
                    "type=" + std::to_string(entityType), glm::vec3(0.0f), entity.position);
                entity.yaw = entity.previousYaw = AngleByteToRadians(yawByte);
                entity.lastUpdateTime = std::chrono::steady_clock::now();

                const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entityType);
                if (info) {
                    entity.boundingBox = glm::vec2(info->width, info->height);
                    if (info->name == "falling_block") entity.blockStateId = objectData;
                    else if (info->name == "tnt") entity.blockStateId = BlockRegistry::DefaultStateIdForName("tnt");
                }

                // Other players (the dedicated Spawn Player packet is long
                // gone — they arrive here too) get the full PlayerEntity,
                // with their name and game mode picked up from the tab list,
                // which the server always fills in before spawning them.
                if (info && info->entityKind == "player") {
                    PlayerEntity player;
                    static_cast<Entity&>(player) = entity;
                    player.uuid = uuid;
                    {
                        std::lock_guard<std::mutex> lock(state->playerListMutex);
                        std::string key = FormatUuid(uuid);
                        if (auto it = state->playerList.find(key); it != state->playerList.end()) player.username = it->second;
                        if (auto it = state->playerGameModes.find(key); it != state->playerGameModes.end()) player.gameMode = it->second;
                    }

                    std::lock_guard lock(state->entitiesMutex);
                    state->entities.erase(entityId);
                    state->playerEntities[entityId] = std::move(player);
                } else {
                    std::lock_guard lock(state->entitiesMutex);
                    state->playerEntities.erase(entityId);
                    state->entities[entityId] = entity;
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Spawn Entity: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::RelEntityMove || packetId == PlayS2C::EntityMoveLook) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                int16_t rawX = reader.ReadShort();
                int16_t rawY = reader.ReadShort();
                int16_t rawZ = reader.ReadShort();
                glm::vec3 delta(FixedDeltaToBlocks(rawX), FixedDeltaToBlocks(rawY), FixedDeltaToBlocks(rawZ));

                bool hasRotation = (packetId == PlayS2C::EntityMoveLook);
                int8_t yawByte = hasRotation ? static_cast<int8_t>(reader.ReadByte()) : 0;
                if (hasRotation) reader.ReadByte(); // pitch, unused.
                reader.ReadBool(); // onGround, unused — no client-side physics for remote entities.

                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId)) {
                    glm::vec3 before = entity->position;
                    entity->previousPosition = entity->position;
                    entity->position += delta;
                    TraceEntity(state, "MOVE", packetId, payload, entityId,
                        "rawShorts=" + std::to_string(rawX) + "," + std::to_string(rawY) + "," + std::to_string(rawZ)
                            + " delta=" + std::to_string(delta.x) + "," + std::to_string(delta.y) + "," + std::to_string(delta.z),
                        before, entity->position);
                    if (hasRotation) {
                        entity->previousYaw = entity->yaw;
                        entity->yaw = AngleByteToRadians(yawByte);
                    }
                    entity->lastUpdateTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Move Entity Pos(Rot): ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::EntityLook) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                int8_t yawByte = static_cast<int8_t>(reader.ReadByte());
                reader.ReadByte(); // pitch, unused.
                reader.ReadBool(); // onGround, unused.

                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId)) {
                    entity->previousYaw = entity->yaw;
                    entity->yaw = AngleByteToRadians(yawByte);
                    entity->lastUpdateTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Move Entity Rot: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::EntityTeleport) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                glm::vec3 newPosition(
                    static_cast<float>(reader.ReadDouble()),
                    static_cast<float>(reader.ReadDouble()),
                    static_cast<float>(reader.ReadDouble()));
                int8_t yawByte = static_cast<int8_t>(reader.ReadByte());
                reader.ReadByte(); // pitch, unused.
                reader.ReadBool(); // onGround, unused.

                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId)) {
                    glm::vec3 before = entity->position;
                    entity->previousPosition = entity->position;
                    entity->position = newPosition;
                    TraceEntity(state, "TELEPORT", packetId, payload, entityId,
                        "pos=" + std::to_string(newPosition.x) + "," + std::to_string(newPosition.y) + "," + std::to_string(newPosition.z),
                        before, entity->position);
                    entity->previousYaw = entity->yaw;
                    entity->yaw = AngleByteToRadians(yawByte);
                    entity->lastUpdateTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Entity Position Sync: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::EntityDestroy) {
            try {
                int32_t count = reader.ReadVarInt();
                std::lock_guard lock(state->entitiesMutex);
                for (int32_t i = 0; i < count; i++) {
                    uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                    state->entities.erase(entityId);
                    state->playerEntities.erase(entityId);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Remove Entities: ") + e.what());
            }
            continue;
        }

        // Knockback. For our own entity id this replaces our velocity
        // outright — vanilla's handleSetEntityMotion does exactly that, and
        // it's how every hit we take (and any other server-side push)
        // actually moves us. Applied through GlobalState::localMotion since
        // TickLoop owns velocity. Other entities are driven entirely by
        // their position updates, so their velocity isn't needed.
        if (packetId == PlayS2C::SetEntityMotion) {
            try {
                int32_t entityId = reader.ReadVarInt();
                glm::vec3 velocity = ReadLpVec3(reader);
                TraceEntity(state, "MOTION", packetId, payload, static_cast<uint32_t>(entityId),
                    "velocity=" + std::to_string(velocity.x) + "," + std::to_string(velocity.y) + "," + std::to_string(velocity.z),
                    glm::vec3(0.0f), glm::vec3(0.0f));
                if (entityId == state->localEntityId.load()) {
                    state->localMotion.Push({ LocalMotionEvent::Kind::SetVelocity, velocity });
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Entity Motion: ") + e.what());
            }
            continue;
        }

        // Only the knockback part matters here: it's added onto our velocity
        // (vanilla's handleExplosion). Everything after it (particles,
        // sound, block particle list) is left unread.
        if (packetId == PlayS2C::Explosion) {
            try {
                reader.ReadDouble(); reader.ReadDouble(); reader.ReadDouble(); // center
                reader.ReadFloat(); // radius
                reader.ReadInt(); // blockCount
                if (reader.ReadBool()) {
                    glm::vec3 knockback(
                        static_cast<float>(reader.ReadDouble()),
                        static_cast<float>(reader.ReadDouble()),
                        static_cast<float>(reader.ReadDouble()));
                    state->localMotion.Push({ LocalMotionEvent::Kind::AddVelocity, knockback });
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Explosion: ") + e.what());
            }
            continue;
        }

        // Both mean "this entity just took damage" — vanilla starts its
        // 10-tick red hurt flash off either one. The rest of each packet
        // (damage source / hit direction) is only used for sounds and the
        // local camera tilt, neither of which exists here.
        if (packetId == PlayS2C::DamageEvent || packetId == PlayS2C::HurtAnimation) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId)) {
                    entity->lastHurtTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Damage Event/Hurt Animation: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::EntityEvent) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadInt());
                int8_t status = static_cast<int8_t>(reader.ReadByte());

                constexpr int8_t LEGACY_HURT = 2, DEATH = 3, TOTEM_OF_UNDYING = 35;
                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId)) {
                    if (status == LEGACY_HURT) entity->lastHurtTime = std::chrono::steady_clock::now();
                    else if (status == DEATH) entity->dead = true;
                    else if (status == TOTEM_OF_UNDYING) entity->dead = false;
                    // Everything else (shield block/break, eating, taming
                    // hearts, op-level updates, ...) has no visual here yet.
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Entity Event: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::Animate) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                uint8_t animation = reader.ReadByte();

                constexpr uint8_t SWING_MAIN_HAND = 0, SWING_OFF_HAND = 3, CRITICAL_HIT = 4, MAGIC_CRITICAL_HIT = 5;
                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId)) {
                    auto now = std::chrono::steady_clock::now();
                    if (animation == SWING_MAIN_HAND || animation == SWING_OFF_HAND) entity->lastSwingTime = now;
                    else if (animation == CRITICAL_HIT || animation == MAGIC_CRITICAL_HIT) entity->lastCritTime = now;
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Animate: ") + e.what());
            }
            continue;
        }

        // Another player's held items and armor. Entries are read until one
        // doesn't have the "more follow" bit (0x80) set on its slot byte —
        // or until a Slot carries data components, after which the rest of
        // the packet can't be located (see ParsedSlot::complete); whatever
        // was read up to that point still applies.
        if (packetId == PlayS2C::SetEquipment) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                std::vector<std::pair<uint8_t, ItemStack>> changes;
                for (;;) {
                    uint8_t slotByte = reader.ReadByte();
                    bool more = (slotByte & 0x80u) != 0;
                    ParsedSlot parsed = ReadItemStack(reader);
                    changes.emplace_back(static_cast<uint8_t>(slotByte & 0x7Fu), parsed.item);
                    if (!more || !parsed.complete) break;
                }

                std::lock_guard lock(state->entitiesMutex);
                if (PlayerEntity* player = state->FindPlayerEntity(entityId)) {
                    for (const auto& [slot, item] : changes) {
                        if (slot < EQUIPMENT_SLOT_COUNT) player->equipment[slot] = item;
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Equipment: ") + e.what());
            }
            continue;
        }

        // Only players' metadata is read (health, sneaking/sprinting/
        // invisible flags) — nothing else tracked here has a use for it yet.
        if (packetId == PlayS2C::SetEntityData) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                bool isPlayer;
                {
                    std::lock_guard lock(state->entitiesMutex);
                    isPlayer = state->FindPlayerEntity(entityId) != nullptr;
                }
                if (!isPlayer) continue;

                PlayerDataUpdate update = ReadPlayerEntityData(reader);

                std::lock_guard lock(state->entitiesMutex);
                if (PlayerEntity* player = state->FindPlayerEntity(entityId)) {
                    if (update.sharedFlags) {
                        player->sneaking = (*update.sharedFlags & 0x02u) != 0;
                        player->sprinting = (*update.sharedFlags & 0x08u) != 0;
                        player->invisible = (*update.sharedFlags & 0x20u) != 0;
                    }
                    if (update.health) {
                        player->health = *update.health;
                        // A player at 0 health is dying even if the Entity
                        // Event for it hasn't arrived (or never will).
                        if (*update.health <= 0.0f) player->dead = true;
                        else player->dead = false;
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Entity Data: ") + e.what());
            }
            continue;
        }

        // Our own attributes, fully resolved (base value plus modifiers) the
        // way vanilla's AttributeInstance.calculateValue does, into
        // state->attributes — this is what makes the attack cooldown follow
        // the held weapon (generic.attack_speed), and reach, movement speed,
        // gravity, etc. follow the server. Other entities' attributes aren't
        // needed.
        if (packetId == PlayS2C::UpdateAttributes) {
            try {
                int32_t entityId = reader.ReadVarInt();
                if (entityId != state->localEntityId.load() || state->attributes == nullptr) continue;

                int32_t count = reader.ReadVarInt();
                for (int32_t i = 0; i < count; i++) {
                    int32_t key = reader.ReadVarInt();
                    double base = reader.ReadDouble();
                    int32_t modifierCount = reader.ReadVarInt();

                    double addValue = 0.0, multiplyBase = 0.0, multiplyTotal = 1.0;
                    for (int32_t m = 0; m < modifierCount; m++) {
                        std::string modifierId = reader.ReadString();
                        double amount = reader.ReadDouble();
                        int8_t operation = static_cast<int8_t>(reader.ReadByte());
                        // TickLoop applies sprint speed itself — counting the
                        // server's own sprint modifier too would double it.
                        if (modifierId == "minecraft:sprinting") continue;
                        if (operation == 0) addValue += amount;
                        else if (operation == 1) multiplyBase += amount;
                        else if (operation == 2) multiplyTotal *= 1.0 + amount;
                    }

                    if (key < 0 || static_cast<size_t>(key) >= ATTRIBUTE_NAMES.size()) continue;
                    double value = base + addValue;
                    value = (value + value * multiplyBase) * multiplyTotal;
                    state->attributes->SetDouble(ATTRIBUTE_NAMES[static_cast<size_t>(key)], value);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Update Attributes: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::PlayerInfoUpdate) {
            try {
                uint8_t actions = reader.ReadByte();
                constexpr uint8_t ADD_PLAYER = 0x01, INITIALIZE_CHAT = 0x02, UPDATE_GAME_MODE = 0x04,
                                   UPDATE_LISTED = 0x08, UPDATE_LATENCY = 0x10, UPDATE_DISPLAY_NAME = 0x20,
                                   UPDATE_HAT = 0x40, UPDATE_LIST_ORDER = 0x80;

                int32_t count = reader.ReadVarInt();
                for (int32_t i = 0; i < count; i++) {
                    std::array<uint8_t, 16> uuid{};
                    reader.ReadBytes(uuid.data(), uuid.size());

                    std::optional<std::string> name;
                    std::optional<GameMode> gameMode;
                    if (actions & ADD_PLAYER) {
                        name = reader.ReadString();
                        int32_t propCount = reader.ReadVarInt();
                        for (int32_t p = 0; p < propCount; p++) {
                            reader.ReadString(); // property name — skin data, unused (no skin rendering yet)
                            reader.ReadString(); // property value
                            if (reader.ReadBool()) reader.ReadString(); // signature, when present
                        }
                    }
                    if (actions & INITIALIZE_CHAT) {
                        if (reader.ReadBool()) { // chat session present
                            reader.Skip(16); // session uuid
                            reader.ReadLong(); // public key expire time
                            reader.Skip(static_cast<size_t>(reader.ReadVarInt())); // key bytes
                            reader.Skip(static_cast<size_t>(reader.ReadVarInt())); // key signature
                        }
                    }
                    if (actions & UPDATE_GAME_MODE) gameMode = static_cast<GameMode>(reader.ReadVarInt());
                    if (actions & UPDATE_LISTED) reader.ReadVarInt();
                    if (actions & UPDATE_LATENCY) reader.ReadVarInt();
                    if (actions & UPDATE_DISPLAY_NAME) {
                        if (reader.ReadBool()) ReadTextComponent(reader); // display name NBT, unused — plain name is enough for now
                    }
                    if (actions & UPDATE_LIST_ORDER) reader.ReadVarInt();
                    if (actions & UPDATE_HAT) reader.ReadBool();

                    if (name.has_value() || gameMode.has_value()) {
                        std::lock_guard<std::mutex> lock(state->playerListMutex);
                        if (name.has_value()) state->playerList[FormatUuid(uuid)] = *name;
                        if (gameMode.has_value()) state->playerGameModes[FormatUuid(uuid)] = *gameMode;
                    }

                    // Keep an already-spawned PlayerEntity in step too
                    // (normally the tab-list entry comes first and Spawn
                    // Entity copies it — but a game mode change, e.g. into
                    // spectator, arrives later).
                    if (name.has_value() || gameMode.has_value()) {
                        std::lock_guard<std::mutex> lock(state->entitiesMutex);
                        for (auto& [id, player] : state->playerEntities) {
                            if (player.uuid != uuid) continue;
                            if (name.has_value()) player.username = *name;
                            if (gameMode.has_value()) player.gameMode = *gameMode;
                        }
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Player Info Update: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::PlayerInfoRemove) {
            try {
                int32_t count = reader.ReadVarInt();
                std::lock_guard<std::mutex> lock(state->playerListMutex);
                for (int32_t i = 0; i < count; i++) {
                    std::array<uint8_t, 16> uuid{};
                    reader.ReadBytes(uuid.data(), uuid.size());
                    state->playerList.erase(FormatUuid(uuid));
                    state->playerGameModes.erase(FormatUuid(uuid));
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Player Info Remove: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SystemChatMessage) {
            try {
                ChatEvent event;
                event.message = ReadTextComponent(reader);
                bool overlay = reader.ReadBool();
                if (!overlay) { // action bar messages aren't shown in the scrollback (no HUD for them yet)
                    GUI::Chat::AddMessage(event);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse System Chat Message: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::PlayerChatMessage) {
            // Logged unconditionally, before parsing even starts — the open
            // question is whether the server sends this packet back to the
            // sender at all, not just whether this client parses it right.
            // If this line never appears, the server isn't broadcasting the
            // message back (or is sending it via SystemChatMessage/a
            // different id instead); if it appears but no chat line shows
            // up, the bug is in parsing/display below.
            Log::Info("[NET] Received PlayerChatMessage, " + std::to_string(payload.size()) + " bytes");

            // Best-effort — see the UNVERIFIED note on PlayerChatMessage in
            // PacketIds.hpp. Anything read wrong here is contained to a
            // wrong-looking chat line or the catch below; it can't desync
            // the connection, since Connection::ReadPacket already framed
            // this whole payload by length.
            try {
                std::array<uint8_t, 16> senderUuid{};
                reader.ReadBytes(senderUuid.data(), senderUuid.size());
                reader.ReadVarInt(); // index

                if (reader.ReadBool()) reader.Skip(256); // message signature, fixed-length when present

                std::string plainBody = reader.ReadString();
                reader.ReadLong(); // timestamp
                reader.ReadLong(); // salt

                int32_t previousCount = reader.ReadVarInt();
                for (int32_t i = 0; i < previousCount; i++) {
                    int32_t id = reader.ReadVarInt();
                    if (id == 0) reader.Skip(256); // full signature, when this entry isn't a cache reference
                }

                std::vector<ChatSegment> message;
                if (reader.ReadBool()) message = ReadTextComponent(reader); // Unsigned Content: preferred display form
                if (message.empty()) message = { ChatSegment{ plainBody, 0xFFFFFFu } };

                int32_t filterType = reader.ReadVarInt();
                if (filterType == 2) { // PARTIALLY_FILTERED
                    int32_t longCount = reader.ReadVarInt();
                    reader.Skip(static_cast<size_t>(longCount) * 8);
                }

                reader.ReadVarInt(); // chat type registry id, unused — no per-type decoration formatting yet
                ChatEvent event;
                event.sender = ReadTextComponent(reader);
                if (reader.ReadBool()) ReadTextComponent(reader); // target name, unused (whisper/team target)
                event.message = std::move(message);

                GUI::Chat::AddMessage(event);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Player Chat Message: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::Disconnect) {
            // Same anonymousNbt reason field as the Configuration-state
            // Disconnect above — see that handler's comment. ReadString()
            // here meant a kick's real reason was never actually visible.
            std::string reason = PlainText(ReadTextComponent(reader));
            Log::Error("Server disconnected: " + reason);
            lastError = reason;
            return;
        }

        {
            // Logged once per distinct id, not once per occurrence — a busy
            // populated world sends routine high-frequency packets (entity
            // metadata, sound, particles, ...) this client doesn't handle
            // yet, and logging every single one was thousands of lines
            // (each one a real syscall — see Logger::Info's own comment)
            // competing with this same loop's own chunk-packet reads.
            static std::unordered_set<int32_t> loggedIds;
            if (loggedIds.insert(packetId).second) {
                std::ostringstream oss;
                oss << "[NET] Unhandled Play packet id 0x" << std::hex << packetId
                    << std::dec << " (" << payload.size() << " bytes), skipping.";
                Log::Info(oss.str());
            }
        }
    }
}

void SendChatMessage(GlobalState* state, const std::string& message)
{
    // The loaded copy keeps the connection alive for the rest of this
    // function even if the session ends mid-call — see
    // GlobalState::activeConnection's own comment.
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped outgoing chat message, no active connection: " + message);
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteString(message);
        writer.WriteLong(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        writer.WriteLong(0); // salt — 0 since this message is unsigned.
        writer.WriteBool(false); // signature present? No (see PacketIds.hpp's own comment).
        writer.WriteVarInt(0); // offset
        std::array<uint8_t, 3> acknowledged{}; // All-zero 20-bit "last seen" bitset — nothing acknowledged.
        writer.WriteBytes(acknowledged.data(), acknowledged.size());
        writer.WriteBytes(reinterpret_cast<const uint8_t*>("\x00"), 1); // checksum — see PacketIds.hpp's own comment.

        connection->QueuePacket(PlayC2S::ChatMessage, writer.Data());
        // Neither this nor SendChatCommand below logged anything on a
        // successful queue — "no chat logs at all" was consistent with both
        // "never called" and "sent fine, just no visible server response",
        // completely undiagnosable from the log alone. This line exists to
        // separate those two cases.
        Log::Info("[NET] Queued chat message: " + message);
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send chat message: ") + e.what());
    }
}

void SendChatCommand(GlobalState* state, const std::string& command)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped outgoing command, no active connection: /" + command);
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteString(command); // No leading "/" — the packet field is just the command text.
        connection->QueuePacket(PlayC2S::ChatCommand, writer.Data());
        Log::Info("[NET] Queued chat command: /" + command);
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send chat command: ") + e.what());
    }
}

void SendRespawnRequest(GlobalState* state)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped respawn request, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(0); // action 0 = perform_respawn.
        connection->QueuePacket(PlayC2S::ClientCommand, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send respawn request: ") + e.what());
    }
}

void SendAttack(GlobalState* state, uint32_t targetEntityId)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped attack, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(static_cast<int32_t>(targetEntityId));
        connection->QueuePacket(PlayC2S::Attack, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send attack: ") + e.what());
    }
}

void SendSwing(GlobalState* state, int32_t hand)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) return; // Nothing to animate for anyone without a session — not worth a log line per click.

    try {
        PacketWriter writer;
        writer.WriteVarInt(hand);
        connection->QueuePacket(PlayC2S::SwingArm, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send arm swing: ") + e.what());
    }
}

void SendHeldItemSlot(GlobalState* state, uint8_t slot)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped held-item-slot report, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteShort(static_cast<int16_t>(slot));
        connection->QueuePacket(PlayC2S::SetHeldItem, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send held item slot: ") + e.what());
    }
}

namespace {

void WriteHashedSlot(PacketWriter& writer, const ItemStack& item)
{
    writer.WriteVarInt(item.itemId);
    writer.WriteVarInt(item.count);
    writer.WriteVarInt(0); // No component hashes are tracked by ItemStack yet.
    writer.WriteVarInt(0); // No removed components.
}

void WriteOptionalHashedSlot(PacketWriter& writer, const ItemStack& item)
{
    bool present = !item.IsEmpty();
    writer.WriteBool(present);
    if (present) WriteHashedSlot(writer, item);
}

}

void SendInventoryClick(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                        uint8_t button, const ItemStack& slotItem, const ItemStack& carriedItem)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) return;

    try {
        PacketWriter writer;
        writer.WriteVarInt(windowId);
        writer.WriteVarInt(stateId);
        writer.WriteShort(static_cast<int16_t>(slot));
        uint8_t mouseButton = button;
        writer.WriteBytes(&mouseButton, 1);
        writer.WriteVarInt(0); // Mode 0: ordinary slot click.
        bool slotClicked = slot >= 0;
        writer.WriteVarInt(slotClicked ? 1 : 0);
        if (slotClicked) {
            writer.WriteShort(static_cast<int16_t>(slot));
            WriteOptionalHashedSlot(writer, slotItem);
        }
        WriteOptionalHashedSlot(writer, carriedItem);
        connection->QueuePacket(PlayC2S::WindowClick, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send inventory click: ") + e.what());
    }
}

void SendInventoryClose(GlobalState* state, int32_t windowId)
{
    if (windowId == 0) return;
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) return;
    Log::Info("Sending close window id=" + std::to_string(windowId));

    try {
        PacketWriter writer;
        writer.WriteVarInt(windowId);
        connection->QueuePacket(PlayC2S::CloseWindow, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to close inventory window: ") + e.what());
    }
}

void SendInteractEntity(GlobalState* state, uint32_t entityId, Hand hand, bool sneaking)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped entity interact, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(static_cast<int32_t>(entityId));
        writer.WriteVarInt(static_cast<int32_t>(hand));
        // location (lpVec3) — see PacketIds.hpp::UseEntity's own comment on
        // why this is always the entity's own origin for now.
        uint8_t zeroLpVec3 = 0; // lpVec3 encodes (0,0,0) as one zero byte — see SpawnEntity's velocity parse.
        writer.WriteBytes(&zeroLpVec3, 1);
        writer.WriteBool(sneaking);
        connection->QueuePacket(PlayC2S::UseEntity, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send entity interact: ") + e.what());
    }
}

void SendPlayerAction(GlobalState* state, DigStatus status, glm::ivec3 position, glm::ivec3 face)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped player action, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(static_cast<int32_t>(status));
        writer.WritePosition(position.x, position.y, position.z);
        uint8_t faceByte = static_cast<uint8_t>(DirectionToProtocolId(face));
        writer.WriteBytes(&faceByte, 1);
        writer.WriteVarInt(state->interactionSequence.fetch_add(1, std::memory_order_relaxed) + 1);
        connection->QueuePacket(PlayC2S::BlockDig, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send player action: ") + e.what());
    }
}

void SendArmAnimation(GlobalState* state, Hand hand)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped arm animation, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(static_cast<int32_t>(hand));
        connection->QueuePacket(PlayC2S::ArmAnimation, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send arm animation: ") + e.what());
    }
}

void SendBlockPlace(GlobalState* state, Hand hand, glm::ivec3 position, glm::ivec3 face, glm::vec3 cursor)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped block place, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(static_cast<int32_t>(hand));
        writer.WritePosition(position.x, position.y, position.z);
        writer.WriteVarInt(DirectionToProtocolId(face));
        writer.WriteFloat(cursor.x);
        writer.WriteFloat(cursor.y);
        writer.WriteFloat(cursor.z);
        writer.WriteBool(false); // insideBlock — the player's own head/feet occupying the target cell; not tracked yet.
        writer.WriteBool(false); // worldBorderHit — this client doesn't track the world border yet.
        writer.WriteVarInt(state->interactionSequence.fetch_add(1, std::memory_order_relaxed) + 1);
        connection->QueuePacket(PlayC2S::BlockPlace, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send block place: ") + e.what());
    }
}

void SendUseItem(GlobalState* state, Hand hand, float yaw, float pitch)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("Dropped use item, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(static_cast<int32_t>(hand));
        writer.WriteVarInt(state->interactionSequence.fetch_add(1, std::memory_order_relaxed) + 1);
        writer.WriteFloat(yaw);
        writer.WriteFloat(pitch);
        connection->QueuePacket(PlayC2S::UseItem, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send use item: ") + e.what());
    }
}

} // namespace Volcano
