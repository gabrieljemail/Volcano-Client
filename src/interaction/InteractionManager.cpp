#include "InteractionManager.hpp"
#include "../Logger.hpp"
#include "../inventory/ItemRegistry.hpp"
#include "../inventory/ToolStats.hpp"
#include "../renderer/entity/models/EntityRegistry.hpp"
#include "../renderer/terrain/models/BlockRegistry.hpp"
#include "../network/NetworkClient.hpp"
#include "../inventory/ItemRegistry.hpp"
#include "../renderer/entity/models/EntityRegistry.hpp"
#include "../TickLoop.hpp"
#include "../audio/BlockSounds.hpp"
#include "CombatSounds.hpp"
#include "events/AttackEvent.hpp"
#include "events/InteractEntityEvent.hpp"
#include "events/BreakBlockEvent.hpp"
#include "events/BlockEntityInteractEvent.hpp"
#include "events/PlaceBlockEvent.hpp"
#include "events/UseItemEvent.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <variant>
#include <string>

namespace Volcano {

// Declared here rather than including network/NetworkClient.hpp, for the
// same asio/<windows.h> reason GUIController.cpp and RenderThread.cpp give.
void SendAttack(GlobalState* state, uint32_t targetEntityId);
void SendSwing(GlobalState* state, int32_t hand);

namespace {

// One ray-vs-AABB hit: how far along the ray, and the outward normal of
// whichever face it entered through (pointing back toward the ray's
// origin) - always one of the 6 axis-aligned unit directions.
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
    int hitAxis = -1; // Which axis last raised tMin

