#pragma once
#ifndef ENTITY_MODEL_H
#define ENTITY_MODEL_H

#include <cstdint>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace Volcano {

// How long a render-time interpolation blend from previous*/lastUpdateTime
// to the current authoritative position/yaw runs — see InterpolatedPosition/
// InterpolatedYaw. 3 ticks (vanilla's own remote-entity interpolation
// window) smooths out normal 1-tick-per-packet movement jitter without
// feeling laggy; a bigger gap (a teleport, a missed packet) just means the
// entity sits at its target a little before the next update arrives, since
// alpha clamps at 1.
constexpr float ENTITY_INTERP_SECONDS = 3.0f / 20.0f;

// A remote entity (mob, other player, item frame, etc.) tracked for
// rendering. Populated by NetworkClient's Play packet loop (Spawn Entity /
// Move Entity Pos(Rot) / Entity Teleport / Remove Entities — see
// PacketIds.hpp) directly into GlobalState::entities, guarded by
// entitiesMutex the same way renderList is guarded against RenderThread.
//
// Holds only the fields every entity has regardless of category. For the
// category-specific data the network layer will eventually populate, see
// PlayerEntity.hpp (other players), SubjectEntity.hpp (mobs/animals — living
// entities with their own AI/health), and ObjectEntity.hpp (items, boats,
// projectiles — inanimate objects). Each inherits these fields as plain
// data (no virtuals), so any of them still slices into a plain Entity here.
struct Entity {
    uint32_t id = 0;   // Network entity ID.
    uint16_t type = 0; // Raw network entity-type ID (an index into EntityRegistry/minecraft-data's entities.json).

    // Authoritative state, as of the most recent packet — NOT what should be
    // rendered directly; use InterpolatedPosition/InterpolatedYaw for that.
    glm::vec3 position{0.0f}; // World position, feet/base (Minecraft's convention).
    float yaw = 0.0f;         // Radians around world +Y. Pitch/head rotation aren't
                              // modeled yet since the placeholder box below is
                              // rotationally symmetric about them.

    // The authoritative state just before the update above landed, plus when
    // that update arrived — together with position/yaw these are all
    // InterpolatedPosition/InterpolatedYaw need to blend smoothly between
    // network updates instead of snapping an entity from one spot to the
    // next every ~50ms. A freshly spawned entity has these equal to
    // position/yaw (see NetworkClient's Spawn Entity handling), so it
    // doesn't animate in from some earlier undefined state.
    glm::vec3 previousPosition{0.0f};
    float previousYaw = 0.0f;
    std::chrono::steady_clock::time_point lastUpdateTime{};

    // Placeholder visuals until real per-type models/skins exist: a
    // Minecraft-player-sized box tinted a flat color. Sized from
    // EntityRegistry's per-type width/height at spawn time (see
    // NetworkClient) — a real guess at the entity's actual footprint,
    // rather than always assuming player size.
    glm::vec2 boundingBox{0.6f, 1.8f}; // width, height.
    glm::vec3 color{0.8f, 0.2f, 0.2f};

    float hurtAmount = 0.0f; // 0-1 red hit-flash intensity (tick invulnerability).
    bool visible = true;

    // Render-time position, blending previousPosition -> position over
    // ENTITY_INTERP_SECONDS starting at lastUpdateTime. Clamped to
    // position once that window has elapsed (the common case: entities
    // update roughly once per server tick, i.e. faster than the window).
    glm::vec3 InterpolatedPosition(std::chrono::steady_clock::time_point now) const
    {
        float alpha = InterpolationAlpha(now);
        return glm::mix(previousPosition, position, alpha);
    }

    // Same blend for yaw, taking the shortest way around the circle so an
    // update that crosses the +-pi wraparound doesn't spin the long way.
    float InterpolatedYaw(std::chrono::steady_clock::time_point now) const
    {
        float alpha = InterpolationAlpha(now);
        float delta = std::fmod(yaw - previousYaw + glm::pi<float>(), glm::two_pi<float>());
        if (delta < 0.0f) delta += glm::two_pi<float>();
        delta -= glm::pi<float>();
        return previousYaw + delta * alpha;
    }

private:
    float InterpolationAlpha(std::chrono::steady_clock::time_point now) const
    {
        float elapsed = std::chrono::duration<float>(now - lastUpdateTime).count();
        return std::clamp(elapsed / ENTITY_INTERP_SECONDS, 0.0f, 1.0f);
    }
};

} // namespace Volcano

#endif
