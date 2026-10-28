#include "NetworkClient.hpp"
#include "VarInt.hpp"
#include "SlotSkipper.hpp"
#include "PacketIds.hpp"
#include "NetworkStats.hpp"
#include "../renderer/entity/models/EntityMotion.hpp"
#include <thread>
#include "TextComponent.hpp"
#include "models/ChatEvent.hpp"
#include "models/PacketEvent.hpp"
#include "../Logger.hpp"
#include "../ModLoader.hpp"
#include "../RideController.hpp"
#include "../audio/EntitySounds.hpp"
#include "../interaction/CombatSounds.hpp"
#include "../renderer/terrain/ChunkParser.hpp"
#include "../renderer/terrain/models/BlockRegistry.hpp"
#include "../renderer/gui/models/Chat.hpp"
#include "../renderer/entity/models/Entity.hpp"
#include "../renderer/entity/models/EntityRegistry.hpp"
#include "../renderer/particle/ParticleSystem.hpp"
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

// Tells mods a chat message arrived (CHAT_RECEIVED): sender and message flattened to plain text.
void FireChatReceived(GlobalState* state, const ChatEvent& event)
{
    if (state->modLoader == nullptr || !state->modLoader->HasHandlers(EventType::CHAT_RECEIVED)) return;

    std::string text;
    if (!event.sender.empty())
    {
        text += "<";
        for (const ChatSegment& segment : event.sender) text += segment.text;
        text += "> ";
    }
    for (const ChatSegment& segment : event.message) text += segment.text;

    VC_EventData data{ sizeof(VC_EventData), text.c_str(), 0, 0, 0, 0 };
    state->modLoader->Fire(EventType::CHAT_RECEIVED, &data);
}

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
// mesh's local +Z - the mirror image of Minecraft's own convention
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

// Puts an entity where the server says it is. A simulated entity (items,
// arrows, ...) keeps its own flight and fades any real difference out as a
// correction, rather than snapping to the position the server sent a few
// ticks ago.
void SetServerPosition(Entity& entity, const glm::vec3& serverPosition, std::chrono::steady_clock::time_point now)
{
    entity.serverPosition = serverPosition;
    if (!entity.simulated) {
        entity.previousPosition = entity.position;
        entity.position = serverPosition;
        entity.lastUpdateTime = now;
        return;
    }

    if (glm::length(serverPosition - entity.position) < 0.6f) return; // The simulation is close enough.
    entity.correction = entity.InterpolatedPosition(now) - serverPosition;
    entity.correctionTime = now;
    entity.previousPosition = entity.position = serverPosition;
    entity.interpSeconds = 1.0f / 20.0f;
    entity.lastUpdateTime = now;
}

void MoveServerPosition(Entity& entity, const glm::vec3& delta, std::chrono::steady_clock::time_point now)
{
    SetServerPosition(entity, (entity.simulated ? entity.serverPosition : entity.position) + delta, now);
}

// Minecraft's LpVec3
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

// Attribute ids as Update Attributes sends them
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