    for (int axis = 0; axis < 3; axis++)
    {
        if (direction[axis] == 0.0f)
        {
            // Parallel to this axis's slab
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
    // on every axis (tMin never left 0)
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
// largest magnitude wins, its sign giving the direction
glm::ivec3 NearestCardinalDirection(const glm::vec3& v)
{
    glm::vec3 absV = glm::abs(v);
    if (absV.x >= absV.y && absV.x >= absV.z) return glm::ivec3(v.x > 0.0f ? 1 : -1, 0, 0);
    if (absV.y >= absV.x && absV.y >= absV.z) return glm::ivec3(0, v.y > 0.0f ? 1 : -1, 0);
    return glm::ivec3(0, 0, v.z > 0.0f ? 1 : -1);
}

// DDA (Amanatides & Woo) voxel walk
std::optional<TargetedBlock> RaycastBlocks(GlobalState* state, const glm::vec3& origin,
                                            const glm::vec3& direction, float maxDistance)
{
    glm::ivec3 cell(static_cast<int>(std::floor(origin.x)),
                     static_cast<int>(std::floor(origin.y)),
                     static_cast<int>(std::floor(origin.z)));

    glm::ivec3 step;
    glm::vec3 tMax, tDelta;
    // A large finite sentinel, not actual infinity: this project builds
    // with -ffast-math, which assumes no infinities/NaNs exist
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
        // Fluids are never targeted (vanilla: air to the crosshair), so
        // swimming doesn't try to break water and take the mining penalty.
        bool isFluid = block.nonCubeVisualId != 0
            && BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId).shape == BlockRegistry::NonCubeShape::Liquid;
        BlockRegistry::CollisionBoxes shape;
        if (!isFluid) shape = BlockRegistry::GetSelectionBoxes(block);
        if (shape.count > 0)
        {
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
                // outside the unit cube (a fence/wall post above y=1 - see
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

// Vanilla's crosshair can't target dropped items, XP orbs or projectiles
// (Entity::isPickable), and the server kicks for interacting with or
// attacking them.
bool IsTargetable(uint16_t entityType)
{
    const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entityType);
    if (info == nullptr) return false;
    return info->entityKind != "projectile" && info->name != "item" && info->name != "experience_orb";
}

bool IsSword(const ItemRegistry::ItemTypeInfo* item)
{
    return item != nullptr && item->name.size() > 6
        && item->name.compare(item->name.size() - 6, 6, "_sword") == 0;
}

bool IsSpear(const ItemRegistry::ItemTypeInfo* item)
{
    return item != nullptr && item->name.size() > 6
        && item->name.compare(item->name.size() - 6, 6, "_spear") == 0;
}

// One entity-shaped raycast hit.
struct EntityHit { uint32_t entityId; float distance; };

// Linear scan, but with a cheap bounding-sphere reject before the real
// ray/AABB test
std::optional<EntityHit> RaycastEntities(GlobalState* state, const glm::vec3& origin,
                                          const glm::vec3& direction, float maxDistance,
                                          std::chrono::steady_clock::time_point now)
{
    std::optional<EntityHit> best;

    std::lock_guard<std::mutex> lock(state->entitiesMutex);
    state->ForEachEntity([&](uint32_t id, const Entity& entity)
    {
        if (!IsTargetable(entity.type)) return;
        if (entity.dead) return;
        if (const PlayerEntity* player = state->FindPlayerEntity(id); player && player->IsSpectator()) return;

        glm::vec3 halfExtents(entity.boundingBox.x * 0.5f, entity.boundingBox.y * 0.5f, entity.boundingBox.x * 0.5f);
        glm::vec3 center = entity.InterpolatedPosition(now) + glm::vec3(0.0f, halfExtents.y, 0.0f);

        float boundingRadius = glm::length(halfExtents);
        float rejectDist = maxDistance + boundingRadius;
        if (glm::dot(center - origin, center - origin) > rejectDist * rejectDist) return;

        std::optional<RayHit> hit = RayIntersectsAABB(origin, direction, center - halfExtents, center + halfExtents, maxDistance);
        if (hit && (!best || hit->distance < best->distance)) best = EntityHit{id, hit->distance};
    });

    return best;
}

} // namespace

InteractionManager::InteractionManager(GlobalState* globalState)
    : state(globalState)
{
    // Config::Get returns a ConfigValue (std::variant<std::string,uint32_t,
    // bool>), not a raw number - every setting here is stored as uint32_t,
    // so it needs unwrapping via std::get before it can go anywhere near a
    // uint8_t/float field. Alpha is stored as a 0-100 percentage (so it
    // reads sanely in a hand-edited settings.json), which also needs a
    // floating-point divide by 100
    auto GetColorByte = [this](const std::string& key, uint32_t fallback) {
        return static_cast<uint8_t>(std::get<uint32_t>(state->config->Get(key, uint32_t{fallback})));
    };
    auto GetAlpha = [this](const std::string& key, uint32_t fallbackPercent) {
        uint32_t percent = std::get<uint32_t>(state->config->Get(key, uint32_t{fallbackPercent}));
        return static_cast<float>(percent) / 100.0f;
    };

    selectionOutline.r = GetColorByte("Graphics.BlockSelection.Outline.Red", 0);
    selectionOutline.g = GetColorByte("Graphics.BlockSelection.Outline.Green", 0);
    selectionOutline.b = GetColorByte("Graphics.BlockSelection.Outline.Blue", 0);
    selectionOutline.a = GetAlpha("Graphics.BlockSelection.Outline.Alpha", 48);

    selectionFill.r = GetColorByte("Graphics.BlockSelection.Fill.Red", 255);
    selectionFill.g = GetColorByte("Graphics.BlockSelection.Fill.Green", 255);
    selectionFill.b = GetColorByte("Graphics.BlockSelection.Fill.Blue", 255);
    selectionFill.a = GetAlpha("Graphics.BlockSelection.Fill.Alpha", 16);
}

void InteractionManager::Update(bool inputAllowed)
{
    RefreshHeldItems();
    RefreshReach();

    // Keeps the selection current even behind an open Screen/chat box,
    // same as TickLoop::Advance
    RaycastSelection();

    UpdateUsingItem(inputAllowed);

    // ImGui owns the mouse while a Screen/chat box is focused.
    if (!inputAllowed) return;

    primaryPressedThisFrame = state->input->WasActivated("PrimaryActionPress");
    if (primaryPressedThisFrame)
    {
        PrimaryTriggerStart();
    }
    if (state->input->IsActive("PrimaryAction"))
    {
        PrimaryTriggerHold();
    }
    else if (mining.has_value())
    {
        // The button came up somewhere between last frame and this one -
        // PrimaryTrigger (the only other place that would see this) simply
        // isn't called while the button's up, so this is the one place a
        // release actually gets observed.
        CancelMining();
    }

    // Not mutually exclusive on the press frame
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

float InteractionManager::GetAttackStrengthScale(float partialTicks) const
{
    double attackSpeed = state->attributes->GetDouble("generic.attack_speed", 4.0);
    if (attackSpeed <= 0.0) return 1.0f;

    // TODO: This is an expensive operation. Look into reducing this.
    double ticksToFullCharge = 20.0 / attackSpeed; // Vanilla's own getCurrentItemAttackStrengthDelay().
    float ticks = static_cast<float>(state->attackStrengthTicker.load(std::memory_order_relaxed)) + partialTicks;
    return std::clamp(ticks / static_cast<float>(ticksToFullCharge), 0.0f, 1.0f);
}

void InteractionManager::RefreshHeldItems()
{
    std::lock_guard<std::mutex> lock(state->inventory.mutex);
    selectedSlot = state->inventory.selectedHotbarSlot;
    mainhand = state->inventory.hotbar.Get(selectedSlot);
    offhand = state->inventory.offhand.Get(0);

    mainhandEmpty = mainhand.IsEmpty();

    const ItemRegistry::ItemTypeInfo* mainInfo = ItemRegistry::Lookup(mainhand.itemId);
    const ItemRegistry::ItemTypeInfo* offInfo = ItemRegistry::Lookup(offhand.itemId);
    offhandInteractible = !offhand.IsEmpty() && ItemBehaviorFor(offInfo).action != UseAction::None
        && (mainhand.IsEmpty() || !UsesOnBlocks(mainInfo));
}

void InteractionManager::RefreshReach()
{
    // Named the way the server's Update Attributes packet names them (see
    // NetworkClient's ATTRIBUTE_NAMES).
    reachDistance = static_cast<float>(state->attributes->GetDouble("player.entity_interaction_range", 3.0));
    blockReachDistance = static_cast<float>(state->attributes->GetDouble("player.block_interaction_range", 4.5));
}

void InteractionManager::PrimaryTriggerStart()
{
    // Vanilla's isHandsBusy: no attacking or mining mid-use.
    if (usingState.has_value())
    {
        CancelMining();
        return;
    }

    // Vanilla never refuses a swing
    if (targetedEntityId.has_value())
    {
        // An entity in front always wins over whatever was being mined
        // (mirrors RaycastSelection picking whichever the ray reaches
        // first) - attack itself is edge-gated, one dispatch per click,
        // same as before mining existed.
        CancelMining();
        AttackEntity(*targetedEntityId);

        // Vanilla swings on every click, hit or not, right after the
        // Attack packet above.
        constexpr int32_t MAIN_HAND = 0;
        SendSwing(state, MAIN_HAND);
        state->attackStrengthTicker.store(0, std::memory_order_relaxed);
        return;
    }

    if (!targetedBlock.has_value())
    {
        // An air-swing. Vanilla still sends Swing Arm for one (visual only,
        // no packet this client models yet needs it for) - no dispatch
        // needed here.
        CancelMining();
        lastAttackType = AttackType::Miss;
        if (state->combatSounds != nullptr && IsSpear(ItemRegistry::Lookup(mainhand.itemId)))
        {
            state->combatSounds->PlaySpear(ItemRegistry::Lookup(mainhand.itemId)->name, /*landedHit=*/false);
        }
        state->attackStrengthTicker.store(0, std::memory_order_relaxed);
        return;
    }

    // A block is targeted: BeginMining/ContinueMining/CancelMining below
    // drive that state machine from PrimaryTriggerHold instead, since
    // mining needs to keep running across every frame the button stays
    // down, not just the press edge.
}

void InteractionManager::PrimaryTriggerHold()
{
    if (usingState.has_value())
    {
        CancelMining();
        return;
    }

    if (targetedEntityId.has_value())
    {
        // Vanilla doesn't keep attacking while the button stays down -
        // PrimaryTriggerStart already handled the click.
        return;
    }

    if (!targetedBlock.has_value())
    {
        CancelMining();
        return;
    }

    if (mining.has_value() && mining->position != targetedBlock->position)
    {
        // Looked at a different block mid-hold
        CancelMining();
    }

    if (!mining.has_value())
    {
        BeginMining(*targetedBlock);
        return;
    }

    ContinueMining();
}

void InteractionManager::BeginMining(const TargetedBlock& target)
{
    // Holding: wait out vanilla's destroyDelay since the last break.
    auto now = std::chrono::steady_clock::now();
    if (!primaryPressedThisFrame && now - lastBreakTime < BREAK_REPEAT_DELAY) return;

    bool instabuild = state->canInstabuild.load(std::memory_order_relaxed);

    // Like vanilla's creative mode: a sword can't break blocks.
    if (instabuild && IsSword(ItemRegistry::Lookup(mainhand.itemId))) return;

    Block block = state->world->GetBlock(target.position.x, target.position.y, target.position.z);
    int32_t breakTicks = BreakTicksFor(block);

    if (instabuild || breakTicks == 0)
    {
        lastBreakTime = now;
        BreakBlockEvent{ target.position, target.face }.Dispatch(state);
        return;
    }
    if (breakTicks < 0) return; // Unbreakable (bedrock, ...), or not resolved from a real state id.

    SendPlayerAction(state, DigStatus::StartDigging, target.position, target.face);
    SendArmAnimation(state, Hand::Main);
    mining = MiningState{ target.position, target.face, breakTicks, std::chrono::steady_clock::now() };
    PublishMining();
}

void InteractionManager::PublishMining()
{
    if (mining.has_value())
    {
        state->miningStartNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            mining->startTime.time_since_epoch()).count(), std::memory_order_relaxed);
        state->miningTicksRequired.store(mining->ticksRequired, std::memory_order_relaxed);
    }
    else
    {
        state->miningTicksRequired.store(0, std::memory_order_relaxed);
    }
}

// Vanilla's destroy progress per tick: speed / hardness / (30 if the held
// tool can harvest the block, else 100), where speed is the tool's tier
// speed on its own block type (1 otherwise) and a fifth of that in the air.
// Not modeled yet: Efficiency, Haste/Mining Fatigue, underwater penalty.
int32_t InteractionManager::BreakTicksFor(const Block& block) const
{
    if (block.breakTicks <= 0) return block.breakTicks; // Unbreakable or instant.

    const BlockRegistry::BreakInfo& info = BlockRegistry::GetBreakInfo(block.breakInfoId);
    if (info.hardness <= 0.0f) return block.breakTicks;

    float speed = 1.0f;
    bool canHarvest = !info.requiresTool;
    if (!mainhand.IsEmpty())
    {
        if (const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(mainhand.itemId))
        {
            ToolStats tool = ToolStatsForItem(item->name);
            if (tool.toolClass != BlockRegistry::ToolClass::None && tool.toolClass == info.toolClass) speed = tool.speed;
        }
        if (info.requiresTool)
        {
            canHarvest = std::find(info.harvestToolIds.begin(), info.harvestToolIds.end(),
                static_cast<uint16_t>(mainhand.itemId)) != info.harvestToolIds.end();
        }
    }
    if (!state->tickLoop->IsGrounded()) speed /= 5.0f;

    float damage = speed / info.hardness / (canHarvest ? 30.0f : 100.0f);
    if (damage >= 1.0f) return 0;
    return std::max(1, static_cast<int32_t>(std::ceil(1.0f / damage)));
}

void InteractionManager::ContinueMining()
{
    auto now = std::chrono::steady_clock::now();
    if (state->blockSounds != nullptr && now - lastHitSoundTime >= std::chrono::milliseconds(200))
    {
        lastHitSoundTime = now;
        state->blockSounds->PlayHit(state->world->GetBlock(mining->position.x, mining->position.y, mining->position.z));
    }

    double elapsedTicks = std::chrono::duration<double>(std::chrono::steady_clock::now() - mining->startTime).count() * 20.0;
    if (elapsedTicks < static_cast<double>(mining->ticksRequired)) return;

    BreakBlockEvent{ mining->position, mining->face, /*sendStart=*/false }.Dispatch(state);
    mining.reset();
    PublishMining();
    lastBreakTime = std::chrono::steady_clock::now();
}

void InteractionManager::CancelMining()
{
    if (!mining.has_value()) return;
    SendPlayerAction(state, DigStatus::CancelDigging, mining->position, mining->face);
    mining.reset();
    PublishMining();
}

void InteractionManager::AttackEntity(uint32_t entityId)
{
    // Snapshot what the classification needs about the target. The ray
    // cast that picked it ran this same frame, but the network thread may
    // have removed it since.
    bool targetIsPlayer = false;
    bool targetIsLiving = false;
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        if (const PlayerEntity* player = state->FindPlayerEntity(entityId))
        {
            if (player->IsSpectator()) return;
            targetIsPlayer = true;
            targetIsLiving = true;
        }
        else if (const Entity* entity = state->FindEntity(entityId))
        {
            const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity->type);
            targetIsLiving = info != nullptr && info->IsLiving();
        }
        else
        {
            return;
        }
    }

