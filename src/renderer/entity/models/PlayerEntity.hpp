#pragma once
#ifndef PLAYER_ENTITY_MODEL_H
#define PLAYER_ENTITY_MODEL_H

#include <array>
#include <cstdint>
#include <string>
#include "Entity.hpp"
#include "../../../inventory/ItemStack.hpp"

namespace Volcano {

// Protocol game mode values, as sent in Player Info Update's
// UPDATE_GAME_MODE action (and Login/Respawn's SpawnInfo).
enum class GameMode : uint8_t {
    Survival = 0,
    Creative = 1,
    Adventure = 2,
    Spectator = 3,
};

// A remote player (anyone in the world other than this client's own
// Player - see src/Player.hpp for that). Inherits Entity's common fields
// (id, position, yaw, placeholder box/color, hurt flash) and adds
// everything the server tells us about another player specifically:
// identity from Spawn Entity + Player Info Update, gear from Set Equipment,
// and health/pose flags from Set Entity Data.
//
// Stored in GlobalState::playerEntities rather than GlobalState::entities
// (which holds plain Entity values - a PlayerEntity inserted there would
// be sliced down to its Entity part and lose all of this). Both maps share
// entitiesMutex; GlobalState::FindEntity/ForEachEntity walk the two
// together for code that only needs the common Entity fields.
struct PlayerEntity : public Entity {
    // Matches NetworkClient.cpp's UUID representation (raw 16 bytes, not a
    // formatted string) so both sides can compare/copy without reparsing.
    std::array<uint8_t, 16> uuid{};
    std::string username;

    GameMode gameMode = GameMode::Survival;

    // Set Entity Data fields. health starts at vanilla's max so a player
    // whose metadata hasn't arrived yet doesn't read as dying.
    float health = 20.0f;
    bool sneaking = false;  // Shared flags bit 0x02 (crouching).
    bool sprinting = false; // Shared flags bit 0x08.
    bool swimming = false;  // Shared flags bit 0x10.
    bool invisible = false; // Shared flags bit 0x20.
    int32_t pose = 0;       // Vanilla Pose ordinal: 0 standing, 3 swimming, 5 crouching.

    // Swim pose per the shared flag or the pose field, whichever arrives.
    bool IsSwimming() const { return swimming || pose == 3; }
    bool IsCrouching() const { return sneaking || pose == 5; }

    // Vanilla refuses to attack a spectator at all (Player.cannotAttack), and
    // the crosshair raycast skips them for the same reason.
    bool IsSpectator() const { return gameMode == GameMode::Spectator; }
};

} // namespace Volcano

#endif