// x, y, z as three f64. Read as separate statements: the order a call's arguments are evaluated in is
// unspecified, and this compiler (MSVC conventions) does the last first, which once swapped x and z and sent every
// synced mob thousands of blocks away. Never read more than once inside one call's arguments.
glm::vec3 ReadVec3(PacketReader& reader)
{
    float x = static_cast<float>(reader.ReadDouble());
    float y = static_cast<float>(reader.ReadDouble());
    float z = static_cast<float>(reader.ReadDouble());
    return glm::vec3(x, y, z);
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

// Debug.EntityTraceNear: the same trace, for entities within NEAR_TRACE_BLOCKS of the player only
constexpr float NEAR_TRACE_BLOCKS = 8.0f;

bool EntityTraceNearEnabled(GlobalState* state)
{
    static const bool enabled = std::get<bool>(state->config->Get("Debug.EntityTraceNear", false));
    return enabled;
}

// Drops every riding link that names `id`, before that entity is erased.
// Caller holds entitiesMutex.
void UnlinkRider(GlobalState* state, uint32_t id)
{
    if (state->ridingEntityId.load() == static_cast<int32_t>(id)) state->ridingEntityId.store(-1);

    Entity* entity = state->FindEntity(id);
    if (entity == nullptr) return;

    for (uint32_t passengerId : entity->passengers)
    {
        if (Entity* passenger = state->FindEntity(passengerId)) passenger->vehicleId = -1;
    }
    if (entity->vehicleId >= 0)
    {
        if (Entity* vehicle = state->FindEntity(static_cast<uint32_t>(entity->vehicleId)))
        {
            std::erase(vehicle->passengers, id);
        }
    }
}

void TraceEntity(GlobalState* state, const char* label, int32_t packetId,
                 const std::vector<uint8_t>& payload, uint32_t entityId,
                 const std::string& parsed, glm::vec3 before, glm::vec3 after)
{
    if (!EntityTraceEnabled(state))
    {
        if (!EntityTraceNearEnabled(state) || !state->player) return;
        // Near either end of the move, so a jump away from the player is logged too.
        glm::vec3 p = state->player->GetPosition();
        if (glm::distance(p, before) > NEAR_TRACE_BLOCKS && glm::distance(p, after) > NEAR_TRACE_BLOCKS) return;
    }

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
    // False only if SlotSkipper couldn't skip this Slot's data components
    // (protocol.json missing, or an unknown/malformed component). The
    // post-1.20.5 encoding has no length prefix around a component payload,
    // so the reader's position is then untrusted and a caller reading more
    // than one Slot must stop after this one. item.itemId/count are correct
    // either way.
    bool complete = true;
};

ParsedSlot ReadItemStack(PacketReader& reader)
{
    ParsedSlot result;

    int32_t count = reader.ReadVarInt();
    if (count == 0) return result; // Empty slot

    result.item.itemId = reader.ReadVarInt();
    result.item.count = static_cast<uint8_t>(std::clamp(count, 0, 255));

    int32_t addedComponentCount = reader.ReadVarInt();
    int32_t removedComponentCount = reader.ReadVarInt();
    if (addedComponentCount == 0 && removedComponentCount == 0) return result;

    SlotSkipper& skipper = SlotSkipper::Instance();
    SlotComponentValues values;
    result.complete = skipper.SkipComponents(reader, addedComponentCount, removedComponentCount, values);
    result.item.damage = values.damage;
    result.item.maxDamage = values.maxDamage;
    return result;
}

// The Set Entity Data fields tracked on a PlayerEntity, when present in
// one packet
struct PlayerDataUpdate {
    std::optional<uint8_t> sharedFlags; // Index 0 (Entity.DATA_SHARED_FLAGS_ID).
    std::optional<int32_t> pose;        // Index 6 (Entity.DATA_POSE), Pose ordinal.
    std::optional<float> health;        // Index 9 (LivingEntity.DATA_HEALTH_ID).
    std::optional<int32_t> itemId;      // Index 8, item stack (ItemEntity.DATA_ITEM).
};

// Walks a Set Entity Data entry list (u8 index, varint type, value; 0xFF
// ends it) picking out the player fields above. Indices are from the
// define order in vanilla's Entity/LivingEntity (resources/26.2.zip):
// Entity 0-7, then LivingEntity 8-14 starting with its flags and health.
// Every value type is skipped by its protocol.json encoding; a type with no
// fixed shape to skip (particles, profiles, ...) ends the walk early -
// entries arrive in index order and the ones read here come first, so that
// only ever loses fields this doesn't track.
PlayerDataUpdate ReadPlayerEntityData(PacketReader& reader)
{
    constexpr uint8_t SHARED_FLAGS_INDEX = 0, POSE_INDEX = 6, ITEM_INDEX = 8, HEALTH_INDEX = 9, END_OF_DATA = 0xFF;
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
        case 20: // pose
        {
            int32_t value = reader.ReadVarInt();
            if (index == POSE_INDEX) update.pose = value;
            break;
        }
        case 1: case 12: case 14: case 15: case 19: case 42: // varint-encoded (int, direction, block state(s), optvarint, humanoid_arm)
        case 21: case 22: case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: // registry variants
        case 35: case 36: case 37: case 38: // sniffer/armadillo/copper golem states
            reader.ReadVarInt();
            break;
        case 2: reader.ReadVarLong(); break;
        case 4: reader.ReadString(); break;
        case 5: ReadTextComponent(reader); break;
        case 6: if (reader.ReadBool()) ReadTextComponent(reader); break;
        case 7:
        {
            ParsedSlot slot = ReadItemStack(reader);
            if (index == ITEM_INDEX) update.itemId = slot.item.IsEmpty() ? 0 : slot.item.itemId;
            if (!slot.complete) return update;
            break;
        }
        case 8: reader.ReadBool(); break;
        case 9: reader.Skip(12); break; // rotations: 3x f32
        case 10: reader.Skip(8); break; // block_pos: packed i64
        case 11: if (reader.ReadBool()) reader.Skip(8); break;
        case 13: if (reader.ReadBool()) reader.Skip(16); break; // optional_uuid
        case 18: reader.ReadVarInt(); reader.ReadVarInt(); reader.ReadVarInt(); break; // villager_data
        case 39: reader.Skip(12); break; // vector3: 3x f32
        case 40: reader.Skip(16); break; // quaternion: 4x f32
        default:
            return update; // particle(s), global pos, painting variant, profile
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
        // server has compression enabled (the common case)
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
                loginUsername = serverUsername;

                // Login Acknowledged (Login state, serverbound, empty body) -
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
                    << "Request packet - not handled yet).";
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
    {
        std::lock_guard<std::mutex> lock(state->localUsernameMutex);
        state->localUsername = loginUsername;
    }

    try {
        if (state->modLoader) state->modLoader->Fire(EventType::ENTER_CONFIG_STATE);
        if (!RunConfiguration(state)) return;
        if (state->modLoader) state->modLoader->Fire(EventType::ENTER_PLAY_STATE);
        RunPlayLoop(state, stopToken);
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Session error: ") + e.what());
        lastError = e.what();
    }
}

bool NetworkClient::RunConfiguration(GlobalState* state)
{
    // Client Information
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
    clientInfo.WriteVarInt(static_cast<int32_t>(std::min<uint32_t>(std::get<uint32_t>(state->config->Get("Graphics.Particles", uint32_t{0})), 2))); // particle status: all, decreased, minimal
    {
        std::ostringstream diagBytes;
        diagBytes << "[DIAG] Client Information bytes: ";
        for (uint8_t b : clientInfo.Data()) diagBytes << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << " ";
        Log::Debug(diagBytes.str());
    }
    if (!std::getenv("NETDIAG_SKIP_SEND")) {
        connection.SendPacket(ConfigC2S::ClientInformation, clientInfo.Data());
        Log::Debug("[DIAG] Client Information sent.");

        // Every real client sends this right after Client Information -
        // some servers' anti-bot plugins use its absence as a bot signal
        // and silently hide the connection from the tab list/`/list` (still
        // fully joined and receiving world state) rather than kicking it,
        // which looks exactly like a server-side mystery until you notice
        // the missing brand. Identifies honestly as "volcano-client" rather
        // than spoofing "vanilla"
        PacketWriter brand;
        brand.WriteString("minecraft:brand");
        brand.WriteString("volcano-client");
        connection.SendPacket(ConfigC2S::PluginMessage, brand.Data());
        Log::Debug("[DIAG] Brand plugin message sent.");
    } else {
        Log::Debug("[DIAG] Skipping Client Information send (NETDIAG_SKIP_SEND set).");
    }

    // Each packet below updates GlobalState::connectionDetail so the loading
    // screen can say what Configuration is up to
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
            // Same array schema in both directions
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

        if (packetId == ConfigS2C::KeepAlive) {
            // Same echo as the Play-state handler; vanilla's common listener
            // answers it in both states.
            int64_t id = reader.ReadLong();
            PacketWriter response;
            response.WriteLong(id);
            connection.SendPacket(ConfigC2S::KeepAlive, response.Data());
            continue;
        }

        if (packetId == ConfigS2C::Ping) {
            // Fixed 4-byte int, NOT a VarInt
            int32_t id = reader.ReadInt();
            PacketWriter pong;
            pong.WriteInt(id);
            connection.SendPacket(ConfigC2S::Pong, pong.Data());
            continue;
        }

        if (packetId == ConfigS2C::Disconnect) {
            // reason is `anonymousNbt` per this client's own bundled
            // protocol.json, not a plain string
            std::string reason = PlainText(ReadTextComponent(reader));
            Log::Error("Server disconnected during configuration: " + reason);
            lastError = reason;
            return false;
        }

        // Registry Data, Update Tags, Feature Flags, resource pack packets,
        // plugin messages, etc.
    }
}

