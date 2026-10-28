#pragma once
#ifndef VOLCANO_CONNECTION_H
#define VOLCANO_CONNECTION_H

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Volcano {

// One raw TCP connection to a Minecraft server: connect, and read/write
// whole packets framed the way the protocol expects (VarInt length prefix
// around a VarInt packet ID + payload bytes). No protocol/state-machine
// knowledge lives here
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
    //
    // NETWORK THREAD ONLY. This writes to the socket synchronously and
    // holds no lock; anything on another thread must go through
    // QueuePacket() below instead.
    //
    // It used to be callable from anywhere, guarded by a write mutex, on the
    // reasoning that a blocking socket's send() and recv() are independent
    // directions at the OS level so only writers needed to exclude each
    // other. That stopped being true once WaitForReadable() added
    // socket.async_wait() and socket.cancel() on the network thread: asio
    // documents basic_stream_socket as "Shared objects: Unsafe", and on the
    // Windows IOCP backend cancel() issues CancelIoEx on the handle, which
    // cancels outstanding I/O on that handle regardless of which thread
    // issued it. A chat message sent from the render thread could therefore
    // race the readability timer expiring on the network thread, and the
    // worst case was not a lost message but a PARTIAL write
    void SendPacket(int32_t packetId, const std::vector<uint8_t>& payload);

    // Thread-safe way to send from any thread other than the network one
    // (chat/commands/respawn from the render thread - see
    // NetworkClient::SendChatMessage). Copies the packet onto an outbound
    // queue and returns immediately without touching the socket; the network
    // thread drains it via FlushOutbound() below, so every actual write
    // still happens on the one thread that owns this socket.
    //
    // Fire-and-forget: a queued packet goes out within one RunPlayLoop
    // iteration (~15ms), or is dropped silently if the session ends first.
    void QueuePacket(int32_t packetId, std::vector<uint8_t> payload);

    // NETWORK THREAD ONLY. Writes out everything QueuePacket() has
    // accumulated since the last call, in order. Called once per RunPlayLoop
    // iteration. Throws whatever SendPacket throws (a dead socket)
    void FlushOutbound();

    // Blocks for one full packet. Returns its packet ID; `outPayload` is set
    // to everything after the packet-ID VarInt.
    int32_t ReadPacket(std::vector<uint8_t>& outPayload);

    // Blocks until either the socket has data ready to read or timeout
    // elapses - without consuming any bytes, so ReadPacket() above still
    // does the actual reading exactly as before. Lets the Play loop (see
    // NetworkClient::RunPlayLoop) interleave TickLoop::Tick() between
    // packets on a bounded cadence instead of sitting inside ReadPacket()'s
    // indefinite blocking read for however long the server goes quiet
    // between packets (idle time between Keep Alives can be many seconds).
    // Returns true once data is ready (call ReadPacket() next); false if
    // timeout elapsed with nothing arriving.
    bool WaitForReadable(std::chrono::milliseconds timeout);

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

    // Packets pushed by other threads (QueuePacket) and drained by the
    // network thread (FlushOutbound). The mutex guards only this vector -
    // never a socket operation
    std::mutex outboundMutex;
    std::vector<std::pair<int32_t, std::vector<uint8_t>>> outbound;

    uint8_t ReadByteBlocking();
    void SendUncompressed(int32_t packetId, const std::vector<uint8_t>& payload);
    void SendCompressed(int32_t packetId, const std::vector<uint8_t>& payload);
    int32_t ReadPacketRaw(std::vector<uint8_t>& outPayload);      // no compression framing
    int32_t ReadPacketCompressed(std::vector<uint8_t>& outPayload);

    // Appends one line ("C2S"/"S2C", packet id, payload hex) to
    // settings.json's Network.LogPackets file
    void LogPacket(const char* direction, int32_t packetId, const uint8_t* data, size_t size);
    bool packetLoggingChecked = false;
    std::optional<std::ofstream> packetLogFile;

    // t=0 for every line's leading timestamp
    std::chrono::steady_clock::time_point packetLogStartTime;
};

} // namespace Volcano

#endif
