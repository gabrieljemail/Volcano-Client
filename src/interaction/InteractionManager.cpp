#include "InteractionManager.hpp"
#include "../renderer/terrain/models/BlockRegistry.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <variant>
#include <string>

namespace Volcano {

namespace {

// One ray-vs-AABB hit: how far along the ray, and the outward normal of
// whichever face it entered through (pointing back toward the ray's
// origin) — always one of the 6 axis-aligned unit directions.
struct RayHit { float distance; glm::ivec3 normal; };

// Ray-vs-AABB slab test. Returns the entry hit if it hits [boxMin, boxMax]
// within [0, maxDistance], else nullopt. `direction` must be unit length
// (not required by the math, just by every caller below, which all compare
// returned distances against each other).
std::optional<RayHit> RayIntersectsAABB(const glm::vec3& origin, const glm::vec3& direction,
                                         const glm::vec3& boxMin, const glm::vec3& boxMax,
                                         float maxDistance)
{
    float tMin = 0.0f;
    float tMax = maxDistance;
    int hitAxis = -1; // Which axis last raised tMin — i.e. the entry face, once the loop finishes.

    for (int axis = 0; axis < 3; axis++)
    {
        if (direction[axis] == 0.0f)
        {
            // Parallel to this axis's slab — inside it, or never hits.
            if (origin[axis] < boxMin[axis] || origin[axis] > boxMax[axis]) return std::nullopt;
            continue;
        }

        float invDir = 1.0f / direction[axis];
        float t1 = (boxMin[axis] - origin[axis]) * invDir;
        float t2 = (boxMax[axis] - origin[axis]) * invDir;
        if (t1 > t2) std::swap(t1, t2);

        if (t1 > tMin)
        {
            tMin = t1;
            hitAxis = axis;
        }
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return std::nullopt;
    }

    glm::ivec3 normal(0);
    // hitAxis stays -1 only if the ray's origin is already inside the box
    // on every axis (tMin never left 0) — an edge case (the camera clipped
    // into a block), not a real face; leave normal zeroed rather than guess.
    if (hitAxis >= 0)
    {
        // Moving in +axis means the box's near (min) face is what the ray
        // crossed to get in, and that face's own outward normal points
        // back the way the ray came: -axis. Moving in -axis, the near
        // face is the max one, so its outward normal is +axis.
        normal[hitAxis] = (direction[hitAxis] > 0.0f) ? -1 : 1;
    }

    return RayHit{tMin, normal};
}

// Vanilla's own Direction.getNearest(x,y,z): whichever axis has the
// largest magnitude wins, its sign giving the direction — turns a
// continuous vector into one of the 6 cardinal directions.
glm::ivec3 NearestCardinalDirection(const glm::vec3& v)
{
    glm::vec3 absV = glm::abs(v);
    if (absV.x >= absV.y && absV.x >= absV.z) return glm::ivec3(v.x > 0.0f ? 1 : -1, 0, 0);
    if (absV.y >= absV.x && absV.y >= absV.z) return glm::ivec3(0, v.y > 0.0f ? 1 : -1, 0);
    return glm::ivec3(0, 0, v.z > 0.0f ? 1 : -1);
}

// DDA (Amanatides & Woo) voxel walk — visits only the cells the ray
// actually passes through between origin and maxDistance, in order, instead
// of scanning every cell in a bounding box (most of which the ray never
// comes near). Stops at the first cell with real collision geometry
// (BlockRegistry::GetCollisionBoxes — the same shapes TickLoop's own
// physics collides against), so a slab/stair is only "hit" where its
// actual shape is, not its full voxel.
std::optional<TargetedBlock> RaycastBlocks(GlobalState* state, const glm::vec3& origin,
                                            const glm::vec3& direction, float maxDistance)
{
    glm::ivec3 cell(static_cast<int>(std::floor(origin.x)),
                     static_cast<int>(std::floor(origin.y)),
                     static_cast<int>(std::floor(origin.z)));

    glm::ivec3 step;
    glm::vec3 tMax, tDelta;
    // A large finite sentinel, not actual infinity: this project builds
    // with -ffast-math, which assumes no infinities/NaNs exist — comparing
    // real IEEE infinity against tMax's other components would be
    // undefined behavior under that flag, not just unusual.
    constexpr float INF = 1e30f;
    for (int axis = 0; axis < 3; axis++)
    {
        if (direction[axis] > 0.0f)
        {
            step[axis] = 1;
            tMax[axis] = (static_cast<float>(cell[axis]) + 1.0f - origin[axis]) / direction[axis];
            tDelta[axis] = 1.0f / direction[axis];
        }
        else if (direction[axis] < 0.0f)
        {
            step[axis] = -1;
            tMax[axis] = (origin[axis] - static_cast<float>(cell[axis])) / -direction[axis];
            tDelta[axis] = -1.0f / direction[axis];
        }
        else
        {
            step[axis] = 0;
            tMax[axis] = INF;
            tDelta[axis] = INF;
        }
    }

    for (float traveled = 0.0f; traveled <= maxDistance; )
    {
        Block block = state->world->GetBlock(cell.x, cell.y, cell.z);
        if (block.hasCollision())
        {
            BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);
            glm::vec3 blockOrigin(cell);

            std::optional<RayHit> closest;
            for (int i = 0; i < shape.count; i++)
            {
                glm::vec3 boxMin = blockOrigin + shape.boxes[static_cast<size_t>(i)].min;
                glm::vec3 boxMax = blockOrigin + shape.boxes[static_cast<size_t>(i)].max;
                std::optional<RayHit> hit = RayIntersectsAABB(origin, direction, boxMin, boxMax, maxDistance);
                if (hit && (!closest || hit->distance < closest->distance)) closest = hit;
            }
            // Cells are visited in increasing distance order, so the first
            // one with any real hit holds the closest hit overall.
            if (closest)
            {
                // Cubic-only face: re-test against the block's full unit
                // cube instead of its real collision shape (see
                // TargetedBlock::face's own comment for why). Falls back to
                // the real shape hit's own normal on a shape that reaches
                // outside the unit cube (a fence/wall post above y=1 — see
                // BlockRegistry::AABB's own comment), where the cube retest
                // can miss even though the shape hit didn't.
                std::optional<RayHit> cubeHit = RayIntersectsAABB(origin, direction,
                    blockOrigin, blockOrigin + glm::vec3(1.0f), maxDistance);
                glm::ivec3 face = cubeHit ? cubeHit->normal : closest->normal;

                glm::vec3 blockCenter = blockOrigin + glm::vec3(0.5f);
                glm::ivec3 relativeDirection = NearestCardinalDirection(origin - blockCenter);

                return TargetedBlock{cell, closest->distance, face, relativeDirection};
            }
        }

        int axis = (tMax.x < tMax.y) ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
        traveled = tMax[axis];
        cell[axis] += step[axis];
        tMax[axis] += tDelta[axis];
    }

