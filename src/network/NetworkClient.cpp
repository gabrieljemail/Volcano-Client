#include "NetworkClient.hpp"
#include "VarInt.hpp"
#include <openssl/evp.h>
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Volcano {

namespace {

// Matches Java's UUID.nameUUIDFromBytes("OfflinePlayer:<name>"), which is
// what vanilla uses to derive an offline-mode account's UUID: MD5 of the
// UTF-8 name bytes, then force the version (3) and variant bits per RFC
// 4122 §4.3. An offline-mode server recomputes/overrides this from the
// submitted username anyway, but the Login Start packet still requires a
// well-formed 16-byte UUID field to be present.
std::array<uint8_t, 16> OfflineUuidFromUsername(const std::string& username)
{
    std::string input = "OfflinePlayer:" + username;

    std::array<uint8_t, 16> digest{};
    unsigned int digestLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, input.data(), input.size());
    EVP_DigestFinal_ex(ctx, digest.data(), &digestLen);
    EVP_MD_CTX_free(ctx);

    digest[6] = (digest[6] & 0x0Fu) | 0x30u; // version 3
    digest[8] = (digest[8] & 0x3Fu) | 0x80u; // RFC 4122 variant
    return digest;
}

std::string FormatUuid(const std::array<uint8_t, 16>& uuid)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < uuid.size(); i++) {
        oss << std::setw(2) << static_cast<int>(uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) oss << '-';
    }
    return oss.str();
}

} // namespace

bool NetworkClient::ConnectAndLogin(const std::string& host, uint16_t port, const std::string& username)
{
    try {
        std::cout << "[NET] Connecting to " << host << ":" << port << "..." << std::endl;
        connection.Connect(host, port);

        // Handshake (-> Login state).
        PacketWriter handshake;
        handshake.WriteVarInt(PROTOCOL_VERSION);
        handshake.WriteString(host);
        handshake.WriteUShortBE(port);
        handshake.WriteVarInt(2); // next state: Login
        connection.SendPacket(0x00, handshake.Data());

        // Login Start.
        auto uuid = OfflineUuidFromUsername(username);
        PacketWriter loginStart;
        loginStart.WriteString(username);
        loginStart.WriteBytes(uuid.data(), uuid.size());
        connection.SendPacket(0x00, loginStart.Data());

        std::cout << "[NET] Sent Handshake + Login Start as '" << username
                  << "' (offline UUID " << FormatUuid(uuid) << "). Waiting for response..." << std::endl;

        // Set Compression (0x03) can arrive before Login Success once the
        // server has compression enabled (the common case) — handle it and
        // keep reading until we hit an actual Login Success/Disconnect.
        for (;;) {
            std::vector<uint8_t> payload;
            int32_t packetId = connection.ReadPacket(payload);
            PacketReader reader(payload.data(), payload.size());

            if (packetId == 0x03) {
                int32_t compressionThreshold = reader.ReadVarInt();
                connection.EnableCompression(compressionThreshold);
                std::cout << "[NET] Set Compression: threshold " << compressionThreshold << std::endl;
                continue;
            }

            if (packetId == 0x02) {
                std::array<uint8_t, 16> serverUuid{};
                reader.ReadBytes(serverUuid.data(), serverUuid.size());
                std::string serverUsername = reader.ReadString();
                std::cout << "[NET] Login Success: " << serverUsername
                          << " (" << FormatUuid(serverUuid) << ")" << std::endl;
                std::cout << "[NET] Configuration/Play state handling comes next." << std::endl;
                return true;
            }

            if (packetId == 0x00) {
                std::string reason = reader.ReadString();
                std::cerr << "[NET] Server disconnected during login: " << reason << std::endl;
                return false;
            }

            std::cerr << "[NET] Unexpected packet ID 0x" << std::hex << packetId << std::dec
                      << " while waiting for Login Success (likely an Encryption "
                      << "Request packet — not handled yet)." << std::endl;
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "[NET] Connection failed: " << e.what() << std::endl;
        return false;
    }
}

} // namespace Volcano
