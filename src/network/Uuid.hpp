#pragma once
#ifndef VOLCANO_NETWORK_UUID_H
#define VOLCANO_NETWORK_UUID_H

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace Volcano {

// "8-4-4-4-12" lowercase hex, the key the player tables use.
inline std::string FormatUuid(const std::array<uint8_t, 16>& uuid)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < uuid.size(); i++)
    {
        oss << std::setw(2) << static_cast<int>(uuid[i]);
        if (i == 3 || i == 5 || i == 7 || i == 9) oss << '-';
    }
    return oss.str();
}

} // namespace Volcano

#endif
