#include "Connection.hpp"
#include "VarInt.hpp"
#include <zlib.h>
#include <stdexcept>

namespace Volcano {

namespace {

// Vanilla's own MAX_PACKET_SIZE, applied to both the wire frame length and a
// compressed packet's declared uncompressed size. Both numbers come straight
// off the socket before a single byte of them has been validated, and both
// are fed directly to a vector's sizing constructor — so without this, one
// desynced byte anywhere in the stream (exactly what the Set Container Slot
// VarInt-vs-i16 misread produced before it was fixed) turns the next "length"
// into garbage and asks for an allocation of up to 2 GiB. That's a hard
// crash or a swap-storm instead of a clean, catchable "malformed packet".
constexpr int32_t MAX_PACKET_SIZE = 2097152;

// Validates a length field read off the wire before it's used to size a
// buffer. Negative is always malformed (a 5-byte VarInt can encode one, and
// static_cast<size_t> of it is SIZE_MAX).
int32_t CheckedLength(int32_t length, const char* what)
{
    if (length < 0 || length > MAX_PACKET_SIZE)
    {
        throw std::runtime_error(std::string("Malformed ") + what + " (" + std::to_string(length)
            + ") — stream is desynced or the server is not speaking this protocol");
    }
    return length;
}

std::vector<uint8_t> ZlibCompress(const std::vector<uint8_t>& data)
{
    uLongf destLen = compressBound(static_cast<uLong>(data.size()));
    std::vector<uint8_t> dest(destLen);
    int result = compress2(dest.data(), &destLen, data.data(), static_cast<uLong>(data.size()), Z_DEFAULT_COMPRESSION);
    if (result != Z_OK) throw std::runtime_error("zlib compress2 failed");
    dest.resize(destLen);
    return dest;
}

std::vector<uint8_t> ZlibDecompress(const uint8_t* data, size_t size, int32_t uncompressedSize)
{
    CheckedLength(uncompressedSize, "compressed packet's declared uncompressed size");
    std::vector<uint8_t> dest(static_cast<size_t>(uncompressedSize));
    uLongf destLen = static_cast<uLongf>(uncompressedSize);
    int result = uncompress(dest.data(), &destLen, data, static_cast<uLong>(size));
    if (result != Z_OK) throw std::runtime_error("zlib uncompress failed");
    dest.resize(destLen);
    return dest;
}

} // namespace

void Connection::SendPacket(int32_t packetId, const std::vector<uint8_t>& payload)
{
    if (threshold < 0) SendUncompressed(packetId, payload);
    else SendCompressed(packetId, payload);
}

void Connection::QueuePacket(int32_t packetId, std::vector<uint8_t> payload)
{
    std::lock_guard<std::mutex> lock(outboundMutex);
    outbound.emplace_back(packetId, std::move(payload));
}

void Connection::FlushOutbound()
{
    // Swap the queue out under the lock and write outside it, so a
    // cross-thread QueuePacket() never blocks behind a socket write — and so
    // a throw from SendPacket below can't leave the lock held.
    std::vector<std::pair<int32_t, std::vector<uint8_t>>> pending;
    {
        std::lock_guard<std::mutex> lock(outboundMutex);
        if (outbound.empty()) return;
        pending.swap(outbound);
    }

    for (const auto& [packetId, payload] : pending)
    {
        SendPacket(packetId, payload);
    }
}

void Connection::SendUncompressed(int32_t packetId, const std::vector<uint8_t>& payload)
{
    PacketWriter idAndPayload;
    idAndPayload.WriteVarInt(packetId);
    idAndPayload.WriteBytes(payload.data(), payload.size());

    PacketWriter framed;
    framed.WriteVarInt(static_cast<int32_t>(idAndPayload.Data().size()));
    framed.WriteBytes(idAndPayload.Data().data(), idAndPayload.Data().size());

    asio::write(socket, asio::buffer(framed.Data()));
}

void Connection::SendCompressed(int32_t packetId, const std::vector<uint8_t>& payload)
{
    PacketWriter idAndPayload;
    idAndPayload.WriteVarInt(packetId);
    idAndPayload.WriteBytes(payload.data(), payload.size());
    const auto& uncompressed = idAndPayload.Data();

    PacketWriter body;
    if (static_cast<int32_t>(uncompressed.size()) >= threshold)
    {
        auto compressed = ZlibCompress(uncompressed);
        body.WriteVarInt(static_cast<int32_t>(uncompressed.size())); // dataLength = uncompressed size
        body.WriteBytes(compressed.data(), compressed.size());
    }
    else
    {
        body.WriteVarInt(0); // dataLength == 0 means "not compressed"
        body.WriteBytes(uncompressed.data(), uncompressed.size());
    }

    PacketWriter framed;
    framed.WriteVarInt(static_cast<int32_t>(body.Data().size()));
    framed.WriteBytes(body.Data().data(), body.Data().size());

    asio::write(socket, asio::buffer(framed.Data()));
}

uint8_t Connection::ReadByteBlocking()
{
    uint8_t byte;
    asio::read(socket, asio::buffer(&byte, 1));
    return byte;
}

int32_t Connection::ReadPacket(std::vector<uint8_t>& outPayload)
{
    return threshold < 0 ? ReadPacketRaw(outPayload) : ReadPacketCompressed(outPayload);
}

bool Connection::WaitForReadable(std::chrono::milliseconds timeout)
{
    auto& ctx = static_cast<asio::io_context&>(socket.get_executor().context());
    asio::steady_timer timer(ctx);
    timer.expires_after(timeout);

    bool dataReady = false;
    // Both handlers below are guaranteed to fire exactly once each (asio
    // completes every async op, successfully or with operation_aborted) —
    // waiting for both before returning means neither is left posted to
    // this io_context with a dangling reference into this stack frame,
    // which a later WaitForReadable() call's ctx.run_one() could otherwise
    // service after these locals no longer exist.
    int pending = 2;

    socket.async_wait(asio::socket_base::wait_read, [&](const asio::error_code& ec) {
        if (!ec) dataReady = true;
        timer.cancel();
        --pending;
    });
    timer.async_wait([&](const asio::error_code&) {
        socket.cancel();
        --pending;
    });

    // This io_context is otherwise never run (see the class comment) —
    // only this thread ever touches this socket, so driving it with
    // run_one() here just to service the two handlers above is safe.
    ctx.restart();
    while (pending > 0)
    {
        ctx.run_one();
    }

    return dataReady;
}

int32_t Connection::ReadPacketRaw(std::vector<uint8_t>& outPayload)
{
    // The length-prefix VarInt precedes everything else in the frame, so it
    // has to be read one byte at a time straight off the socket — there's
    // nothing to size a bulk read against yet.
    int32_t length = 0;
    int shift = 0;
    while (true) {
        uint8_t byte = ReadByteBlocking();
        length |= static_cast<int32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
        if (shift >= 35) throw std::runtime_error("VarInt is too big reading packet length");
    }

    std::vector<uint8_t> body(static_cast<size_t>(CheckedLength(length, "packet length")));
    asio::read(socket, asio::buffer(body));

    PacketReader reader(body.data(), body.size());
    int32_t packetId = reader.ReadVarInt();
    outPayload.assign(body.data() + (body.size() - reader.Remaining()), body.data() + body.size());
    return packetId;
}

int32_t Connection::ReadPacketCompressed(std::vector<uint8_t>& outPayload)
{
    int32_t packetLength = 0;
    int shift = 0;
    while (true) {
        uint8_t byte = ReadByteBlocking();
        packetLength |= static_cast<int32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) break;
        shift += 7;
        if (shift >= 35) throw std::runtime_error("VarInt is too big reading packet length");
    }

    std::vector<uint8_t> body(static_cast<size_t>(CheckedLength(packetLength, "packet length")));
    asio::read(socket, asio::buffer(body));

    PacketReader outer(body.data(), body.size());
    int32_t dataLength = outer.ReadVarInt();
    const uint8_t* rest = body.data() + (body.size() - outer.Remaining());
    size_t restSize = outer.Remaining();

    std::vector<uint8_t> idAndPayload;
    if (dataLength == 0)
    {
        idAndPayload.assign(rest, rest + restSize);
    }
    else
    {
        idAndPayload = ZlibDecompress(rest, restSize, dataLength);
    }

    PacketReader inner(idAndPayload.data(), idAndPayload.size());
    int32_t packetId = inner.ReadVarInt();
    outPayload.assign(idAndPayload.data() + (idAndPayload.size() - inner.Remaining()),
                       idAndPayload.data() + idAndPayload.size());
    return packetId;
}

} // namespace Volcano