    // Classified before the swing resets attackStrengthTicker (see
    // PrimaryTriggerStart), with vanilla's half-tick lookahead.
    TickLoop::MovementSnapshot movement = state->tickLoop->GetMovementSnapshot();
    lastAttackType = ClassifyAttack(GetAttackStrengthScale(0.5f), movement, targetIsLiving);

    SendAttack(state, entityId);

    // The part of a hit vanilla applies to the attacker on the client
    // (Player.causeExtraKnockback): any knockback
    float knockback = static_cast<float>(state->attributes->GetDouble("generic.attack_knockback", 0.0))
        + (lastAttackType == AttackType::Knockback ? 0.5f : 0.0f);
    if (targetIsPlayer && knockback > 0.0f)
    {
        constexpr float ATTACKER_SLOWDOWN = 0.6f;
        state->localMotion.Push({ LocalMotionEvent::Kind::AttackSlowdown, glm::vec3(ATTACKER_SLOWDOWN) });
    }

    PlayAttackSounds(movement);

    static constexpr const char* ATTACK_TYPE_NAMES[] = { "miss", "normal", "knockback", "critical", "sweep" };
    Log::Info(std::string("[COMBAT] Attacked entity ") + std::to_string(entityId) + " ("
        + ATTACK_TYPE_NAMES[static_cast<size_t>(lastAttackType)] + " hit).");
}

