#pragma once
#ifndef VOLCANO_VARINT_H
#define VOLCANO_VARINT_H

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Volcano {

// Minecraft protocol primitive codec. VarInt is the core piece (7 payload
// bits per byte, MSB = continuation), plus the small set of primitives that
// are directly built on top of it (VarInt-prefixed strings, raw bytes) and
// that essentially every packet needs — kept together rather than split
// into more one-liner headers.

class PacketWriter {
public:
    void WriteVarInt(int32_t value)
    {
        uint32_t v = static_cast<uint32_t>(value);
        do {
            uint8_t byte = v & 0x7Fu;
            v >>= 7;
            if (v != 0) byte |= 0x80u;
            bytes.push_back(byte);
        } while (v != 0);
    }

    void WriteString(const std::string& str)
    {
        WriteVarInt(static_cast<int32_t>(str.size()));
        bytes.insert(bytes.end(), str.begin(), str.end());
    }

    // Server port in Handshake is a raw big-endian unsigned short, not a VarInt.
    void WriteUShortBE(uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    }

    void WriteBytes(const uint8_t* src, size_t count)
    {
        bytes.insert(bytes.end(), src, src + count);
    }

    const std::vector<uint8_t>& Data() const { return bytes; }

private:
    std::vector<uint8_t> bytes;
};

class PacketReader {
public:
    PacketReader(const uint8_t* data, size_t size) : data(data), size(size) {}

    int32_t ReadVarInt()
    {
        int32_t result = 0;
        int shift = 0;
        while (true) {
            uint8_t byte = ReadByte();
            result |= static_cast<int32_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0) break;
            shift += 7;
            if (shift >= 35) throw std::runtime_error("VarInt is too big");
        }
        return result;
    }

    std::string ReadString()
    {
        int32_t length = ReadVarInt();
        if (length < 0 || static_cast<size_t>(length) > Remaining())
            throw std::runtime_error("Malformed string length in packet");
        std::string str(reinterpret_cast<const char*>(data + pos), static_cast<size_t>(length));
        pos += static_cast<size_t>(length);
        return str;
    }

    uint8_t ReadByte()
    {
        if (pos >= size) throw std::runtime_error("Unexpected end of packet");
        return data[pos++];
    }

    void ReadBytes(uint8_t* dst, size_t count)
    {
        if (count > Remaining()) throw std::runtime_error("Unexpected end of packet");
        std::memcpy(dst, data + pos, count);
        pos += count;
    }

    size_t Remaining() const { return size - pos; }

private:
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
};

} // namespace Volcano

#endif
