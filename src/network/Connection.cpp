#include "Connection.hpp"
#include "VarInt.hpp"
#include <zlib.h>
#include <stdexcept>

namespace Volcano {

namespace {

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
    std::lock_guard<std::mutex> lock(writeMutex);
    if (threshold < 0) SendUncompressed(packetId, payload);
    else SendCompressed(packetId, payload);
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

    std::vector<uint8_t> body(static_cast<size_t>(length));
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

    std::vector<uint8_t> body(static_cast<size_t>(packetLength));
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