void InteractionManager::PlayAttackSounds(const TickLoop::MovementSnapshot& movement)
{
    if (state->combatSounds == nullptr) return;

    const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(mainhand.itemId);
    if (IsSpear(item))
    {
        state->combatSounds->PlaySpear(item->name, /*landedHit=*/true);
        return;
    }

    // Same half-tick lookahead as the classification in AttackEntity.
    state->combatSounds->PlayAttack(lastAttackType, GetAttackStrengthScale(0.5f) > 0.9f);

    // Vanilla's smash needs a fall of more than 1.5 blocks.
    if (item != nullptr && item->name == "mace" && movement.fallDistance > 1.5f)
    {
        state->combatSounds->PlayMaceSmash(movement.fallDistance, movement.grounded);
    }
}

AttackType InteractionManager::ClassifyAttack(float strength, const TickLoop::MovementSnapshot& movement,
                                               bool targetIsLiving) const
{
    bool fullStrength = strength > 0.9f;

    // Critical: falling (not rising, not standing) at full strength, not
    // sprinting, against something alive. Climbing/water/blindness/riding
    // would also rule it out in vanilla; none of those are modeled here.
    bool critical = fullStrength && movement.fallDistance > 0.0f && !movement.grounded
        && !movement.sprinting && targetIsLiving;
    if (critical) return AttackType::Critical;

    bool knockback = fullStrength && movement.sprinting;
    if (knockback) return AttackType::Knockback;

    // Sweep: a sword swung at full strength while on the ground and moving
    // slower than 2.5x the movement_speed attribute (per tick)
    if (fullStrength && movement.grounded)
    {
        float speedLimit = static_cast<float>(state->attributes->GetDouble("generic.movement_speed", 0.1)) * 2.5f;
        if (IsSword(ItemRegistry::Lookup(mainhand.itemId)) &&movement.horizontalSpeed < speedLimit) return AttackType::Sweep;
    }

    return AttackType::Normal;
}