    return std::nullopt;
}

// One entity-shaped raycast hit.
struct EntityHit { uint32_t entityId; float distance; };

// Linear scan, but with a cheap bounding-sphere reject before the real
// ray/AABB test — entities within reach are normally few, so this costs
// one subtraction and one dot product for everything the ray has no chance
// of touching, rather than a full slab test for each.
std::optional<EntityHit> RaycastEntities(GlobalState* state, const glm::vec3& origin,
                                          const glm::vec3& direction, float maxDistance,
                                          std::chrono::steady_clock::time_point now)
{
    std::optional<EntityHit> best;

    std::lock_guard<std::mutex> lock(state->entitiesMutex);
    for (const auto& [id, entity] : state->entities)
    {
        glm::vec3 halfExtents(entity.boundingBox.x * 0.5f, entity.boundingBox.y * 0.5f, entity.boundingBox.x * 0.5f);
        glm::vec3 center = entity.InterpolatedPosition(now) + glm::vec3(0.0f, halfExtents.y, 0.0f);

        float boundingRadius = glm::length(halfExtents);
        float rejectDist = maxDistance + boundingRadius;
        if (glm::dot(center - origin, center - origin) > rejectDist * rejectDist) continue;

        std::optional<RayHit> hit = RayIntersectsAABB(origin, direction, center - halfExtents, center + halfExtents, maxDistance);
        if (hit && (!best || hit->distance < best->distance)) best = EntityHit{id, hit->distance};
    }

    return best;
}

} // namespace

