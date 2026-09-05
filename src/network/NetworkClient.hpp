#pragma once
#ifndef VOLCANO_NETWORK_CLIENT_H
#define VOLCANO_NETWORK_CLIENT_H

#include <asio.hpp>
#include <cstdint>
#include <string>
#include "Connection.hpp"

namespace Volcano {

// Protocol 775 = Minecraft 1.21.2/1.21.3 (see the architecture spec).
constexpr int32_t PROTOCOL_VERSION = 775;

class NetworkClient {
public:
    explicit NetworkClient(asio::io_context& ioContext) : connection(ioContext) {}

    // Connects and runs Handshake -> Login Start, then reads and logs the
    // server's first response (Login Success or Disconnect). Returns true
    // on Login Success. Configuration and Play state handling land in a
    // later pass — this only proves the pipe works end to end.
    bool ConnectAndLogin(const std::string& host, uint16_t port, const std::string& username);

private:
    Connection connection;
};

} // namespace Volcano

#endif