void InteractionManager::SecondaryTriggerStart()
{
    // Starts the repeat timer too, or this frame's Hold would place again.
    lastPlaceTime = std::chrono::steady_clock::now();
    DispatchUse();
}

void InteractionManager::SecondaryTriggerHold()
{
    // A continuous use (eating, a shield, a bow) is timed by the server off
    // the single UseItem packet it started with, so nothing is sent while one
    // is running. Otherwise vanilla repeats the click while the button stays
    // down (bridging, the next apple), paced by PLACE_REPEAT_DELAY.
    if (usingState.has_value()) return;

    bool canRepeat = targetedBlock.has_value();
    if (!canRepeat)
    {
        const ItemStack& held = HeldItem(offhandInteractible ? Hand::Off : Hand::Main);
        UseAction action = ItemBehaviorFor(ItemRegistry::Lookup(held.itemId)).action;
        canRepeat = action != UseAction::None;
    }
    if (!canRepeat) return;

    auto now = std::chrono::steady_clock::now();
    if (now - lastPlaceTime < PLACE_REPEAT_DELAY) return;
    lastPlaceTime = now;
    DispatchUse();
}

void InteractionManager::DispatchUse()
{
    if (usingState.has_value()) return;

    // Mainhand always gets the block/entity interaction; UseHeldItem decides
    // whether the offhand takes over the item use.
    Hand hand = Hand::Main;

    if (targetedEntityId.has_value())
    {
        bool sneaking = state->tickLoop->IsSneaking();
        InteractEntityEvent{ *targetedEntityId, hand, sneaking }.Dispatch(state);

        // Feedable or tradable mobs consume the click in vanilla; don't also
        // start eating the food that was just handed to them.
        std::string kind;
        {
            std::lock_guard<std::mutex> lock(state->entitiesMutex);
            if (const Entity* entity = state->FindEntity(*targetedEntityId))
            {
                const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity->type);
                if (info != nullptr) kind = info->entityKind;
            }
        }
        UseAction action = ItemBehaviorFor(ItemRegistry::Lookup(HeldItem(offhandInteractible ? Hand::Off : Hand::Main).itemId)).action;
        bool consumable = action == UseAction::Eat || action == UseAction::Drink;
        bool entityTakesIt = kind == "animal" || kind == "passive" || kind == "mob";
        if (!(consumable && entityTakesIt)) UseHeldItem();
        return;
    }

    if (targetedBlock.has_value())
    {
        bool interactsWithBlock = state->world->HasBlockEntity(targetedBlock->position)
            || state->world->GetBlock(targetedBlock->position.x, targetedBlock->position.y,
                                      targetedBlock->position.z).isCraftingTable;
        // Vanilla: sneaking with something in hand places against the block
        // instead of opening it.
        if (interactsWithBlock && !(state->tickLoop->IsSneaking() && !mainhand.IsEmpty()))
        {
            BlockEntityInteractEvent{ targetedBlock->position, targetedBlock->face, hand }.Dispatch(state);
            return;
        }

        // Usable items (food, drinks, shields, armor) win over a plain
        // block: no placement click is sent, so you can eat facing grass.
        ItemBehavior useBehavior = ItemBehaviorFor(ItemRegistry::Lookup(HeldItem(offhandInteractible ? Hand::Off : Hand::Main).itemId));
        if (useBehavior.action != UseAction::None || useBehavior.armorSlot >= 0)
        {
            UseHeldItem();
            return;
        }

        // Block items share their block's name ("stone" -> stone), which is
        // enough to predict the common case; anything else just waits for
        // the server.
        int32_t predictedStateId = -1;
        const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(mainhand.itemId);
        if (item != nullptr) predictedStateId = BlockRegistry::DefaultStateIdForName(item->name);

        // The offhand takes the click (a shield with a sword in hand) without
        // placing anything against the block first.
        if (!offhandInteractible)
        {
            PlaceBlockEvent{ targetedBlock->position, targetedBlock->face, hand, predictedStateId }.Dispatch(state);
        }
        // A placed block ends the click; anything else (armor, food, a
        // shield) falls through to its own use, as vanilla's PASS does.
        if (predictedStateId >= 0 && !offhandInteractible) return;

        // Tools and the like were fully handled by the block click. Buckets,
        // boats and thrown items pass it and need the UseItem that follows.
        ItemBehavior behavior = ItemBehaviorFor(item);
        if (!offhandInteractible && behavior.action == UseAction::None && behavior.armorSlot < 0
            && !UsesItemAfterBlockClick(item)) return;
    }

    UseHeldItem();
}