InteractionManager::InteractionManager(GlobalState* globalState)
    : state(globalState)
{
    // Config::Get returns a ConfigValue (std::variant<std::string,uint32_t,
    // bool>), not a raw number — every setting here is stored as uint32_t,
    // so it needs unwrapping via std::get before it can go anywhere near a
    // uint8_t/float field. Alpha is stored as a 0-100 percentage (so it
    // reads sanely in a hand-edited settings.json), which also needs a
    // floating-point divide by 100 — an integer one truncates 80/100 to 0.
    auto GetColorByte = [this](const std::string& key, uint32_t fallback) {
        return static_cast<uint8_t>(std::get<uint32_t>(state->config->Get(key, uint32_t{fallback})));
    };
    auto GetAlpha = [this](const std::string& key, uint32_t fallbackPercent) {
        uint32_t percent = std::get<uint32_t>(state->config->Get(key, uint32_t{fallbackPercent}));
        return static_cast<float>(percent) / 100.0f;
    };

    selectionOutline.r = GetColorByte("Graphics.BlockSelection.Outline.Red", 255);
    selectionOutline.g = GetColorByte("Graphics.BlockSelection.Outline.Green", 255);
    selectionOutline.b = GetColorByte("Graphics.BlockSelection.Outline.Blue", 255);
    selectionOutline.a = GetAlpha("Graphics.BlockSelection.Outline.Alpha", 80);

    selectionFill.r = GetColorByte("Graphics.BlockSelection.Fill.Red", 255);
    selectionFill.g = GetColorByte("Graphics.BlockSelection.Fill.Green", 255);
    selectionFill.b = GetColorByte("Graphics.BlockSelection.Fill.Blue", 255);
    selectionFill.a = GetAlpha("Graphics.BlockSelection.Fill.Alpha", 15);
}

void InteractionManager::Update(bool inputAllowed)
{
    RefreshHeldItems();
    RefreshReach();

    // Keeps the selection current even behind an open Screen/chat box,
    // same as TickLoop::Advance — see RenderThread::PollInputs.
    RaycastSelection();

    // ImGui owns the mouse while a Screen/chat box is focused.
    if (!inputAllowed) return;

    if (state->input->IsActive("PrimaryAction"))
    {
        PrimaryTrigger();
    }

    // Not mutually exclusive on the press frame — Hold is already true then
    // too (it's a HOLD action and the button is down), so Start and Hold
    // both fire that frame. Harmless while both are stubs; whoever wires
    // real item-use logic in here can decide whether Start should suppress
    // that frame's Hold call.
    if (state->input->WasActivated("SecondaryAction"))
    {
        SecondaryTriggerStart();
    }
    if (state->input->IsActive("SecondaryActionHold"))
    {
        SecondaryTriggerHold();
    }
    if (state->input->WasActivated("SecondaryActionRelease"))
    {
        SecondaryTriggerRelease();
    }
}

float InteractionManager::GetAttackStrengthScale() const
{
    double attackSpeed = state->attributes->GetDouble("generic.attack_speed", 4.0);
    if (attackSpeed <= 0.0) return 1.0f;

    // TODO: This is an expensive operation. Look into reducing this.
    double ticksToFullCharge = 20.0 / attackSpeed; // Vanilla's own getCurrentItemAttackStrengthDelay().
    float ticks = static_cast<float>(state->attackStrengthTicker.load(std::memory_order_relaxed));
    return std::clamp(ticks / static_cast<float>(ticksToFullCharge), 0.0f, 1.0f);
}

