#pragma once
#ifndef ENTITY_MODEL_H
#define ENTITY_MODEL_H

#include <array>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include "../../../inventory/ItemStack.hpp"
#include "../../../Math.hpp"

namespace Volcano {

// How long a render-time interpolation blend from previous*/lastUpdateTime
// to the current authoritative position/yaw runs
constexpr float ENTITY_INTERP_SECONDS = 3.0f / 20.0f;

// How long a server position correction on a simulated entity fades out over.
constexpr float CORRECTION_SECONDS = 3.0f / 20.0f;

// A remote entity (mob, other player, item frame, etc.) tracked for
// rendering. Populated by NetworkClient's Play packet loop (Spawn Entity /
// Move Entity Pos(Rot) / Entity Teleport / Remove Entities
enum class EquipmentSlot : uint8_t {
    MainHand = 0,
    OffHand = 1,
    Feet = 2,
    Legs = 3,
    Chest = 4,
    Head = 5,
    Body = 6,   // Horse/wolf armor.
    Saddle = 7,
};
constexpr size_t EQUIPMENT_SLOT_COUNT = 8;

struct Entity {
    uint32_t id = 0;   // Network entity ID.
    uint16_t type = 0; // Raw network entity-type ID (an index into EntityRegistry/minecraft-data's entities.json).

    // Authoritative state, as of the most recent packet
    glm::vec3 position{0.0f}; // World position, feet/base (Minecraft's convention).
    // Held items and worn armor, indexed by EquipmentSlot. Only id/count are
    // tracked - see ItemStack's own comment on data components.
    std::array<ItemStack, EQUIPMENT_SLOT_COUNT> equipment{};
    const ItemStack& GetEquipment(EquipmentSlot slot) const { return equipment[static_cast<size_t>(slot)]; }

    float yaw = 0.0f;         // Radians around world +Y. Pitch/head rotation aren't
                              // modeled yet since the placeholder box below is
                              // rotationally symmetric about them.

    // The authoritative state just before the update above landed, plus when
    // that update arrived
    glm::vec3 previousPosition{0.0f};
    float previousYaw = 0.0f;

    // Blocks per tick, from the spawn / motion / position-sync packets. Only
    // dropped items use it (TickLoop::StepDroppedItems): the server updates
    // them about once a second, vanilla clients simulate the fall in between.
    glm::vec3 velocity{0.0f};

    // Entities the client flies itself (see EntityMotion.hpp). position is
    // then the simulated one; the packets' own position lives in
    // serverPosition, and when it disagrees the difference fades out as
    // correction over CORRECTION_SECONDS instead of snapping.
    bool simulated = false;
    glm::vec3 serverPosition{0.0f};
    glm::vec3 correction{0.0f};
    std::chrono::steady_clock::time_point correctionTime{};
    float interpSeconds = 3.0f / 20.0f; // Blend window from previousPosition to position.
    std::chrono::steady_clock::time_point lastUpdateTime{};

    // Placeholder visuals until real per-type models/skins exist: a
    // Minecraft-player-sized box tinted a flat color. Sized from
    // EntityRegistry's per-type width/height at spawn time (see
    // NetworkClient) - a real guess at the entity's actual footprint,
    // rather than always assuming player size.
    glm::vec2 boundingBox{0.6f, 1.8f}; // width, height.
    glm::vec3 color{0.8f, 0.2f, 0.2f};

    // Protocol block-state id for block-shaped entities (falling_block's
    // spawn objectData, tnt's fixed block). -1 for everything else.
    int32_t blockStateId = -1;

    // Protocol item id of a dropped item's stack (Set Entity Data index 8).
    // 0 until the metadata arrives, and for every non-item entity.
    int32_t itemId = 0;

    // Set by the Damage Event packet. The hit flash lasts vanilla's 10-tick
    // hurtTime
    std::chrono::steady_clock::time_point lastHurtTime{};
    bool visible = true;

    // Set once the server reports this entity died (EntityEvent status 3).
    // It lingers until Remove Entities arrives, tinted red the whole time,
    // the same way vanilla keeps a dying mob/player tinted through its death
    // animation.
    bool dead = false;

    // When this entity last swung an arm, and when a critical hit last
    // landed on it (PlayS2C::Animate) - recorded for an arm-swing animation
    // and crit particles, neither of which is drawn yet.
    std::chrono::steady_clock::time_point lastSwingTime{};
    std::chrono::steady_clock::time_point lastCritTime{};

    // Riding links from Set Passengers, guarded by entitiesMutex. vehicleId is
    // the entity this one sits on (-1 for none). passengers lists this
    // entity's riders in seat order; it can hold the local player's id, who
    // has no Entity of their own (see GlobalState::ridingEntityId).
    int32_t vehicleId = -1;
    std::vector<uint32_t> passengers;

    // Vanilla movement_speed and jump_strength from Update Attributes, what a
    // mount steers and jumps with (RideController). 0 until the packet arrives.
    float movementSpeed = 0.0f;
    float jumpStrength = 0.0f;

    // Set by RideController while it steers this vehicle; position packets
    // for it are ignored then, as vanilla's isControlledByLocalInstance does.
    bool locallyControlled = false;

    bool IsPassenger() const { return vehicleId >= 0; }

    bool IsHurt(std::chrono::steady_clock::time_point now) const
    {
        return dead
            || (lastHurtTime != std::chrono::steady_clock::time_point{}
                && now - lastHurtTime < std::chrono::milliseconds(500));
    }

    // Render-time position, blending previousPosition -> position over
    // ENTITY_INTERP_SECONDS starting at lastUpdateTime. Clamped to
    // position once that window has elapsed (the common case: entities
    // update roughly once per server tick, i.e. faster than the window).
    glm::vec3 InterpolatedPosition(std::chrono::steady_clock::time_point now) const
    {
        float alpha = InterpolationAlpha(now);
        glm::vec3 result = Math::InterpolatePosition(previousPosition, position, alpha);
        if (correction != glm::vec3(0.0f))
        {
            float elapsed = std::chrono::duration<float>(now - correctionTime).count();
            result += correction * std::clamp(1.0f - elapsed / CORRECTION_SECONDS, 0.0f, 1.0f);
        }
        return result;
    }

    // Same blend for yaw, taking the shortest way around the circle so an
    // update that crosses the +-pi wraparound doesn't spin the long way.
    float InterpolatedYaw(std::chrono::steady_clock::time_point now) const
    {
        float alpha = InterpolationAlpha(now);
        return Math::InterpolateAngle(previousYaw, yaw, alpha);
    }

private:
    float InterpolationAlpha(std::chrono::steady_clock::time_point now) const
    {
        float elapsed = std::chrono::duration<float>(now - lastUpdateTime).count();
        return std::clamp(elapsed / interpSeconds, 0.0f, 1.0f);
    }
};

} // namespace Volcano

#endif