void InteractionManager::UseHeldItem()
{
    Hand hand = offhandInteractible ? Hand::Off : Hand::Main;
    const ItemStack& held = HeldItem(hand);
    if (held.IsEmpty()) return;

    const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(held.itemId);
    ItemBehavior behavior = ItemBehaviorFor(item);

    // Nothing to eat at full hunger (creative can always eat, like vanilla's
    // invulnerable abilities).
    if (behavior.action == UseAction::Eat && !behavior.alwaysEdible
        && !state->canInstabuild.load(std::memory_order_relaxed))
    {
        int32_t food;
        {
            std::lock_guard<std::mutex> lock(state->healthMutex);
            food = state->food;
        }
        if (food >= 20) return;
    }

    // Negated like TickThread's per-tick report: the camera turns the
    // opposite way from Minecraft's convention.
    float yaw = std::remainder(-state->player->camera.GetYaw(), 360.0f);
    float pitch = -state->player->camera.GetPitch();
    bool continuous = behavior.action != UseAction::None;
    UseItemEvent{ hand, yaw, pitch, /*swing=*/!continuous }.Dispatch(state);

    if (!continuous) return;
    usingState = UsingState{ hand, held.itemId, selectedSlot, behavior.action,
                             behavior.useTicks, std::chrono::steady_clock::now() };
    PublishUsing();
}

