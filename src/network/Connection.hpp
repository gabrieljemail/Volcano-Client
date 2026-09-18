#pragma once
#ifndef VOLCANO_CONNECTION_H
#define VOLCANO_CONNECTION_H

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Volcano {

// One raw TCP connection to a Minecraft server: connect, and read/write
// whole packets framed the way the protocol expects (VarInt length prefix
// around a VarInt packet ID + payload bytes). No protocol/state-machine
// knowledge lives here — see NetworkClient for that.
//
// Reads/writes themselves are still synchronous (see ReadPacket/SendPacket)
// — the only asynchronous piece is WaitForReadable below, a pure readiness
// check used to bound how long the Play loop can block with nothing to
// read. A full async_read/async_write rewrite (posting the tick timer
// through the same io_context instead of polling readiness) is a separate,
// later step — see the tick-loop plan's "blocker" section.
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
    // worst case was not a lost message but a PARTIAL write — a half-written
    // frame desyncs the packet stream permanently for the rest of the
    // session. Widening the mutex to cover reads would have fixed it only by
    // blocking every send for up to the full WaitForReadable timeout; the
    // queue below fixes it without that.
    void SendPacket(int32_t packetId, const std::vector<uint8_t>& payload);

    // Thread-safe way to send from any thread other than the network one
    // (chat/commands/respawn from the render thread — see
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
    // iteration. Throws whatever SendPacket throws (a dead socket) — the
    // same failure the loop's own sends would hit on that iteration anyway.
    void FlushOutbound();

    // Blocks for one full packet. Returns its packet ID; `outPayload` is set
    // to everything after the packet-ID VarInt.
    int32_t ReadPacket(std::vector<uint8_t>& outPayload);

    // Blocks until either the socket has data ready to read or timeout
    // elapses — without consuming any bytes, so ReadPacket() above still
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
    // network thread (FlushOutbound). The mutex guards only this vector —
    // never a socket operation — so a cross-thread send never waits on I/O.
    std::mutex outboundMutex;
    std::vector<std::pair<int32_t, std::vector<uint8_t>>> outbound;

    uint8_t ReadByteBlocking();
    void SendUncompressed(int32_t packetId, const std::vector<uint8_t>& payload);
    void SendCompressed(int32_t packetId, const std::vector<uint8_t>& payload);
    int32_t ReadPacketRaw(std::vector<uint8_t>& outPayload);      // no compression framing
    int32_t ReadPacketCompressed(std::vector<uint8_t>& outPayload);
};

} // namespace Volcano

#endif
