#pragma once
#ifndef VOLCANO_PLAYER_ATTRIBUTES_H
#define VOLCANO_PLAYER_ATTRIBUTES_H

#include <string>
#include "network/NBT.hpp"

namespace Volcano {

// Backed by a real NBT::TagCompound (same field names Minecraft itself uses,
// e.g. "generic.movement_speed"), but populated with hardcoded defaults for
// now — there's no player-data file or server attribute sync to read from
// yet. Same "slot" pattern as BiomeColors.hpp: once real NBT data is
// available (a parsed player.dat, or a server attributes packet), it plugs
// in here without changing how TickLoop reads attributes.
class PlayerAttributes {
public:
    static PlayerAttributes Defaults();

    double GetDouble(const std::string& name, double fallback) const;
    void SetDouble(const std::string& name, double value);

private:
    NBT::TagCompound attributes;
};

} // namespace Volcano

#endif