void InteractionManager::StopUsingItem()
{
    if (!usingState.has_value()) return;
    // Vanilla's release action ignores position; same fixed dummies as before.
    SendPlayerAction(state, DigStatus::ReleaseUseItem, glm::ivec3(0), glm::ivec3(0, 1, 0));
    usingState.reset();
    PublishUsing();
}

void InteractionManager::UpdateUsingItem(bool inputAllowed)
{
    if (!usingState.has_value()) return;

    // Finished by itself: the server consumed the item, nothing to release.
    if (usingState->useTicks > 0)
    {
        double elapsedTicks = std::chrono::duration<double>(std::chrono::steady_clock::now() - usingState->startTime).count() * 20.0;
        if (elapsedTicks >= static_cast<double>(usingState->useTicks))
        {
            usingState.reset();
            PublishUsing();
            return;
        }
    }

    // Switching slots or items, losing the stack, a screen opening, or letting
    // go all end the use early.
    const ItemStack& held = HeldItem(usingState->hand);
    bool itemChanged = held.IsEmpty() || held.itemId != usingState->itemId
        || (usingState->hand == Hand::Main && selectedSlot != usingState->hotbarSlot);
    if (itemChanged || !inputAllowed || !state->input->IsActive("SecondaryActionHold"))
    {
        StopUsingItem();
    }
}

void InteractionManager::PublishUsing()
{
    if (usingState.has_value())
    {
        state->useStartNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
            usingState->startTime.time_since_epoch()).count(), std::memory_order_relaxed);
        state->usingItemAction.store(static_cast<uint8_t>(usingState->action), std::memory_order_relaxed);
    }
    state->usingItem.store(usingState.has_value(), std::memory_order_relaxed);
}

void InteractionManager::SecondaryTriggerRelease()
{
    // Only a continuous use has anything to release; vanilla sends nothing
    // otherwise.
    StopUsingItem();
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
    // type - see its own signature/comment); the result is already
    // ~unit-length by construction either way.
    Quat3 normalized{direction.x, direction.y, direction.z};
    Normalize(normalized);
    direction = glm::vec3(normalized.x, normalized.y, normalized.z);

    // Whichever the ray reaches first wins
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
