#include "NetworkClient.hpp"
#include "VarInt.hpp"
#include "PacketIds.hpp"
#include "TextComponent.hpp"
#include "models/ChatEvent.hpp"
#include "../Logger.hpp"
#include "../renderer/terrain/ChunkParser.hpp"
#include "../renderer/gui/models/Chat.hpp"
#include "../renderer/entity/models/Entity.hpp"
#include "../renderer/entity/models/EntityRegistry.hpp"
#include <glm/gtc/constants.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>

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
// (256 steps per turn), matching the shader's own yaw convention (see
// entity.vert: forward = (-sin(yaw), 0, cos(yaw)), the same "0 = looking
// +Z, increasing per Minecraft's own rotation direction" convention this
// byte encodes) — so no sign flip is needed converting it to the radians
// Entity::yaw stores.
float AngleByteToRadians(int8_t angleByte)
{
    return static_cast<float>(angleByte) * (glm::pi<float>() / 128.0f);
}

// Update Entity Position packets encode movement as a fixed-point delta:
// 1/4096th of a block per unit, in a signed short (matches vanilla's own
// (currentX*32 - prevX*32)*128 encoding, which inverts to value/4096.0).
float FixedDeltaToBlocks(int16_t delta)
{
    return static_cast<float>(delta) / 4096.0f;
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

} // namespace

