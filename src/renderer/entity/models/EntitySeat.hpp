#pragma once
#ifndef VOLCANO_ENTITY_SEAT_H
#define VOLCANO_ENTITY_SEAT_H

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>
#include "../../../GlobalState.hpp"
#include "Entity.hpp"
#include "EntityRegistry.hpp"

namespace Volcano::EntitySeat {

// What kind of seat a vehicle type offers. Pose code picks sitting poses off this.
enum class Kind : uint8_t { None, Boat, Horse, Pig, Strider, Minecart };

inline bool EndsWith(const std::string& text, const char* suffix)
{
    std::string tail(suffix);
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

inline Kind KindOf(const std::string& typeName)
{
    if (EndsWith(typeName, "boat") || EndsWith(typeName, "raft")) return Kind::Boat;
    if (typeName == "horse" || typeName == "donkey" || typeName == "mule"
        || typeName == "skeleton_horse" || typeName == "zombie_horse") return Kind::Horse;
    if (typeName == "pig") return Kind::Pig;
    if (typeName == "strider") return Kind::Strider;
    if (typeName == "minecart") return Kind::Minecart;
    return Kind::None;
}

inline Kind KindOf(const Entity& vehicle)
{
    const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(vehicle.type);
    return info != nullptr ? KindOf(info->name) : Kind::None;
}

// Where a rider's feet sit relative to the vehicle's origin: blocks along the
// vehicle's facing, and blocks up. Two boat riders take the front and back
// seats (vanilla's 0.2 and -0.6).
struct Seat
{
    float forward = 0.0f;
    float up = 0.0f;
};

inline Seat SeatFor(Kind kind, float vehicleHeight, int index, int count)
{
    switch (kind)
    {
        case Kind::Boat: return { count > 1 ? (index == 0 ? 0.2f : -0.6f) : 0.0f, -0.45f };
        case Kind::Horse:
        case Kind::Pig: return { 0.0f, vehicleHeight * 0.75f - 0.55f };
        case Kind::Strider: return { 0.0f, vehicleHeight - 0.94f };
        case Kind::Minecart: return { 0.0f, -0.3f };
        default: return {};
    }
}

inline glm::vec3 SeatPosition(Kind kind, glm::vec3 vehiclePosition, float vehicleYaw, float vehicleHeight, int index, int count)
{
    Seat seat = SeatFor(kind, vehicleHeight, index, count);
    return vehiclePosition + glm::vec3(std::sin(vehicleYaw) * seat.forward, seat.up, std::cos(vehicleYaw) * seat.forward);
}

// Where a passenger renders: the interpolated seat on its vehicle. Returns
// the vehicle's Kind and fills position and vehicleYaw, or Kind::None (outputs
// untouched) when the entity isn't riding a tracked vehicle. Caller holds
// state->entitiesMutex.
inline Kind Resolve(GlobalState* state, const Entity& passenger, std::chrono::steady_clock::time_point now,
    glm::vec3& position, float& vehicleYaw)
{
    if (!passenger.IsPassenger()) return Kind::None;
    const Entity* vehicle = state->FindEntity(static_cast<uint32_t>(passenger.vehicleId));
    if (vehicle == nullptr) return Kind::None;

    Kind kind = KindOf(*vehicle);
    int count = static_cast<int>(vehicle->passengers.size());
    int index = 0;
    for (int i = 0; i < count; i++)
    {
        if (vehicle->passengers[static_cast<size_t>(i)] == passenger.id) index = i;
    }
    vehicleYaw = vehicle->InterpolatedYaw(now);
    position = SeatPosition(kind, vehicle->InterpolatedPosition(now), vehicleYaw, vehicle->boundingBox.y, index, count);
    return kind;
}

// The local player's seat index among the vehicle's riders (0 when alone).
// Caller holds state->entitiesMutex.
inline int LocalSeatIndex(GlobalState* state, const Entity& vehicle)
{
    uint32_t localId = static_cast<uint32_t>(state->localEntityId.load());
    for (size_t i = 0; i < vehicle.passengers.size(); i++)
    {
        if (vehicle.passengers[i] == localId) return static_cast<int>(i);
    }
    return 0;
}

} // namespace Volcano::EntitySeat

#endif