void NetworkClient::SendPlayerPosition(glm::vec3 position, float yaw, float pitch, bool onGround)
{
    // Every outgoing position funnels through here, so this is the one place
    // that can catch a non-finite value regardless of which caller produced
    // it - see the "Invalid move player packet received" server kick this
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
    // MovementFlags is a u8 bitflag
    uint8_t flags = onGround ? 0x01 : 0x00;
    writer.WriteBytes(&flags, 1);
    connection.SendPacket(PlayC2S::SetPlayerPositionAndRotation, writer.Data());
}

void NetworkClient::RunPlayLoop(GlobalState* state, std::stop_token stopToken)
{
    // Bounds how long WaitForReadable() below can block with nothing to
    // read, so this loop can drive TickLoop::Tick() on its own steady
    // cadence even when the server goes quiet between packets (idle time
    // between Keep Alives can be many seconds) - see the tick-loop plan's
    // "blocker" section.
    constexpr auto kReadPollInterval = std::chrono::milliseconds(15);

    // Logs once per stall when this loop sits in one phase for over 3 s, so a
    // hang names the packet handler (or wait) it is stuck in. Locks held by
    // other threads show up here as a long "handling a packet".
    std::jthread watchdog([](std::stop_token token) {
        int64_t reportedTicks = 0;
        while (!token.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            int64_t since = networkStats.loopPhaseTicks.load(std::memory_order_relaxed);
            int32_t phase = networkStats.loopPhase.load(std::memory_order_relaxed);
            if (phase == 0 || since == reportedTicks) continue;
            double ageMs = NetworkStats::AgeMs(since);
            if (ageMs < 3000.0) continue;
            reportedTicks = since;
            char buffer[160];
            std::snprintf(buffer, sizeof(buffer), "[NET] Network thread stalled for %.0f ms while %s (packet id 0x%x)",
                ageMs, NetworkStats::LoopPhaseName(phase), static_cast<unsigned>(networkStats.loopPacketId.load()));
            Log::Error(buffer);
        }
    });

    // Packet listeners (PacketEvents) only see the play state.
    struct PlayTapScope {
        PlayTapScope() { PacketEvents::SetPlayActive(true); }
        ~PlayTapScope() { PacketEvents::SetPlayActive(false); }
    } playTapScope;

    while (!stopToken.stop_requested() && !state->shouldClose) {
        networkStats.SetLoopPhase(1);
        // TickLoop::Tick() (and with it, sending PlayC2S::PlayerLoaded and
        // the per-tick position/rotation/sprint/input report) used to happen
        // right here, once per RunPlayLoop iteration

        // Drain anything other threads queued (chat, commands, respawn -
        // see SendChatMessage). Must happen on this thread and outside
        // WaitForReadable below, since that call cancels socket operations
        // and a write racing it can be left half-written
        connection.FlushOutbound();

        networkStats.SetLoopPhase(2);
        if (!connection.WaitForReadable(kReadPollInterval)) {
            continue; // Nothing arrived within the poll window
        }

        std::vector<uint8_t> payload;
        networkStats.SetLoopPhase(3);
        int32_t packetId = connection.ReadPacket(payload);
        networkStats.SetLoopPhase(4, packetId);

        // Clientbound tap: listeners may edit the packet or drop it unhandled.
        PacketEvent packetEvent{ PacketEvent::Direction::CLIENTBOUND, packetId, payload };
        if (!PacketEvents::Dispatch(packetEvent)) continue;

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
                uint8_t gameMode = reader.ReadByte(); // worldState.gamemode
                state->localGameMode.store(gameMode, std::memory_order_relaxed);
                // Rest of SpawnInfo (previousGamemode, isDebug, isFlat, death,
                // portalCooldown, seaLevel) and enforcesSecureChat aren't
                // needed - nothing else in this packet is read.
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
                reader.ReadVarInt(); // dimension id
                std::string dimensionName = reader.ReadString();
                {
                    std::lock_guard<std::mutex> lock(state->currentDimensionMutex);
                    state->currentDimension = dimensionName;
                }
                Log::Info("Respawn: switching to dimension " + dimensionName);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Respawn (resetting world state anyway): ") + e.what());
            }

            // Sent on every dimension change, not just death
            state->ResetWorldState();

            // A respawn is a fresh player entity server-side: not sprinting,
            // no keys held. TickLoop resets its own side the same way once
            // the new spawn position lands (SyncToPlayerPosition) - that
            // naturally makes TickThread's next report a stop_sprinting/
            // cleared-input diff against whatever it last told the server.
            continue;
        }

        if (packetId == PlayS2C::PlayerCombatKill) {
            reader.ReadVarInt(); // playerId
            std::string message = PlainText(ReadTextComponent(reader));
            state->ReportDeath(message);
            continue;
        }

        if (packetId == PlayS2C::SetExperience) {
            float progress = reader.ReadFloat();
            int32_t level = reader.ReadVarInt();
            reader.ReadVarInt(); // total experience
            {
                std::lock_guard<std::mutex> lock(state->experienceMutex);
                state->experienceProgress = progress;
                state->experienceLevel = level;
            }
            continue;
        }

        if (packetId == PlayS2C::SetHealth) {
            float health = reader.ReadFloat();
            int32_t food = reader.ReadVarInt();
            float saturation = reader.ReadFloat();

            // Stored for GUIController::RenderPlayerStatusBars' HUD display
            // - same manual-mutex-next-to-data convention as entities/
            // entitiesMutex (see GlobalState's own comment on health/food/
            // saturation/healthMutex).
            {
                std::lock_guard<std::mutex> lock(state->healthMutex);
                state->health = health;
                state->food = food;
                state->saturation = saturation;
            }

            // Catches joining a server while already dead from a previous
            // session - PlayerCombatKill above only fires at the actual
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
                            + " carries item data components this client doesn't decode yet - "
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
                int32_t slotIndex = reader.ReadShort(); // i16 per protocol.json's packet_set_slot
                // Last field in this packet, so ParsedSlot::complete doesn't
                // matter here the way it does for Set Container Content's
                // array - nothing reads past it either way.
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

        if (packetId == PlayS2C::SetPlayerInventory) {
            try {
                int32_t inventoryIndex = reader.ReadVarInt();
                ParsedSlot slot = ReadItemStack(reader);

                // Inventory numbering to the player window's protocol slots.
                int32_t windowSlot = -1;
                if (inventoryIndex >= 0 && inventoryIndex <= 8) windowSlot = 36 + inventoryIndex;
                else if (inventoryIndex >= 9 && inventoryIndex <= 35) windowSlot = inventoryIndex;
                else if (inventoryIndex >= 36 && inventoryIndex <= 39) windowSlot = 44 - inventoryIndex; // boots 36 -> 8 ... helmet 39 -> 5.
                else if (inventoryIndex == 40) windowSlot = 45;

                if (windowSlot >= 0) {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    state->inventory.SetSlot(windowSlot, slot.item);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Player Inventory: ") + e.what());
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

        // A tick's worth of packets arrives bundled; each boundary counts as a
        // server tick for GlobalState::serverTicks.
        if (packetId == PlayS2C::BundleDelimiter) {
            state->serverTicks.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        if (packetId == PlayS2C::UpdateTime) {
            state->serverTicks.fetch_add(1, std::memory_order_relaxed); // Keeps counting on a server that bundles nothing.
            reader.ReadLong(); // age (world age, ticks since creation)
            int32_t clockCount = reader.ReadVarInt();
            for (int32_t i = 0; i < clockCount; i++) {
                int32_t clockId = reader.ReadVarInt();
                int64_t totalTicks = reader.ReadVarLong();
                reader.ReadFloat(); // partialTick, unused
                reader.ReadFloat(); // rate, unused for the same reason.

                // Clock id 0 is assumed to be the day/night cycle clock -
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

            // Copy what the sound needs and play it outside the lock.
            uint16_t type = 0;
            glm::vec3 hurtPosition{0.0f};
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId))
                {
                    entity->lastHurtTime = std::chrono::steady_clock::now();
                    type = entity->type;
                    hurtPosition = entity->position;
                    found = true;
                }
            }

            if (state->entitySounds == nullptr) continue;
            if (static_cast<int32_t>(entityId) == state->localEntityId.load())
            {
                state->entitySounds->PlayHurt("player", 0.0f);
            }
            else if (found)
            {
                if (const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(type))
                {
                    float distance = state->player ? glm::distance(hurtPosition, state->player->GetPosition()) : 0.0f;
                    state->entitySounds->PlayHurt(info->name, distance);
                }
            }
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

        if (packetId == PlayS2C::Ping) {
            PacketWriter pong;
            pong.WriteInt(reader.ReadInt());
            connection.SendPacket(PlayC2S::Pong, pong.Data());
            continue;
        }

        if (packetId == PlayS2C::KeepAlive) {
            int64_t id = reader.ReadLong();
            networkStats.RecordKeepAliveIn(id);
            PacketWriter response;
            response.WriteLong(id);
            connection.SendPacket(PlayC2S::KeepAlive, response.Data());
            networkStats.RecordKeepAliveOut();
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
            // calculateAbsolute) - treating it as absolute teleports the
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
            // grounded state to report
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
                // zero, 6+ otherwise) - ReadLpVec3 decodes it exactly, which
                // is what used to make every Spawn Entity packet fail to
                // parse back when it was skipped as a fixed 6 bytes (49 out
                // of 49 threw "Unexpected end of packet" in a 20s session,
                // since the overwhelming majority of spawns carry no
                // velocity at all and are short of a fixed 6-byte skip).
                glm::vec3 spawnVelocity = ReadLpVec3(reader); // Only dropped items simulate with it.
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
                entity.velocity = spawnVelocity;
                entity.lastUpdateTime = std::chrono::steady_clock::now();

                const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entityType);
                if (info) {
                    entity.boundingBox = glm::vec2(info->width, info->height);
                    if (MotionFor(info->name).simulated) {
                        entity.simulated = true;
                        entity.serverPosition = entity.position;
                        entity.interpSeconds = 1.0f / 20.0f;
                    }
                    if (info->name == "falling_block") entity.blockStateId = objectData;
                    else if (info->name == "tnt") entity.blockStateId = BlockRegistry::DefaultStateIdForName("tnt");
                }

                // Every arrow's spawn is its shot, ours or anyone else's.
                if (info && state->combatSounds != nullptr
                    && (info->name == "arrow" || info->name == "spectral_arrow")) {
                    float distance = state->player ? glm::distance(entity.position, state->player->GetPosition()) : 0.0f;
                    state->combatSounds->PlayArrowShot(distance);
                }

                // Other players (the dedicated Spawn Player packet is long
                // gone - they arrive here too) get the full PlayerEntity,
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
                reader.ReadBool(); // onGround, unused

                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId); entity != nullptr && !entity->locallyControlled) {
                    glm::vec3 before = entity->position;
                    MoveServerPosition(*entity, delta, std::chrono::steady_clock::now());
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
                if (Entity* entity = state->FindEntity(entityId); entity != nullptr && !entity->locallyControlled) {
                    entity->previousYaw = entity->yaw;
                    entity->yaw = AngleByteToRadians(yawByte);
                    entity->lastUpdateTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Move Entity Rot: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SyncEntityPosition) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                glm::vec3 newPosition = ReadVec3(reader);
                glm::vec3 velocity = ReadVec3(reader);
                float yawDegrees = reader.ReadFloat();
                reader.ReadFloat(); // pitch, unused.
                reader.ReadBool();  // onGround, unused.

                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId); entity != nullptr && !entity->locallyControlled) {
                    glm::vec3 before = entity->position;
                    SetServerPosition(*entity, newPosition, std::chrono::steady_clock::now());
                    entity->velocity = velocity;
                    TraceEntity(state, "SYNC", packetId, payload, entityId,
                        "pos=" + std::to_string(newPosition.x) + "," + std::to_string(newPosition.y) + "," + std::to_string(newPosition.z),
                        before, entity->position);
                    entity->previousYaw = entity->yaw;
                    entity->yaw = -glm::radians(yawDegrees); // Same turn direction as AngleByteToRadians.
                    entity->lastUpdateTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Sync Entity Position: ") + e.what());
            }
            continue;
        }

        // TELEPORT_ENTITY, as vanilla 26.x's ClientboundTeleportEntityPacket: position and velocity (f64), yaw and
        // pitch (f32 degrees), then an int of which of those are relative to the entity's own (vanilla's Relative:
        // bit 0-2 X/Y/Z, 3 yaw, 4 pitch, 5-7 velocity), then onGround. A relative coordinate taken as absolute puts
        // the entity near the world origin.
        if (packetId == PlayS2C::EntityTeleport) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                glm::vec3 position = ReadVec3(reader);
                glm::vec3 velocity = ReadVec3(reader);
                float yawDegrees = reader.ReadFloat();
                reader.ReadFloat(); // pitch, unused.
                uint32_t relative = static_cast<uint32_t>(reader.ReadInt());
                reader.ReadBool(); // onGround, unused.

                std::lock_guard lock(state->entitiesMutex);
                if (Entity* entity = state->FindEntity(entityId); entity != nullptr && !entity->locallyControlled) {
                    glm::vec3 before = entity->position;
                    glm::vec3 base = entity->simulated ? entity->serverPosition : entity->position;
                    glm::vec3 newPosition(
                        (relative & 0x01u) ? base.x + position.x : position.x,
                        (relative & 0x02u) ? base.y + position.y : position.y,
                        (relative & 0x04u) ? base.z + position.z : position.z);
                    SetServerPosition(*entity, newPosition, std::chrono::steady_clock::now());
                    entity->velocity = glm::vec3(
                        (relative & 0x20u) ? entity->velocity.x + velocity.x : velocity.x,
                        (relative & 0x40u) ? entity->velocity.y + velocity.y : velocity.y,
                        (relative & 0x80u) ? entity->velocity.z + velocity.z : velocity.z);
                    TraceEntity(state, "TELEPORT", packetId, payload, entityId,
                        "pos=" + std::to_string(position.x) + "," + std::to_string(position.y) + "," + std::to_string(position.z)
                            + " relativeBits=" + std::to_string(relative),
                        before, entity->position);
                    float yaw = -glm::radians(yawDegrees); // Same turn direction as AngleByteToRadians.
                    entity->previousYaw = entity->yaw;
                    entity->yaw = (relative & 0x08u) ? entity->yaw + yaw : yaw;
                    entity->lastUpdateTime = std::chrono::steady_clock::now();
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Teleport Entity: ") + e.what());
            }
            continue;
        }

        // The full rider list of a vehicle: replaces the previous one. The
        // local player has no Entity, so being listed sets ridingEntityId.
        if (packetId == PlayS2C::SetPassengers) {
            try {
                uint32_t vehicleId = static_cast<uint32_t>(reader.ReadVarInt());
                int32_t count = reader.ReadVarInt();
                std::vector<uint32_t> riders;
                for (int32_t i = 0; i < count; i++) riders.push_back(static_cast<uint32_t>(reader.ReadVarInt()));

                uint32_t localId = static_cast<uint32_t>(state->localEntityId.load());
                std::lock_guard lock(state->entitiesMutex);
                Entity* vehicle = state->FindEntity(vehicleId);
                if (vehicle != nullptr) {
                    for (uint32_t oldId : vehicle->passengers) {
                        if (oldId == localId) continue;
                        if (Entity* passenger = state->FindEntity(oldId)) passenger->vehicleId = -1;
                    }
                }
                if (state->ridingEntityId.load() == static_cast<int32_t>(vehicleId)) state->ridingEntityId.store(-1);

                for (uint32_t riderId : riders) {
                    if (riderId == localId) {
                        state->ridingEntityId.store(static_cast<int32_t>(vehicleId));
                    } else if (Entity* passenger = state->FindEntity(riderId)) {
                        passenger->vehicleId = static_cast<int32_t>(vehicleId);
                    }
                }
                if (vehicle != nullptr) vehicle->passengers = std::move(riders);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Passengers: ") + e.what());
            }
            continue;
        }

        // The server snapping the vehicle we steer; RideController applies it.
        if (packetId == PlayS2C::VehicleMove) {
            try {
                glm::vec3 position = ReadVec3(reader);
                float yawDegrees = reader.ReadFloat();
                float pitchDegrees = reader.ReadFloat();
                if (state->rideController != nullptr && state->ridingEntityId.load() >= 0) {
                    state->rideController->ApplyServerMove(position, yawDegrees, pitchDegrees);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Vehicle Move: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::EntityDestroy) {
            try {
                int32_t count = reader.ReadVarInt();
                std::lock_guard lock(state->entitiesMutex);
                for (int32_t i = 0; i < count; i++) {
                    uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                    glm::vec3 lastPosition{0.0f};
                    if (const Entity* entity = state->FindEntity(entityId)) lastPosition = entity->position;
                    TraceEntity(state, "REMOVE", packetId, payload, entityId, "", lastPosition, lastPosition);
                    UnlinkRider(state, entityId);
                    state->entities.erase(entityId);
                    state->playerEntities.erase(entityId);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Remove Entities: ") + e.what());
            }
            continue;
        }

        // Knockback. For our own entity id this replaces our velocity
        // outright
        if (packetId == PlayS2C::SetEntityMotion) {
            try {
                int32_t entityId = reader.ReadVarInt();
                glm::vec3 velocity = ReadLpVec3(reader);
                if (entityId == state->localEntityId.load()) {
                    state->localMotion.Push({ LocalMotionEvent::Kind::SetVelocity, velocity });
                } else {
                    std::lock_guard lock(state->entitiesMutex);
                    if (Entity* entity = state->FindEntity(static_cast<uint32_t>(entityId))) {
                        entity->velocity = velocity;
                        TraceEntity(state, "MOTION", packetId, payload, static_cast<uint32_t>(entityId),
                            "velocity=" + std::to_string(velocity.x) + "," + std::to_string(velocity.y) + "," + std::to_string(velocity.z),
                            entity->position, entity->position);
                    }
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
                    glm::vec3 knockback = ReadVec3(reader);
                    state->localMotion.Push({ LocalMotionEvent::Kind::AddVelocity, knockback });
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Explosion: ") + e.what());
            }
            continue;
        }

        // Both mean "this entity just took damage"
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
                    else if (animation == CRITICAL_HIT || animation == MAGIC_CRITICAL_HIT)
                    {
                        entity->lastCritTime = now;
                        if (state->particles != nullptr)
                        {
                            // A burst around the entity, as vanilla's tracking emitter makes over a few ticks.
                            ParticleSpawn burst;
                            burst.type = state->particles->TypeId(animation == MAGIC_CRITICAL_HIT ? "enchanted_hit" : "crit");
                            burst.position = glm::dvec3(entity->position) + glm::dvec3(0.0, entity->boundingBox.y * 0.5f, 0.0);
                            burst.spread = glm::vec3(entity->boundingBox.x * 0.4f, entity->boundingBox.y * 0.3f, entity->boundingBox.x * 0.4f);
                            burst.speed = 0.35f;
                            burst.count = 24;
                            state->particles->Spawn(burst);
                        }
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Animate: ") + e.what());
            }
            continue;
        }

        // A player's or mob's held items and armor. Entries are read until one
        // doesn't have the "more follow" bit (0x80) set on its slot byte -
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
                if (Entity* wearer = state->FindEntity(entityId)) {
                    for (const auto& [slot, item] : changes) {
                        if (slot < EQUIPMENT_SLOT_COUNT) wearer->equipment[slot] = item;
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Equipment: ") + e.what());
            }
            continue;
        }

        // Only players' metadata is read (health, sneaking/sprinting/
        // invisible flags) - nothing else tracked here has a use for it yet.
        if (packetId == PlayS2C::SetEntityData) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                bool isPlayer;
                bool isItem = false;
                {
                    std::lock_guard lock(state->entitiesMutex);
                    isPlayer = state->FindPlayerEntity(entityId) != nullptr;
                    if (!isPlayer)
                    {
                        if (const Entity* entity = state->FindEntity(entityId))
                        {
                            const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity->type);
                            isItem = info != nullptr && info->name == "item";
                        }
                    }
                }
                if (!isPlayer && !isItem) continue;

                PlayerDataUpdate update = ReadPlayerEntityData(reader);

                std::lock_guard lock(state->entitiesMutex);
                if (isItem)
                {
                    if (update.itemId)
                    {
                        if (Entity* entity = state->FindEntity(entityId)) entity->itemId = *update.itemId;
                    }
                    continue;
                }
                if (PlayerEntity* player = state->FindPlayerEntity(entityId)) {
                    if (update.sharedFlags) {
                        player->sneaking = (*update.sharedFlags & 0x02u) != 0;
                        player->sprinting = (*update.sharedFlags & 0x08u) != 0;
                        player->swimming = (*update.sharedFlags & 0x10u) != 0;
                        player->invisible = (*update.sharedFlags & 0x20u) != 0;
                    }
                    if (update.pose) player->pose = *update.pose;
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
        // state->attributes - this is what makes the attack cooldown follow
        // the held weapon (generic.attack_speed), and reach, movement speed,
        // gravity, etc. follow the server. Other entities' attributes aren't
        // needed.
        if (packetId == PlayS2C::UpdateAttributes) {
            try {
                int32_t entityId = reader.ReadVarInt();
                // Other entities only keep what a mount rides on.
                bool isLocal = entityId == state->localEntityId.load();
                if (isLocal && state->attributes == nullptr) continue;

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
                        // TickLoop applies sprint speed itself
                        if (modifierId == "minecraft:sprinting") continue;
                        if (operation == 0) addValue += amount;
                        else if (operation == 1) multiplyBase += amount;
                        else if (operation == 2) multiplyTotal *= 1.0 + amount;
                    }

                    if (key < 0 || static_cast<size_t>(key) >= ATTRIBUTE_NAMES.size()) continue;
                    double value = base + addValue;
                    value = (value + value * multiplyBase) * multiplyTotal;
                    if (isLocal) {
                        state->attributes->SetDouble(ATTRIBUTE_NAMES[static_cast<size_t>(key)], value);
                    } else {
                        std::string_view name = ATTRIBUTE_NAMES[static_cast<size_t>(key)];
                        std::lock_guard lock(state->entitiesMutex);
                        if (Entity* entity = state->FindEntity(static_cast<uint32_t>(entityId))) {
                            if (name == "generic.movement_speed") entity->movementSpeed = static_cast<float>(value);
                            else if (name == "generic.jump_strength") entity->jumpStrength = static_cast<float>(value);
                        }
                    }
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Update Attributes: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::Commands) {
            try {
                std::shared_ptr<const CommandTree> tree = CommandTree::Read(reader);
                Log::Info("[NET] Command tree: " + std::to_string(tree->Size()) + " nodes");
                std::lock_guard<std::mutex> lock(state->commandsMutex);
                state->commands = std::move(tree);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Commands: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::WorldParticles) {
            try {
                reader.ReadBool(); // long distance: nothing here depends on it
                ParticleSpawn spawn;
                spawn.alwaysShow = reader.ReadBool();
                double x = reader.ReadDouble();
                double y = reader.ReadDouble();
                double z = reader.ReadDouble();
                spawn.position = glm::dvec3(x, y, z);
                spawn.spread.x = reader.ReadFloat();
                spawn.spread.y = reader.ReadFloat();
                spawn.spread.z = reader.ReadFloat();
                spawn.speed = reader.ReadFloat();
                spawn.count = reader.ReadInt();
                spawn.type = reader.ReadVarInt();
                if (state->particles != nullptr) {
                    // Only the types that carry a color or size need their data read: it is the last thing in the packet.
                    const std::string& name = state->particles->TypeName(spawn.type);
                    if (name == "dust") {
                        spawn.color = static_cast<uint32_t>(reader.ReadInt());
                        spawn.scale = std::clamp(reader.ReadFloat(), 0.01f, 4.0f);
                    } else if (name == "dust_color_transition") {
                        spawn.color = static_cast<uint32_t>(reader.ReadInt());
                        reader.ReadInt(); // the color it fades to
                        spawn.scale = std::clamp(reader.ReadFloat(), 0.01f, 4.0f);
                    } else if (name == "entity_effect" || name == "tinted_leaves") {
                        spawn.color = static_cast<uint32_t>(reader.ReadInt());
                    } else if (name == "effect" || name == "instant_effect") {
                        spawn.color = static_cast<uint32_t>(reader.ReadInt());
                    }
                    state->particles->Spawn(spawn);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Level Particles: ") + e.what());
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
                            reader.ReadString(); // property name
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
                        if (reader.ReadBool()) ReadTextComponent(reader); // display name NBT, unused
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
                    // Entity copies it
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
                FireChatReceived(state, event);
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse System Chat Message: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::PlayerChatMessage) {
            // Logged unconditionally, before parsing even starts
            Log::Info("[NET] Received PlayerChatMessage, " + std::to_string(payload.size()) + " bytes");

            // Best-effort
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

                reader.ReadVarInt(); // chat type registry id, unused
                ChatEvent event;
                event.sender = ReadTextComponent(reader);
                if (reader.ReadBool()) ReadTextComponent(reader); // target name, unused (whisper/team target)
                event.message = std::move(message);

                GUI::Chat::AddMessage(event);
                FireChatReceived(state, event);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Player Chat Message: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::Disconnect) {
            // Same anonymousNbt reason field as the Configuration-state
            // Disconnect above
            std::string reason = PlainText(ReadTextComponent(reader));
            Log::Error("Server disconnected: " + reason);
            lastError = reason;
            return;
        }

        {
            // Logged once per distinct id, not once per occurrence
            static std::unordered_set<int32_t> loggedIds;
            if (loggedIds.insert(packetId).second) {
                std::ostringstream oss;
                oss << "\x1b[90mNetwork:\x1b[0m Unhandled Play packet id \x1b[38;2;130;180;140m0x" << std::hex << packetId
                    << std::dec << "\x1b[0m (" << payload.size() << " bytes).";
                Log::Info(oss.str());
            }
        }
    }
}

void SendChatMessage(GlobalState* state, const std::string& message)
{
    // The loaded copy keeps the connection alive for the rest of this
    // function even if the session ends mid-call
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
        writer.WriteLong(0); // salt - 0 since this message is unsigned.
        writer.WriteBool(false); // signature present? No (see PacketIds.hpp's own comment).
        writer.WriteVarInt(0); // offset
        std::array<uint8_t, 3> acknowledged{}; // All-zero 20-bit "last seen" bitset
        writer.WriteBytes(acknowledged.data(), acknowledged.size());
        writer.WriteBytes(reinterpret_cast<const uint8_t*>("\x00"), 1); // checksum

        connection->QueuePacket(PlayC2S::ChatMessage, writer.Data());
        // Neither this nor SendChatCommand below logged anything on a
        // successful queue
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
        writer.WriteString(command); // No leading "/" - the packet field is just the command text.
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
    if (connection == nullptr) return; // Nothing to animate for anyone without a session

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

void SendInventoryAction(GlobalState* state, int32_t windowId, int32_t stateId, int32_t slot,
                         uint8_t button, int32_t mode, const ItemStack& carriedItem)
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
        writer.WriteVarInt(mode);
        writer.WriteVarInt(0); // Changed slots: none claimed
        WriteOptionalHashedSlot(writer, carriedItem);
        connection->QueuePacket(PlayC2S::WindowClick, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send inventory action: ") + e.what());
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
        // location (lpVec3) - see PacketIds.hpp::UseEntity's own comment on
        // why this is always the entity's own origin for now.
        uint8_t zeroLpVec3 = 0; // lpVec3 encodes (0,0,0) as one zero byte
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
    state->localSwingNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);

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
        writer.WriteBool(false); // insideBlock
        writer.WriteBool(false); // worldBorderHit
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