bool NetworkClient::ConnectAndLogin(const std::string& host, uint16_t port, const std::string& username)
{
    try {
        Log::Info("[NET] Connecting to " + host + ":" + std::to_string(port) + "...");
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

        Log::Info("[NET] Sent Handshake + Login Start as '" + username
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
                Log::Info("[NET] Set Compression: threshold " + std::to_string(compressionThreshold));
                continue;
            }

            if (packetId == 0x02) {
                std::array<uint8_t, 16> serverUuid{};
                reader.ReadBytes(serverUuid.data(), serverUuid.size());
                std::string serverUsername = reader.ReadString();
                Log::Info("[NET] Login Success: " + serverUsername
                          + " (" + FormatUuid(serverUuid) + ")");

                // Login Acknowledged (Login state, serverbound, empty body) —
                // required since the Configuration state was introduced: the
                // server stays in Login state, and any Configuration packet
                // we send is misinterpreted (or the connection is dropped
                // outright), until the client explicitly acks Login Success.
                PacketWriter loginAcknowledged;
                connection.SendPacket(0x03, loginAcknowledged.Data());
                Log::Info("[NET] Sent Login Acknowledged. Configuration state handling comes next.");
                return true;
            }

            if (packetId == 0x00) {
                std::string reason = reader.ReadString();
                Log::Error("[NET] Server disconnected during login: " + reason);
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

    for (;;) {
        std::vector<uint8_t> payload;
        int32_t packetId = connection.ReadPacket(payload);
        PacketReader reader(payload.data(), payload.size());

        if (packetId == ConfigS2C::FinishConfiguration) {
            PacketWriter ack;
            connection.SendPacket(ConfigC2S::AcknowledgeFinishConfiguration, ack.Data());
            Log::Info("[NET] Configuration finished, entering Play state.");
            return true;
        }

        if (packetId == ConfigS2C::KnownPacks) {
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
            int32_t id = reader.ReadVarInt();
            PacketWriter pong;
            pong.WriteVarInt(id);
            connection.SendPacket(ConfigC2S::Pong, pong.Data());
            continue;
        }

        if (packetId == ConfigS2C::Disconnect) {
            // reason is `anonymousNbt` per this client's own bundled
            // protocol.json, not a plain string — ReadString() here used to
            // misparse the NBT tag bytes as a length-prefixed string,
            // producing garbage (or throwing) instead of the real reason.
            std::string reason = PlainText(ReadTextComponent(reader));
            Log::Error("[NET] Server disconnected during configuration: " + reason);
            lastError = reason;
            return false;
        }

        // Registry Data, Update Tags, Feature Flags, resource pack packets,
        // plugin messages, etc. — nothing here we need to act on, and
        // Connection::ReadPacket already framed the whole payload so it's
        // safe to just move on to the next packet.
    }
}

void NetworkClient::RunPlayLoop(GlobalState* state, std::stop_token stopToken)
{
    while (!stopToken.stop_requested() && !state->shouldClose) {
        std::vector<uint8_t> payload;
        int32_t packetId = connection.ReadPacket(payload);
        PacketReader reader(payload.data(), payload.size());

        if (packetId == PlayS2C::LoginPlay) {
            Log::Info("[NET] Entered Play state.");
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
            reader.ReadVarInt(); // food, unused
            reader.ReadFloat();  // saturation, unused

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

            PacketWriter confirm;
            confirm.WriteVarInt(teleportId);
            connection.SendPacket(PlayC2S::ConfirmTeleportation, confirm.Data());

            // Reporting our position is what makes servers that don't push
            // chunks unprompted start streaming them.
            PacketWriter setPos;
            setPos.WriteDouble(x);
            setPos.WriteDouble(y);
            setPos.WriteDouble(z);
            setPos.WriteBytes(reinterpret_cast<const uint8_t*>("\x00"), 1); // movement flags
            connection.SendPacket(PlayC2S::SetPlayerPosition, setPos.Data());

            {
                std::lock_guard lock(state->networkInbox.mutex);
                state->networkInbox.spawnPosition = glm::vec3(
                    static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
            }
            {
                std::ostringstream oss;
                oss << "[NET] Synchronized player position: ("
                    << x << ", " << y << ", " << z << ")";
                Log::Info(oss.str());
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

        if (packetId == PlayS2C::SpawnEntity) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                reader.Skip(16); // objectUUID — not tracked per-entity yet.
                uint16_t entityType = static_cast<uint16_t>(reader.ReadVarInt());
                double x = reader.ReadDouble();
                double y = reader.ReadDouble();
                double z = reader.ReadDouble();
                reader.Skip(6); // velocity: 3x i16, unused (no client-side prediction for remote entities).
                reader.ReadByte(); // pitch — not modeled yet, see Entity::yaw's own comment.
                int8_t yawByte = static_cast<int8_t>(reader.ReadByte());
                reader.ReadByte(); // headPitch — ditto.
                reader.ReadVarInt(); // objectData — object-specific (item frame rotation, etc.), unused.

                Entity entity;
                entity.id = entityId;
                entity.type = entityType;
                entity.position = entity.previousPosition = glm::vec3(
                    static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
                entity.yaw = entity.previousYaw = AngleByteToRadians(yawByte);
                entity.lastUpdateTime = std::chrono::steady_clock::now();

                if (const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entityType)) {
                    entity.boundingBox = glm::vec2(info->width, info->height);
                }

                std::lock_guard lock(state->entitiesMutex);
                state->entities[entityId] = entity;
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Spawn Entity: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::RelEntityMove || packetId == PlayS2C::EntityMoveLook) {
            try {
                uint32_t entityId = static_cast<uint32_t>(reader.ReadVarInt());
                glm::vec3 delta(
                    FixedDeltaToBlocks(reader.ReadShort()),
                    FixedDeltaToBlocks(reader.ReadShort()),
                    FixedDeltaToBlocks(reader.ReadShort()));

                bool hasRotation = (packetId == PlayS2C::EntityMoveLook);
                int8_t yawByte = hasRotation ? static_cast<int8_t>(reader.ReadByte()) : 0;
                if (hasRotation) reader.ReadByte(); // pitch, unused.
                reader.ReadBool(); // onGround, unused — no client-side physics for remote entities.

                std::lock_guard lock(state->entitiesMutex);
                auto it = state->entities.find(entityId);
                if (it != state->entities.end()) {
                    Entity& entity = it->second;
                    entity.previousPosition = entity.position;
                    entity.position += delta;
                    if (hasRotation) {
                        entity.previousYaw = entity.yaw;
                        entity.yaw = AngleByteToRadians(yawByte);
                    }
                    entity.lastUpdateTime = std::chrono::steady_clock::now();
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
                auto it = state->entities.find(entityId);
                if (it != state->entities.end()) {
                    Entity& entity = it->second;
                    entity.previousYaw = entity.yaw;
                    entity.yaw = AngleByteToRadians(yawByte);
                    entity.lastUpdateTime = std::chrono::steady_clock::now();
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
                auto it = state->entities.find(entityId);
                if (it != state->entities.end()) {
                    Entity& entity = it->second;
                    entity.previousPosition = entity.position;
                    entity.position = newPosition;
                    entity.previousYaw = entity.yaw;
                    entity.yaw = AngleByteToRadians(yawByte);
                    entity.lastUpdateTime = std::chrono::steady_clock::now();
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
                    state->entities.erase(static_cast<uint32_t>(reader.ReadVarInt()));
                }
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Remove Entities: ") + e.what());
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
            Log::Error("[NET] Server disconnected: " + reason);
            lastError = reason;
            return;
        }

        {
            std::ostringstream oss;
            oss << "[NET] Unhandled Play packet id 0x" << std::hex << packetId
                << std::dec << " (" << payload.size() << " bytes), skipping.";
            Log::Info(oss.str());
        }
    }
}

void SendChatMessage(GlobalState* state, const std::string& message)
{
    Connection* connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped outgoing chat message, no active connection: " + message);
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

        connection->SendPacket(PlayC2S::ChatMessage, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send chat message: ") + e.what());
    }
}

void SendChatCommand(GlobalState* state, const std::string& command)
{
    Connection* connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped outgoing command, no active connection: /" + command);
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteString(command); // No leading "/" — the packet field is just the command text.
        connection->SendPacket(PlayC2S::ChatCommand, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send chat command: ") + e.what());
    }
}

void SendRespawnRequest(GlobalState* state)
{
    Connection* connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped respawn request, no active connection.");
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteVarInt(0); // action 0 = perform_respawn.
        connection->SendPacket(PlayC2S::ClientCommand, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send respawn request: ") + e.what());
    }
}

} // namespace Volcano
