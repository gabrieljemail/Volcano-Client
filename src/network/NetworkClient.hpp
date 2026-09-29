#pragma once
#ifndef VOLCANO_NETWORK_CLIENT_H
#define VOLCANO_NETWORK_CLIENT_H

#include <asio.hpp>
#include <cstdint>
#include <stop_token>
#include <string>
#include <glm/glm.hpp>
#include "Connection.hpp"
#include "../GlobalState.hpp"
#include "../interaction/InteractionTypes.hpp"

namespace Volcano {

// Protocol 775 = Minecraft 26.1 — matches minecraft-data's block/entity id
// tables exactly (776/26.2's aren't available yet, and numeric block-state
// ids don't degrade gracefully across a version gap — see BlockRegistry.cpp).
// This only changes what we tell the server; it doesn't help against one
// that's genuinely running 26.2.
constexpr int32_t PROTOCOL_VERSION = 775;

class NetworkClient {
public:
    explicit NetworkClient(asio::io_context& ioContext) : connection(ioContext) {}

    // Connects and runs Handshake -> Login Start, then reads and logs the
    // server's first response (Login Success or Disconnect). Returns true
    // on Login Success.
    bool ConnectAndLogin(const std::string& host, uint16_t port, const std::string& username);

    // Runs Configuration state to completion, then pumps the Play state
    // packet loop until disconnected or stopToken is cancelled. Parsed
    // chunks and the server-reported spawn position are handed off via
    // state->networkInbox for the main thread to consume — this method
    // never touches state->world/renderList directly.
    void RunSession(GlobalState* state, std::stop_token stopToken);

    // Exposes the underlying Connection so NetworkThread can publish it to
    // GlobalState::activeConnection once the Play session actually starts
    // (see NetworkThread::ThreadEntry) — that's what lets
    // NetworkClient::SendChatMessage reach it from another thread.
    Connection& GetConnection() { return connection; }

    // Human-readable reason the session ended (a kick's chat-component
    // text, an exception message, ...) — set alongside every disconnect/
    // error Log::Error() call in ConnectAndLogin/RunSession/RunConfiguration
    // /RunPlayLoop, empty if the session is still live. NetworkThread reads
    // this once ConnectAndLogin/RunSession returns to tell GlobalState why,
    // so the main thread can show it on the connect screen.
    const std::string& GetLastError() const { return lastError; }

private:
    Connection connection;
    std::string lastError;

    // Reads Graphics.RenderDistance from state->config for the Client
    // Information packet — see the definition for why it's declared to the
    // server here (along with the rest of Client Information).
    bool RunConfiguration(GlobalState* state);
    void RunPlayLoop(GlobalState* state, std::stop_token stopToken);

    // Reports the player's current position+rotation via
    // PlayC2S::SetPlayerPositionAndRotation. Called once, right after the
    // server's initial teleport, to kick off chunk streaming on a server
    // that doesn't push chunks unprompted — see RunPlayLoop's own comment.
    // Every report after this first one is TickThread's job now (queued on
    // a steady per-tick cadence, not sent directly here) — see TickThread's
    // own comment for why that moved off this (network) thread entirely.
    //
    // yaw/pitch (degrees, Camera's own convention — see Camera.hpp).
    // `onGround` feeds MovementFlags.onGround — get this wrong (e.g.
    // hardcode false) and a server that cross-checks it against the
    // reported Y will treat every tick spent standing still as
    // inconsistent and rubber-band the position back.
    void SendPlayerPosition(glm::vec3 position, float yaw, float pitch, bool onGround);
};

// Sends a plain (unsigned) chat message via state->activeConnection — a
// no-op (logged, not thrown) if there's no live session, e.g. chat was
// opened before connecting or the connection just dropped. Callable from
// any thread; see PlayC2S::ChatMessage and Connection::QueuePacket's own
// comments for why this is safe to call from outside the network thread
// (it queues rather than writing the socket directly).
void SendChatMessage(GlobalState* state, const std::string& message);

// Same as SendChatMessage but for a command (no leading "/") — sent via
// PlayC2S::ChatCommand so the server actually executes it through its
// command dispatcher instead of broadcasting it as a literal chat line.
void SendChatCommand(GlobalState* state, const std::string& command);

// Requests respawn after death (PlayC2S::ClientCommand, action 0) — the
// death screen's Respawn button.
void SendRespawnRequest(GlobalState* state);

// Tells the server which hotbar slot (0-8) is now selected, via
// PlayC2S::SetHeldItem — called whenever RenderThread::PollInputs applies a
// hotbar-switch key locally (see InventoryManager::selectedHotbarSlot's own
// comment for why this is separate client state that needs its own report,
// not something the server infers). Same call-from-any-thread reasoning as
// SendChatMessage above; in practice always called from the render thread.
void SendHeldItemSlot(GlobalState* state, uint8_t slot);

// Interaction packets — one function per wire packet (not per gameplay
// event; see src/interaction/events/*.hpp for the higher-level "what does
// an attack/break/place/use actually do" objects that call these). Same
// call-from-any-thread contract as SendChatMessage above; in practice these
// are all called from InteractionManager on the render thread.

// PlayC2S::Attack — melee-attacking an entity. The server decides for
// itself whether this lands as a plain, critical, sprint-knockback or sweep
// hit, from its own copy of our state (sprinting, falling, held weapon,
// attack cooldown) — which is why TickThread keeps it current. In practice
// called by InteractionManager, followed by SendSwing.
void SendAttack(GlobalState* state, uint32_t entityId);

// PlayC2S::UseEntity — right-clicking an entity. See its own PacketIds.hpp
// comment for why location is always (0,0,0) for now.
void SendInteractEntity(GlobalState* state, uint32_t entityId, Hand hand, bool sneaking);

// PlayC2S::BlockDig — one step of breaking a block (see DigStatus). `face`
// is the block face being mined, e.g. TargetedBlock::face.
void SendPlayerAction(GlobalState* state, DigStatus status, glm::ivec3 position, glm::ivec3 face);

// PlayC2S::ArmAnimation — the visual swing. Vanilla sends this alongside
// every attack/dig/use/place, not as a substitute for one.
void SendArmAnimation(GlobalState* state, Hand hand);

// PlayC2S::BlockPlace — using an item against a targeted block (placing a
// block, using a bucket/bonemeal/etc. on one). `position`/`face` describe
// the EXISTING block being clicked, same as TargetedBlock; `cursor` is the
// 0..1 hit point within that face (see PlaceBlockEvent for how it's
// approximated without real per-pixel raycast precision).
void SendBlockPlace(GlobalState* state, Hand hand, glm::ivec3 position, glm::ivec3 face, glm::vec3 cursor);

// PlayC2S::UseItem — right-click with nothing targeted (eat, drink, draw a
// bow, ...). `yaw`/`pitch` are the player's own current look rotation
// (degrees) — the packet's `rotation` field.
void SendUseItem(GlobalState* state, Hand hand, float yaw, float pitch);

// Swings an arm (PlayC2S::SwingArm — an alias of ArmAnimation, see
// PacketIds.hpp) — the animation other players see. hand: 0 = main hand,
// 1 = offhand. Same wire packet as SendArmAnimation above; kept as its own
// function since AttackEntity's callers pass a raw hand index rather than
// the Hand enum.
void SendSwing(GlobalState* state, int32_t hand);

} // namespace Volcano

#endif
