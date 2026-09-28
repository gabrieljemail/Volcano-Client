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

// Slot indices of PlayerEntity::equipment — vanilla's EquipmentSlot
// ordinals, which is exactly what PlayS2C::SetEquipment sends per entry,
// so a packet's slot byte indexes this array directly.
enum class EquipmentSlot : uint8_t {
    MainHand = 0,
    OffHand = 1,
    Feet = 2,
    Legs = 3,
    Chest = 4,
    Head = 5,
    Body = 6,   // Horse/wolf armor — never set for a player, kept so the indices line up.
    Saddle = 7, // Same.
};
constexpr size_t EQUIPMENT_SLOT_COUNT = 8;

// A remote player (anyone in the world other than this client's own
// Player — see src/Player.hpp for that). Inherits Entity's common fields
// (id, position, yaw, placeholder box/color, hurt flash) and adds
// everything the server tells us about another player specifically:
// identity from Spawn Entity + Player Info Update, gear from Set Equipment,
// and health/pose flags from Set Entity Data.
//
// Stored in GlobalState::playerEntities rather than GlobalState::entities
// (which holds plain Entity values — a PlayerEntity inserted there would
// be sliced down to its Entity part and lose all of this). Both maps share
// entitiesMutex; GlobalState::FindEntity/ForEachEntity walk the two
// together for code that only needs the common Entity fields.
struct PlayerEntity : public Entity {
    // Matches NetworkClient.cpp's UUID representation (raw 16 bytes, not a
    // formatted string) so both sides can compare/copy without reparsing.
    std::array<uint8_t, 16> uuid{};
    std::string username;

    GameMode gameMode = GameMode::Survival;

    // Held items and worn armor, indexed by EquipmentSlot. Only id/count are
    // tracked — see ItemStack's own comment on data components.
    std::array<ItemStack, EQUIPMENT_SLOT_COUNT> equipment{};

    // Set Entity Data fields. health starts at vanilla's max so a player
    // whose metadata hasn't arrived yet doesn't read as dying.
    float health = 20.0f;
    bool sneaking = false;  // Shared flags bit 0x02 (crouching).
    bool sprinting = false; // Shared flags bit 0x08.
    bool invisible = false; // Shared flags bit 0x20.

    const ItemStack& GetEquipment(EquipmentSlot slot) const
    {
        return equipment[static_cast<size_t>(slot)];
    }

    // Vanilla refuses to attack a spectator at all (Player.cannotAttack), and
    // the crosshair raycast skips them for the same reason.
    bool IsSpectator() const { return gameMode == GameMode::Spectator; }
};

} // namespace Volcano

#endif
