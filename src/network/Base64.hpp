#pragma once
#ifndef VOLCANO_NETWORK_BASE64_H
#define VOLCANO_NETWORK_BASE64_H

#include <cstdint>
#include <string>
#include <vector>

namespace Volcano {

// Decodes standard or URL-safe base64, padded or not. Characters outside the alphabet are skipped.
inline std::vector<uint8_t> Base64Decode(const std::string& text)
{
    std::vector<uint8_t> out;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text)
    {
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+' || c == '-') value = 62;
        else if (c == '/' || c == '_') value = 63;
        else continue;

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buffer >> bits) & 0xFFu));
        }
    }
    return out;
}

} // namespace Volcano

#endif
