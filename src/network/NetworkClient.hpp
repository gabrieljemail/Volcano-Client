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

// Protocol 776 = Minecraft 26.2 (see resources/minecraft/version.json,
// which is the extracted client/server data for the dev server we target).
constexpr int32_t PROTOCOL_VERSION = 776;

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

private:
    Connection connection;

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

} // namespace Volcano

#endif
