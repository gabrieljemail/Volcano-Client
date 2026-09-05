#pragma once
#ifndef VOLCANO_CONNECTION_H
#define VOLCANO_CONNECTION_H

#include <asio.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace Volcano {

// One raw TCP connection to a Minecraft server: connect, and read/write
// whole packets framed the way the protocol expects (VarInt length prefix
// around a VarInt packet ID + payload bytes). No protocol/state-machine
// knowledge lives here — see NetworkClient for that.
//
// Synchronous for now: the only user right now (NetworkClient's
// Handshake->Login sequence) is a short, one-shot exchange. A continuous
// async receive loop — needed once we're pumping Play-state packets
// indefinitely — is a separate, later step.
class Connection {
public:
    explicit Connection(asio::io_context& ioContext) : socket(ioContext) {}

    void Connect(const std::string& host, uint16_t port)
    {
        asio::ip::tcp::resolver resolver(socket.get_executor());
        auto endpoints = resolver.resolve(host, std::to_string(port));
        asio::connect(socket, endpoints);
        socket.set_option(asio::ip::tcp::no_delay(true));
    }

    // Sends [VarInt totalLength][VarInt packetId][payload bytes], or the
    // compressed framing below once EnableCompression() has been called
    // (triggered by the server's Set Compression packet).
    void SendPacket(int32_t packetId, const std::vector<uint8_t>& payload);

    // Blocks for one full packet. Returns its packet ID; `outPayload` is set
    // to everything after the packet-ID VarInt.
    int32_t ReadPacket(std::vector<uint8_t>& outPayload);

    // Called once, after receiving the server's Set Compression packet.
    // From this point on, every packet in both directions is framed as
    // [VarInt packetLength][VarInt dataLength][data], where `data` is
    // zlib-compressed (packetId+payload) if the uncompressed size was >=
    // threshold, or the raw uncompressed bytes (with dataLength == 0)
    // otherwise.
    void EnableCompression(int32_t thresholdIn) { threshold = thresholdIn; }

    void Close()
    {
        asio::error_code ec;
        socket.close(ec);
    }

private:
    asio::ip::tcp::socket socket;
    int32_t threshold = -1; // -1 == compression not yet enabled

    uint8_t ReadByteBlocking();
    void SendUncompressed(int32_t packetId, const std::vector<uint8_t>& payload);
    void SendCompressed(int32_t packetId, const std::vector<uint8_t>& payload);
    int32_t ReadPacketRaw(std::vector<uint8_t>& outPayload);      // no compression framing
    int32_t ReadPacketCompressed(std::vector<uint8_t>& outPayload);
};

} // namespace Volcano

#endif