void InteractionManager::RefreshHeldItems()
{
    std::lock_guard<std::mutex> lock(state->inventory.mutex);
    mainhand = state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot);
    offhand = state->inventory.offhand.Get(0);

    mainhandEmpty = mainhand.IsEmpty();
    // See offhandInteractible's own comment — nothing to derive this from yet.
    offhandInteractible = false;
}

void InteractionManager::RefreshReach()
{
    reachDistance = static_cast<float>(state->attributes->GetDouble("generic.attack_range", 3.0));
    blockReachDistance = static_cast<float>(state->attributes->GetDouble("generic.block_interaction_range", 4.5));
}

void InteractionManager::PrimaryTrigger()
{
    // Vanilla never refuses a swing — attackStrengthTicker only scales the
    // damage a landed hit deals (GetAttackStrengthScale), it doesn't gate
    // whether one happens. Resets unconditionally, same as an air-swing
    // does in vanilla.
    state->attackStrengthTicker.store(0, std::memory_order_relaxed);

    // Nothing to dispatch to yet — targetedEntityId/targetedBlock now hold
    // a real target, but this client has no outbound attack/dig packet.
}

void InteractionManager::SecondaryTriggerStart()
{
    // Where an instant item (a block, a bucket) would act immediately, and
    // where a continuous one (food, a shield, a bow) would begin — nothing
    // does yet, same "no outbound packet for it" story as PrimaryTrigger.
}

void InteractionManager::SecondaryTriggerHold()
{
    // Continues whatever SecondaryTriggerStart began (keep eating, keep
    // charging a bow, keep blocking with a shield). Nothing does yet.
}

void InteractionManager::SecondaryTriggerRelease()
{
    // Ends whatever SecondaryTriggerStart began, early (release a drawn
    // bow, lower a shield). Nothing does yet.
}

void InteractionManager::RaycastSelection()
{
    // Origin: eye position. `camera` is a value on Player, not a pointer,
    // and GetEyePosition() wants the feet position handed to it.
    glm::vec3 origin = state->player->camera.GetEyePosition(state->player->GetPosition());

    // Direction: same yaw/pitch -> forward-vector formula as
    // Camera::GetViewMatrix (degrees, via glm::radians).
    float yaw = state->player->camera.GetYaw();
    float pitch = state->player->camera.GetPitch();
    glm::vec3 direction{
        cos(glm::radians(pitch)) * sin(glm::radians(yaw)),
        sin(glm::radians(pitch)),
        cos(glm::radians(pitch)) * cos(glm::radians(yaw))
    };
    // Round-tripped through Quat3 to actually use Normalize (its parameter
    // type — see its own signature/comment); the result is already
    // ~unit-length by construction either way.
    Quat3 normalized{direction.x, direction.y, direction.z};
    Normalize(normalized);
    direction = glm::vec3(normalized.x, normalized.y, normalized.z);

    // Whichever the ray reaches first wins — a wall between the camera and
    // an entity should block targeting it, not lose to it unconditionally.
    std::optional<TargetedBlock> blockHit = RaycastBlocks(state, origin, direction, blockReachDistance);
    std::optional<EntityHit> entityHit = RaycastEntities(state, origin, direction, reachDistance,
                                                          std::chrono::steady_clock::now());

    targetedBlock.reset();
    targetedEntityId.reset();
    if (entityHit && (!blockHit || entityHit->distance < blockHit->distance))
    {
        targetedEntityId = entityHit->entityId;
    }
    else if (blockHit)
    {
        targetedBlock = blockHit;
    }

    state->lookingAtEntity.store(targetedEntityId.has_value());
}

} // namespace Volcano
