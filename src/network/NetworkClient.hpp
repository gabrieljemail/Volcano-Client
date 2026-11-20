#pragma once
#ifndef VOLCANO_NETWORK_CLIENT_H
#define VOLCANO_NETWORK_CLIENT_H

#include <asio.hpp>
#include <cstdint>
#include <stop_token>
#include <string>
#include <glm/glm.hpp>
#include "Connection.hpp"
#include "microsoft/MSAccountManager.hpp"
#include "../ApplicationDeps.hpp"
#ifdef VOLCANO_MODULES
import Volcano;
#else
#include "../Application.hpp"
#endif
#include "../interaction/InteractionTypes.hpp"

namespace Volcano {

// Protocol 775 = Minecraft 26.1
constexpr int32_t PROTOCOL_VERSION = 775;

class NetworkClient {
public:
    explicit NetworkClient(asio::io_context& ioContext) : connection(ioContext) {}

    // Connects and runs Handshake -> Login Start, then reads and logs the
    // server's first response (Login Success or Disconnect). Returns true
    // on Login Success.
    //
    // With a `session` the client logs in as that Microsoft account and answers an online-mode server's Encryption Request
    // through `accounts`; without one it logs in offline as `username` and fails if the server wants authentication.
    bool ConnectAndLogin(const std::string& host, uint16_t port, const std::string& username,
        const LoginSession* session = nullptr, MSAccountManager* accounts = nullptr);

    // Runs Configuration state to completion, then pumps the Play state
    // packet loop until disconnected or stopToken is cancelled. Parsed
    // chunks and the server-reported spawn position are handed off via
    // state->networkInbox for the main thread to consume
    void RunSession(Application* state, std::stop_token stopToken);

    // Exposes the underlying Connection so NetworkThread can publish it to
    // Application::activeConnection once the Play session actually starts
    // (see NetworkThread::ThreadEntry)
    Connection& GetConnection() { return connection; }

    // Human-readable reason the session ended (a kick's chat-component
    // text, an exception message, ...) - set alongside every disconnect/
    // error Log::Error() call in ConnectAndLogin/RunSession/RunConfiguration
    // /RunPlayLoop, empty if the session is still live. NetworkThread reads
    // this once ConnectAndLogin/RunSession returns to tell Application why,
    // so the main thread can show it on the connect screen.
    const std::string& GetLastError() const { return lastError; }

private:
    Connection connection;
    std::string lastError;
    std::string loginUsername; // From Login Success; published to Application by RunSession.
    std::string loginTextures; // The "textures" profile property from Login Success, if the server sent one.
    std::string fallbackSkinUrl; // The signed-in account's own skin, used when the server sent no textures property.
    bool fallbackSkinSlim = false;

    // Reads Graphics.RenderDistance from state->config for the Client
    // Information packet
    bool RunConfiguration(Application* state);
    void RunPlayLoop(Application* state, std::stop_token stopToken);

    // Reports the player's current position+rotation via
    // PlayC2S::SetPlayerPositionAndRotation. Called once, right after the
    // server's initial teleport, to kick off chunk streaming on a server
    // that doesn't push chunks unprompted
    void SendPlayerPosition(glm::vec3 position, float yaw, float pitch, bool onGround);
};

// Sends a plain (unsigned) chat message via state->activeConnection - a
// no-op (logged, not thrown) if there's no live session, e.g. chat was
// opened before connecting or the connection just dropped. Callable from
// any thread; see PlayC2S::ChatMessage and Connection::QueuePacket's own
// comments for why this is safe to call from outside the network thread
// (it queues rather than writing the socket directly).
void SendChatMessage(Application* state, const std::string& message);

// Same as SendChatMessage but for a command (no leading "/") - sent via
// PlayC2S::ChatCommand so the server actually executes it through its
// command dispatcher instead of broadcasting it as a literal chat line.
void SendChatCommand(Application* state, const std::string& command);

// Requests respawn after death (PlayC2S::ClientCommand, action 0)
void SendRespawnRequest(Application* state);

// Tells the server which hotbar slot (0-8) is now selected, via
// PlayC2S::SetHeldItem
void SendHeldItemSlot(Application* state, uint8_t slot);

// Reports the local flight state via PlayC2S::PlayerAbilities (flags i8:
// mayFly | flying bits). Called when the double-tap Fly toggle flips.
void SendPlayerAbilities(Application* state, bool flying);

// Reports one predicted player-inventory click and its resulting slot/cursor
// state. `stateId` is the latest revision received from the server.
void SendInventoryClick(Application* state, int32_t windowId, int32_t stateId, int32_t slot,
                        uint8_t button, const ItemStack& slotItem, const ItemStack& carriedItem);

// A non-plain container click, with no predicted result: the server works out
// what moves and answers with the slot updates. `mode` is the protocol's click
// mode: 1 shift-click (button 0/1 = the mouse button), 2 number-key swap
// (button = hotbar slot 0-8), 4 drop (button 0 = one, 1 = the whole stack).
// `slot` is the hovered protocol slot index; `carriedItem` the cursor stack.
void SendInventoryAction(Application* state, int32_t windowId, int32_t stateId, int32_t slot,
                         uint8_t button, int32_t mode, const ItemStack& carriedItem);

// Closes an open server container window. Opening the player's own inventory
// with E is local-only and does not send a packet.
void SendInventoryClose(Application* state, int32_t windowId);

// Interaction packets

// PlayC2S::Attack
void SendAttack(Application* state, uint32_t entityId);

// PlayC2S::UseEntity
void SendInteractEntity(Application* state, uint32_t entityId, Hand hand, bool sneaking);

// PlayC2S::BlockDig
void SendPlayerAction(Application* state, DigStatus status, glm::ivec3 position, glm::ivec3 face);

// PlayC2S::ArmAnimation
void SendArmAnimation(Application* state, Hand hand);

// PlayC2S::BlockPlace
void SendBlockPlace(Application* state, Hand hand, glm::ivec3 position, glm::ivec3 face, glm::vec3 cursor);

// PlayC2S::UseItem
void SendUseItem(Application* state, Hand hand, float yaw, float pitch);

// PlayC2S::PickItemFromBlock
void SendPickItemFromBlock(Application* state, glm::ivec3 position);

// PlayC2S::SetCreativeSlot - writes one player-inventory slot directly in
// creative mode (no window is open). `slot` uses player-inventory menu
// numbering (window 0: main 9-35, hotbar 36-44, armor 5-8, offhand 45);
// -1 drops the stack into the world. ItemStack carries no components, so
// enchanted/damaged variants round-trip as plain items.
void SendSetCreativeSlot(Application* state, int16_t slot, const ItemStack& item);

// Swings an arm (PlayC2S::SwingArm - an alias of ArmAnimation, see
// PacketIds.hpp) - the animation other players see. hand: 0 = main hand,
// 1 = offhand. Same wire packet as SendArmAnimation above; kept as its own
// function since AttackEntity's callers pass a raw hand index rather than
// the Hand enum.
void SendSwing(Application* state, int32_t hand);

} // namespace Volcano

#endif
