#include <chrono>
#include <vector>
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include "ServerPing.hpp"
#include "Connection.hpp"
#include "NetworkClient.hpp"
#include "VarInt.hpp"

namespace Volcano {

namespace {

using nlohmann::json;

// Flattens a description: a bare string, or {"text", "extra"} (and arrays of those).
void AppendText(const json& node, std::string& out)
{
    if (node.is_string())
    {
        out += node.get<std::string>();
    }
    else if (node.is_array())
    {
        for (const json& part : node) AppendText(part, out);
    }
    else if (node.is_object())
    {
        if (node.contains("text")) AppendText(node["text"], out);
        if (node.contains("extra")) AppendText(node["extra"], out);
    }
}

// Drops the legacy section-sign color codes ("§a") and everything after the first newline.
std::string CleanMotd(const std::string& raw)
{
    std::string text;
    for (size_t i = 0; i < raw.size(); i++)
    {
        if (raw[i] == '\n') break;
        if (static_cast<unsigned char>(raw[i]) == 0xC2 && i + 2 < raw.size() && static_cast<unsigned char>(raw[i + 1]) == 0xA7)
        {
            i += 2;
            continue;
        }
        text.push_back(raw[i]);
    }
    return text;
}

// Decodes the "data:image/png;base64,..." favicon into RGBA8. Leaves the icon empty if it is missing or bad.
void DecodeIcon(const std::string& favicon, ServerStatus& status)
{
    const size_t comma = favicon.find(',');
    if (comma == std::string::npos) return;

    std::vector<uint8_t> bytes;
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = comma + 1; i < favicon.size(); i++)
    {
        const char c = favicon[i];
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        else continue; // Padding and line breaks.

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            bytes.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFF));
        }
    }

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) return;
    status.icon.assign(pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    status.iconWidth = static_cast<uint32_t>(width);
    status.iconHeight = static_cast<uint32_t>(height);
    stbi_image_free(pixels);
}

} // namespace

void SplitAddress(const std::string& address, std::string& host, uint16_t& port)
{
    host = address;
    port = 25565;
    size_t colon = address.rfind(':');
    if (colon == std::string::npos || address.find(':') != colon) return; // No port, or a bare IPv6.
    try
    {
        int value = std::stoi(address.substr(colon + 1));
        if (value > 0 && value < 65536)
        {
            host = address.substr(0, colon);
            port = static_cast<uint16_t>(value);
        }
    }
    catch (...)
    {
    }
}

ServerStatus PingServer(const std::string& address, int timeoutMs)
{
    ServerStatus status;
    try
    {
        std::string host;
        uint16_t port;
        SplitAddress(address, host, port);

        asio::io_context io;
        Connection connection(io);
        connection.Connect(host, port);

        PacketWriter handshake;
        handshake.WriteVarInt(PROTOCOL_VERSION);
        handshake.WriteString(host);
        handshake.WriteUShortBE(port);
        handshake.WriteVarInt(1); // next state: Status
        connection.SendPacket(0x00, handshake.Data());
        connection.SendPacket(0x00, {}); // Status Request.

        const auto timeout = std::chrono::milliseconds(timeoutMs);
        if (!connection.WaitForReadable(timeout))
        {
            status.error = "Timed out";
            return status;
        }

        std::vector<uint8_t> payload;
        if (connection.ReadPacket(payload) != 0x00)
        {
            status.error = "Bad reply";
            return status;
        }
        PacketReader reader(payload.data(), payload.size());
        json root = json::parse(reader.ReadString());

        if (root.contains("description")) AppendText(root["description"], status.motd);
        status.motd = CleanMotd(status.motd);
        if (root.contains("favicon") && root["favicon"].is_string()) DecodeIcon(root["favicon"].get<std::string>(), status);
        if (root.contains("version")) status.version = root["version"].value("name", std::string());
        if (root.contains("players"))
        {
            status.playersOnline = root["players"].value("online", 0);
            status.playersMax = root["players"].value("max", 0);
        }

        // Ping: the server echoes the same number back.
        const auto sent = std::chrono::steady_clock::now();
        PacketWriter ping;
        ping.WriteLong(1);
        connection.SendPacket(0x01, ping.Data());
        if (connection.WaitForReadable(timeout) && connection.ReadPacket(payload) == 0x01)
        {
            status.latencyMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - sent).count());
        }
        status.online = true;
        connection.Close();
    }
    catch (const std::exception& e)
    {
        status.online = false;
        status.error = e.what();
    }
    return status;
}

} // namespace Volcano
