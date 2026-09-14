#pragma once
#ifndef VOLCANO_NETWORK_CLIENT_H
#define VOLCANO_NETWORK_CLIENT_H

#include <asio.hpp>
#include <cstdint>
#include <stop_token>
#include <string>
#include "Connection.hpp"
#include "../GlobalState.hpp"

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
};

// Sends a plain (unsigned) chat message via state->activeConnection — a
// no-op (logged, not thrown) if there's no live session, e.g. chat was
// opened before connecting or the connection just dropped. Callable from
// any thread; see PlayC2S::ChatMessage and Connection::SendPacket's own
// comments for why this is safe to call from outside the network thread.
void SendChatMessage(GlobalState* state, const std::string& message);

// Same as SendChatMessage but for a command (no leading "/") — sent via
// PlayC2S::ChatCommand so the server actually executes it through its
// command dispatcher instead of broadcasting it as a literal chat line.
void SendChatCommand(GlobalState* state, const std::string& command);

// Requests respawn after death (PlayC2S::ClientCommand, action 0) — the
// death screen's Respawn button.
void SendRespawnRequest(GlobalState* state);

} // namespace Volcano

#endif
