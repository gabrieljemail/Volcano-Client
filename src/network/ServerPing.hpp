#pragma once
#ifndef VOLCANO_SERVER_PING_H
#define VOLCANO_SERVER_PING_H

#include <cstdint>
#include <string>
#include <vector>

namespace Volcano {

struct ServerStatus {
    bool online = false;
    std::string motd;     // Plain text, first line only.
    std::string version;  // The server's version name, e.g. "26.1".
    int playersOnline = 0;
    int playersMax = 0;
    int latencyMs = 0;
    std::string error;    // Why it is not online.
    std::vector<uint8_t> icon; // The server's icon as RGBA8 (usually 64x64), empty if it sent none.
    uint32_t iconWidth = 0;
    uint32_t iconHeight = 0;
};

// Splits "host" or "host:port" (default 25565).
void SplitAddress(const std::string& address, std::string& host, uint16_t& port);

// Server List Ping: handshake into the Status state, request, read the status
// JSON, then a ping/pong for latency. Blocks until done or `timeoutMs` passes
// waiting on a reply, so call it from a worker thread. Never throws.
ServerStatus PingServer(const std::string& address, int timeoutMs = 4000);

} // namespace Volcano

#endif
