#pragma once
#ifndef VOLCANO_PLAYER_ATTRIBUTES_H
#define VOLCANO_PLAYER_ATTRIBUTES_H

#include <mutex>
#include <string>
#include "network/NBT.hpp"

namespace Volcano {

// Backed by a real NBT::TagCompound (same field names Minecraft itself uses,
// e.g. "generic.movement_speed"), but populated with hardcoded defaults for
// now — there's no player-data file or server attribute sync to read from
// yet. Same "slot" pattern as BiomeColors.hpp: once real NBT data is
// available (a parsed player.dat, or a server attributes packet), it plugs
// in here without changing how TickLoop reads attributes.
//
// Server values now do arrive: NetworkClient's PlayS2C::UpdateAttributes
// handler writes the local player's resolved attributes (attack speed from
// the held weapon, reach, movement speed, ...) here from NetworkThread,
// while TickLoop (NetworkThread) and InteractionManager (render thread)
// read them — so every access goes through `mutex`.
class PlayerAttributes {
public:
    PlayerAttributes() = default;
    // The mutex itself can't move; only the data does. Only Defaults()'s
    // return-by-value needs this, before anything else can see the object.
    PlayerAttributes(PlayerAttributes&& other) noexcept : attributes(std::move(other.attributes)) {}

    static PlayerAttributes Defaults();

    double GetDouble(const std::string& name, double fallback) const;
    void SetDouble(const std::string& name, double value);

private:
    mutable std::mutex mutex;
    NBT::TagCompound attributes;
};

} // namespace Volcano

#endif
