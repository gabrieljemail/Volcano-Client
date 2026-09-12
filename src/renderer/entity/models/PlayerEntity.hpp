#pragma once
#ifndef PLAYER_ENTITY_MODEL_H
#define PLAYER_ENTITY_MODEL_H

#include <array>
#include <cstdint>
#include <string>
#include "Entity.hpp"

namespace Volcano {

// A remote player (anyone in the world other than this client's own
// Player — see src/Player.hpp for that). Inherits Entity's common fields
// (id, position, yaw, placeholder box/color) and adds the identity fields
// only players carry. Plain data, not polymorphic, so a PlayerEntity still
// slices cleanly into a plain Entity for EntityRenderer's uniform batched
// draw — same reasoning as Entity's own placeholder-visuals comment.
struct PlayerEntity : public Entity {
    // Matches NetworkClient.cpp's UUID representation (raw 16 bytes, not a
    // formatted string) so both sides can compare/copy without reparsing.
    std::array<uint8_t, 16> uuid{};
    std::string username;

    // Raw protocol game mode value (0=survival, 1=creative, 2=adventure,
    // 3=spectator as of vanilla today), left unresolved like Entity::type
    // until the runtime protocol registry (see resources/minecraft-data)
    // is wired up to interpret it per-version.
    uint8_t gameMode = 0;
};

} // namespace Volcano

#endif
