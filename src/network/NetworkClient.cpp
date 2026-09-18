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
#include <optional>
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

void NetworkClient::SendPlayerPosition(glm::vec3 position, bool onGround)
{
    PacketWriter writer;
    writer.WriteDouble(static_cast<double>(position.x));
    writer.WriteDouble(static_cast<double>(position.y));
    writer.WriteDouble(static_cast<double>(position.z));
    // MovementFlags is a u8 bitflag — bit 0 onGround, bit 1
    // hasHorizontalCollision (protocol.json's MovementFlags type). This
    // client doesn't track horizontal-collision state, so that bit always
    // stays 0.
    uint8_t flags = onGround ? 0x01 : 0x00;
    writer.WriteBytes(&flags, 1);
    connection.SendPacket(PlayC2S::SetPlayerPosition, writer.Data());
}

void NetworkClient::RunPlayLoop(GlobalState* state, std::stop_token stopToken)
{
    // Bounds how long WaitForReadable() below can block with nothing to
    // read, so this loop can drive TickLoop::Tick() on its own steady
    // cadence even when the server goes quiet between packets (idle time
    // between Keep Alives can be many seconds) — see the tick-loop plan's
    // "blocker" section.
    constexpr auto kReadPollInterval = std::chrono::milliseconds(15);

    // Vanilla clients report position roughly once per tick — without this,
    // the server only ever learns where the player is once (right after the
    // initial teleport, see SendPlayerPosition's call site below), and from
    // its perspective the player never moves again: no fall distance to
    // apply fall damage from, no sprint distance to accumulate hunger
    // exhaustion from, and no reason to push new chunks as the player
    // actually walks around.
    constexpr auto kPositionReportInterval = std::chrono::milliseconds(50); // 1 tick.
    auto lastPositionReport = std::chrono::steady_clock::now();

    while (!stopToken.stop_requested() && !state->shouldClose) {
        // Called every iteration, not just on a timeout/lull — the callback
        // (TickLoop::Tick(), wired up by NetworkThread — see
        // SetTickCallback's own comment) times itself against real elapsed
        // wall time, so calling it here even while packets are arriving
        // back-to-back (e.g. a burst of chunk data right after spawn) keeps
        // ticking steady through the burst instead of freezing
        // movement/gravity until it lets up. Its return value is this
        // tick's grounded state (TickLoop::IsGrounded()) — defaults to
        // false when tickCallback isn't set (NetDiag), matching
        // SendPlayerPosition's own onGround=false default before this existed.
        bool grounded = tickCallback ? tickCallback() : false;

        // Null-checked the same reason tickCallback is a callback rather
        // than a direct state->tickLoop-> call: NetDiag (tools/
        // netdiag_main.cpp) runs this same loop against a headless
        // GlobalState with no Player. Player::GetPosition() is a lock-free
        // atomic read (see Player.hpp), safe to call from this thread even
        // though NetworkThread never writes it (TickLoop, on this same
        // thread, does).
        if (state->player) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastPositionReport >= kPositionReportInterval) {
                SendPlayerPosition(state->player->GetPosition(), grounded);
                lastPositionReport = now;
            }
        }

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
            Log::Info("[NET] Entered Play state.");
            continue;
        }

        if (packetId == PlayS2C::Respawn) {
            try {
                reader.ReadVarInt(); // dimension id — unused, the name below is enough for the log line.
                std::string dimensionName = reader.ReadString();
                Log::Info("[NET] Respawn: switching to dimension " + dimensionName);
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

        if (packetId == PlayS2C::SetContainerContent) {
            try {
                int32_t windowId = reader.ReadVarInt();
                reader.ReadVarInt(); // stateId — unused; this client never sends Click Container Slot, so there's no revision number to echo back.
                int32_t itemCount = reader.ReadVarInt();

                if (windowId != 0) {
                    // Not the player's own inventory — this client can't open a
                    // server-side container (chest, furnace, ...) yet (see
                    // PacketIds.hpp's own note), so there's nothing to apply
                    // this to. Nothing else in this packet is needed either.
                    continue;
                }

                bool appliedAll = true;
                {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    for (int32_t i = 0; i < itemCount; i++) {
                        ParsedSlot slot = ReadItemStack(reader);
                        state->inventory.SetSlot(i, slot.item);
                        if (!slot.complete) {
                            appliedAll = false;
                            Log::Info("[NET] Set Container Content: slot " + std::to_string(i)
                                + " carries item data components this client doesn't decode yet — "
                                + std::to_string(itemCount - i - 1) + " remaining slot(s) in this packet "
                                + "not applied; a later Set Container Slot update will still catch up "
                                + "individual changes.");
                            break;
                        }
                    }
                }
                // carriedItem: Slot — trailing field. Only safe to read (and
                // only worth reading) if every array entry above parsed
                // cleanly; this client has no cursor-held-item state to put
                // it in anyway (no interactive inventory yet), so it's read
                // and discarded purely to leave the reader's position
                // correct for symmetry with the rest of this file's handlers
                // — nothing after this in RunPlayLoop's loop actually needs it.
                if (appliedAll) ReadItemStack(reader);
            } catch (const std::exception& e) {
                Log::Error(std::string("[NET] Failed to parse Set Container Content: ") + e.what());
            }
            continue;
        }

        if (packetId == PlayS2C::SetContainerSlot) {
            try {
                int32_t windowId = reader.ReadVarInt();
                reader.ReadVarInt(); // stateId — unused, see Set Container Content's own comment.
                int32_t slotIndex = reader.ReadShort(); // i16 per protocol.json's packet_set_slot — NOT a varint, unlike every index/count field around it.
                // Last field in this packet, so ParsedSlot::complete doesn't
                // matter here the way it does for Set Container Content's
                // array — nothing reads past it either way.
                ParsedSlot slot = ReadItemStack(reader);

                // windowId -1/-2 are vanilla's "cursor item"/"any open
                // window" sentinels, not the player's own inventory — this
                // client has no cursor-held-item state, so both fall through
                // and are ignored the same as any other non-zero windowId.
                if (windowId == 0) {
                    std::lock_guard<std::mutex> lock(state->inventory.mutex);
                    state->inventory.SetSlot(slotIndex, slot.item);
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
            // chunks unprompted start streaming them; RunPlayLoop's own
            // periodic call keeps the server current after this first one.
            // onGround=false: right after a teleport, TickLoop hasn't run a
            // fixed step from the new position yet, so there's no real
            // grounded state to report — same as vanilla's own behavior
            // immediately following a teleport confirmation.
            SendPlayerPosition(glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)), false);

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
                // Everything after z is [velocity: lpVec3][pitch: i8]
                // [yaw: i8][headPitch: i8][objectData: varint] — that order
                // is straight out of resources/maps/protocol.json, velocity
                // really does precede the angles.
                //
                // lpVec3's WIDTH, though, is not in the schema: protocol.json
                // declares it "native", meaning minecraft-data encodes it in
                // code. It is not a fixed width. Hex-dumping the post-z tail
                // from a live session against the dev server shows a zero
                // velocity occupying a single 0x00 byte and a moving
                // entity's occupying 6 (e.g. tail `f9 ff 7f fe eb ed 00 00
                // 00 00`, vs `00 00 c0 00 05` for a stationary one). A fixed
                // Skip(6) is what used to make EVERY Spawn Entity packet
                // throw "Unexpected end of packet" — 49 out of 49 in a 20s
                // session — because the overwhelming majority of spawns
                // carry no velocity at all and are 5 bytes short of it.
                //
                // Rather than hard-code either width (guessing the width is
                // what produced that bug), resolve it per packet against the
                // one property this tail definitely has: those four fields
                // consume it exactly, to the byte. Try each known width and
                // keep the one that lands precisely on the end of the
                // packet. Self-checking, and if a future protocol change
                // makes every candidate wrong it degrades to a logged skip
                // of one entity instead of a throw.
                const uint8_t* tail = reader.Cursor();
                const size_t tailLen = reader.Remaining();
                constexpr size_t kVelocityWidths[] = { 1, 6 };

                int8_t yawByte = 0;
                int fittingWidths = 0;
                for (size_t width : kVelocityWidths) {
                    if (tailLen < width + 3 + 1) continue; // no room for the angles + a varint

                    // objectData is the last field, so its varint has to
                    // start right after the angles and terminate on the
                    // final byte of the packet for this width to be right.
                    size_t pos = width + 3;
                    bool terminated = false;
                    while (pos < tailLen && pos - (width + 3) < 5) {
                        bool isLast = (tail[pos] & 0x80u) == 0;
                        pos++;
                        if (isLast) { terminated = true; break; }
                    }
                    if (!terminated || pos != tailLen) continue;

                    fittingWidths++;
                    yawByte = static_cast<int8_t>(tail[width + 1]);
                }

                // Ambiguous (two widths both fit) is as untrustworthy as
                // none fitting — either way we'd be guessing at the yaw.
                if (fittingWidths != 1) {
                    Log::Error("[NET] Spawn Entity tail (" + std::to_string(tailLen)
                        + " bytes) matched no unambiguous velocity encoding — skipping entity "
                        + std::to_string(entityId));
                    continue;
                }

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
                    if (actions & UPDATE_GAME_MODE) reader.ReadVarInt();
                    if (actions & UPDATE_LISTED) reader.ReadVarInt();
                    if (actions & UPDATE_LATENCY) reader.ReadVarInt();
                    if (actions & UPDATE_DISPLAY_NAME) {
                        if (reader.ReadBool()) ReadTextComponent(reader); // display name NBT, unused — plain name is enough for now
                    }
                    if (actions & UPDATE_LIST_ORDER) reader.ReadVarInt();
                    if (actions & UPDATE_HAT) reader.ReadBool();

                    if (name.has_value()) {
                        std::lock_guard<std::mutex> lock(state->playerListMutex);
                        state->playerList[FormatUuid(uuid)] = *name;
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
    // The loaded copy keeps the connection alive for the rest of this
    // function even if the session ends mid-call — see
    // GlobalState::activeConnection's own comment.
    std::shared_ptr<Connection> connection = state->activeConnection.load();
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

        connection->QueuePacket(PlayC2S::ChatMessage, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send chat message: ") + e.what());
    }
}

void SendChatCommand(GlobalState* state, const std::string& command)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped outgoing command, no active connection: /" + command);
        return;
    }

    try {
        PacketWriter writer;
        writer.WriteString(command); // No leading "/" — the packet field is just the command text.
        connection->QueuePacket(PlayC2S::ChatCommand, writer.Data());
    } catch (const std::exception& e) {
        Log::Error(std::string("[NET] Failed to send chat command: ") + e.what());
    }
}

void SendRespawnRequest(GlobalState* state)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped respawn request, no active connection.");
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

void SendHeldItemSlot(GlobalState* state, uint8_t slot)
{
    std::shared_ptr<Connection> connection = state->activeConnection.load();
    if (connection == nullptr) {
        Log::Info("[NET] Dropped held-item-slot report, no active connection.");
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

} // namespace Volcano
