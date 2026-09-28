#pragma once
#ifndef VOLCANO_NETWORK_CLIENT_H
#define VOLCANO_NETWORK_CLIENT_H

#include <asio.hpp>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <string>
#include <glm/glm.hpp>
#include "Connection.hpp"
#include "../GlobalState.hpp"

namespace Volcano {

// Protocol 775 = Minecraft 26.1 — matches minecraft-data's block/entity id
// tables exactly (776/26.2's aren't available yet, and numeric block-state
// ids don't degrade gracefully across a version gap — see BlockRegistry.cpp).
// This only changes what we tell the server; it doesn't help against one
// that's genuinely running 26.2.
constexpr int32_t PROTOCOL_VERSION = 775;

// What NetworkClient needs to report to the server after each TickLoop::
// Tick() — see NetworkClient::SetTickCallback. Plain data so NetworkClient
// never has to know TickLoop exists (NetDiag links this file without it).
struct TickReport {
    bool grounded = false;            // MovementFlags bit 0.
    bool horizontalCollision = false; // MovementFlags bit 1.
    bool sprinting = false;           // PlayC2S::PlayerCommand start/stop_sprinting, sent on change.
    uint8_t inputFlags = 0;           // PlayC2S::PlayerInput bitflags, sent on change.
};

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

    // Called once per RunPlayLoop iteration (see its own comment) instead
    // of RunPlayLoop reaching into state->tickLoop directly — deliberately
    // decoupling NetworkClient from TickLoop entirely, the same reason
    // GUIController.cpp forward-declares SendChatMessage rather than
    // including NetworkClient.hpp: NetDiag (tools/netdiag_main.cpp) links
    // this same RunPlayLoop without ever compiling TickLoop.cpp, since it's
    // a headless network-only diagnostic with no Player/World/TickLoop at
    // all. Returns whether the player is grounded as of that Tick() call
    // (TickLoop::IsGrounded(), read right after Tick() on the same thread —
    // see its own comment) — RunPlayLoop's periodic position report needs
    // this for MovementFlags.onGround; folded into this callback rather
    // than a second one, since both are one Tick()-adjacent fact read from
    // the same place. The same goes for the rest of TickReport (wall
    // collision, sprinting, held movement keys) — all read off TickLoop
    // right after Tick() returns. NetworkThread wires this up before calling
    // RunSession(); NetDiag never sets it, so it just stays the default
    // no-op (position reports go out with onGround=false, same as before
    // this existed — NetDiag never runs RunPlayLoop's position report at
    // all, see its own null check on state->player).
    void SetTickCallback(std::function<TickReport()> callback) { tickCallback = std::move(callback); }

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
    std::function<TickReport()> tickCallback; // See SetTickCallback's own comment.

    // What the server was last told about sprinting and held movement keys
    // — ReportMovementState only sends a packet when these change, like
    // vanilla's LocalPlayer.sendIsSprintingIfNeeded/sendInput.
    bool reportedSprinting = false;
    uint8_t reportedInputFlags = 0;

    // Reads Graphics.RenderDistance from state->config for the Client
    // Information packet — see the definition for why it's declared to the
    // server here (along with the rest of Client Information).
    bool RunConfiguration(GlobalState* state);
    void RunPlayLoop(GlobalState* state, std::stop_token stopToken);

    // Reports the player's current position via PlayC2S::SetPlayerPosition.
    // Called both right after the server's initial teleport (to kick off
    // chunk streaming — see RunPlayLoop's own comment) and periodically
    // from RunPlayLoop's own loop thereafter, since the server needs an
    // up-to-date position continuously, not just once, to track fall
    // distance (fall damage) and sprint distance (hunger exhaustion) — see
    // RunPlayLoop's own comment on why this used to be a one-shot call.
    // `onGround` feeds MovementFlags.onGround — get this wrong (e.g.
    // hardcode false) and a server that cross-checks it against the
    // reported Y will treat every tick spent standing still as
    // inconsistent and rubber-band the position back.
    //
    // Sent with rotation (PlayC2S::SetPlayerPositionAndRotation) using the
    // camera's current yaw/pitch, converted to Minecraft's convention — the
    // server aims our knockback hits along it. `horizontalCollision` is
    // MovementFlags bit 1.
    void SendPlayerPosition(GlobalState* state, glm::vec3 position, bool onGround, bool horizontalCollision);

    // Sends PlayerCommand (start/stop_sprinting) and PlayerInput whenever
    // the report differs from what the server was last told. Queued rather
    // than written directly, so they stay ordered after any Attack packet
    // the render thread queued first — a sprint hit has to reach the server
    // before the stop_sprinting it causes, or the server sees a plain hit.
    void ReportMovementState(GlobalState* state, const TickReport& report);
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

// Attacks an entity (PlayC2S::Attack). The server decides for itself
// whether this lands as a plain, critical, sprint-knockback or sweep hit,
// from its own copy of our state (sprinting, falling, held weapon, attack
// cooldown) — which is why RunPlayLoop keeps it current. Callable from any
// thread (queued, like SendChatMessage); in practice called by
// InteractionManager on the render thread, followed by SendSwing.
void SendAttack(GlobalState* state, uint32_t targetEntityId);

// Swings an arm (PlayC2S::SwingArm) — the animation other players see.
// hand: 0 = main hand, 1 = offhand.
void SendSwing(GlobalState* state, int32_t hand);

} // namespace Volcano

#endif
