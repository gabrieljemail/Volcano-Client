#include "Math.hpp"
#include "TickLoop.hpp"
#include "Logger.hpp"
#include "FluidQuery.hpp"
#include "RideController.hpp"
#include "interaction/InteractionManager.hpp"
#include "ModLoader.hpp"
#include "audio/BlockSounds.hpp"
#include "renderer/terrain/models/BlockRegistry.hpp"
#include "renderer/entity/models/EntityRegistry.hpp"
#include "renderer/entity/models/EntityMotion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <glm/gtc/matrix_transform.hpp>
#include <sstream>

namespace Volcano {

namespace {
    constexpr float PLAYER_HALF_WIDTH = Player::HALF_WIDTH;
    constexpr float PLAYER_HEIGHT = Player::HEIGHT;
    // Fixed-timestep accumulators can spiral if a frame stalls badly (a
    // breakpoint, alt-tab, a slow load) - clamp how much time can accumulate so
    // a stall causes a pause, not a burst of catch-up ticks.
    constexpr float MAX_ACCUMULATED_TIME = 0.25f;
    // Vanilla's own step height: obstructions this tall or shorter (slabs,
    // snow layers/carpets, a staircase's first step) are auto-climbed on a
    // horizontal move instead of blocking it
    constexpr float STEP_HEIGHT = 0.6f;

    // Sneak eye-height dip
    constexpr float STANDING_EYE_HEIGHT = 1.62f;
    constexpr float DEAD_EYE_HEIGHT = 0.2f;
    constexpr float SNEAK_EYE_HEIGHT = STANDING_EYE_HEIGHT - (1.62f - 1.27f) * 0.5f;
    // Exponential ease rate applied per-tick below
    constexpr float EYE_HEIGHT_EASE_RATE = 10.0f;

    bool IsFinite(glm::vec3 v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // A vanilla server disconnects with "Invalid move player packet
    // received" the instant a reported position stops being finite (see
    // ServerGamePacketListenerImpl's own move-player check) - this used to
    // show up client-side only as an opaque connection-aborted error with no
    // way to tell which tick, or which half of FixedStep, produced the bad
    // value. Logged once per occurrence rather than guarded/clamped: a NaN
    // here means a real bug upstream, and silently clamping it would only
    // hide that bug behind a still-wrong position.
    void LogIfNotFinite(const char* what, glm::vec3 v, glm::vec3 velocity, bool grounded)
    {
        if (IsFinite(v)) return;
        std::ostringstream oss;
        oss << "[TICK] Non-finite " << what << ": (" << v.x << ", " << v.y << ", " << v.z
            << ") velocity: (" << velocity.x << ", " << velocity.y << ", " << velocity.z
            << ") grounded: " << (grounded ? "true" : "false");
        Log::Error(oss.str());
    }

    // Vanilla's sprint gates: food must be above this to start or keep
    // sprinting (FoodData's own threshold), and sprint-jumping adds this
    // much forward velocity on top of the jump (LivingEntity.jumpFromGround).
    constexpr int32_t SPRINT_MIN_FOOD = 6;
    constexpr float SPRINT_JUMP_BOOST = 0.2f;

    // A wall hit only stops a sprint if it blocked more than a sliver of the
    // intended movement
    constexpr float MINOR_COLLISION_SIN = 0.1392f;

    // Vanilla's water physics (LivingEntity.travelInWater): flat 0.02 input
    // acceleration, 0.8 drag (0.9 sprinting), 0.04 per tick up or down from
    // the jump and sneak keys, +0.3 for the climb-out hop.
    constexpr float WATER_ACCELERATION = 0.02f;
    constexpr float WATER_DRAG = 0.8f;
    constexpr float SWIM_SPRINT_DRAG = 0.9f;
    constexpr float WATER_VERTICAL_PUSH = 0.04f;
    constexpr float WATER_CLIMB_OUT_SPEED = 0.3f;
    // Crawl box and eye height while sprint-swimming, and how fast swimAmount eases.
    constexpr float SWIM_BOX_HEIGHT = 0.6f;
    constexpr float SWIM_EYE_HEIGHT = 0.4f;
    constexpr float SWIM_AMOUNT_STEP = 0.09f;
    // Blocks travelled between swim stroke sounds.
    constexpr float SWIM_STRIDE_DISTANCE = 1.5f;
} // namespace

TickLoop::TickLoop(GlobalState* stateIn) : state(stateIn)
{
    previousPosition = currentPosition = state->player->GetPosition();
    renderPreviousPosition = renderCurrentPosition = currentPosition;
}

void TickLoop::PublishInput(bool inputAllowedIn, float moveForward, float moveRight,
                             bool sneaking, bool sprinting, bool jumpHeld)
{
    inputAllowed.store(inputAllowedIn);
    moveForwardInput.store(moveForward);
    moveRightInput.store(moveRight);
    sneakActive.store(sneaking);
    sprintActive.store(sprinting);
    jumpHeldActive.store(jumpHeld);
}

void TickLoop::Tick()
{
    // Hold off simulating entirely until the world has something to stand
    // on - otherwise gravity runs from frame one against an empty World
    // (every lookup reads Air) and the player free-falls before any chunk
    // has had a chance to load. See GlobalState::worldReady for the
    // ordering guarantee that makes reading cached position state below
    // safe once this becomes true.
    //
    // Must come before the lastTickTime read/write below
    if (!state->worldReady.load()) return;

    // Self-timed rather than fed a frame delta: measuring elapsed wall time
    // here keeps this correct even across an unusually long frame.
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastTickTime).count();
    lastTickTime = now;

    // Apply any teleport the server sent since the last call. Done here, on
    // RenderThread, because past this point this thread owns the
    // position/velocity state below
    {
        std::lock_guard<std::mutex> lock(teleportMutex);
        if (pendingTeleport.has_value())
        {
            previousPosition = currentPosition = *pendingTeleport;
            state->player->SetPosition(currentPosition);
            grounded = false;
            accumulator = 0.0f;
            pendingTeleport.reset();
        }
    }

    DrainLocalMotion();

    bool inputAllowedNow = inputAllowed.load();

    // The jumpRequested edge case: a tap that lands in a call which runs
    // zero FixedStep()s must not be lost, so it latches here and stays true
    // until a FixedStep() actually consumes it (see jumpQueued's comment).
    // jumpHeldActive adds vanilla's repeat-jump-on-landing on top of that:
    // as long as Space is physically down, this re-latches every single
    // call regardless of whether the previous latch was already consumed -
    // so even though FixedStep() unconditionally clears jumpQueued after
    // checking it (discarding a jump attempt made while airborne), holding
    // the key means the very next call queues another attempt, and the
    // first FixedStep() where the player is grounded again actually jumps.
    // Both stay gated behind inputAllowedNow
    if (inputAllowedNow) {
        if (jumpRequested.exchange(false)) jumpQueued = true;
        if (jumpHeldActive.load()) jumpQueued = true;
    }

    accumulator += dt;
    if (accumulator > MAX_ACCUMULATED_TIME) accumulator = MAX_ACCUMULATED_TIME;

    while (accumulator >= FIXED_DT)
    {
        FixedStep(inputAllowedNow);
        StepViewBob();
        accumulator -= FIXED_DT;
    }

    // Publish the raw pair GetRenderPosition() mixes, plus the wall-clock
    // instant the last completed FixedStep() corresponds to (now minus
    // however far accumulator has drifted past it).
    {
        std::lock_guard<std::mutex> lock(renderPositionMutex);
        renderPreviousPosition = previousPosition;
        renderCurrentPosition = currentPosition;
        renderBobPrevious = bobPrevious;
        renderBobCurrent = bobCurrent;
        lastFixedStepTime = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(accumulator));
    }

    {
        std::lock_guard<std::mutex> lock(movementSnapshotMutex);
        movementSnapshot.sprinting = sprinting;
        movementSnapshot.grounded = grounded;
        movementSnapshot.mode = moveMode;
        movementSnapshot.swimAmount = swimAmount;
        movementSnapshot.eyeInWater = eyeInWater;
        movementSnapshot.fallDistance = fallDistance;
        movementSnapshot.intendedDirection = glm::vec2(lastHorizontalDir.x, lastHorizontalDir.z);
        movementSnapshot.horizontalSpeed = glm::length(glm::vec2(
            currentPosition.x - previousPosition.x, currentPosition.z - previousPosition.z));
    }
}

TickLoop::MovementSnapshot TickLoop::GetMovementSnapshot() const
{
    std::lock_guard<std::mutex> lock(movementSnapshotMutex);
    return movementSnapshot;
}

uint8_t TickLoop::GetMovementInputFlags() const
{
    if (!inputAllowed.load()) return 0;

    constexpr uint8_t FORWARD = 0x01, BACKWARD = 0x02, LEFT = 0x04, RIGHT = 0x08,
                      JUMP = 0x10, SHIFT = 0x20, SPRINT = 0x40;
    float forward = moveForwardInput.load();
    float rightInput = moveRightInput.load();

    uint8_t flags = 0;
    if (forward > 0.0f) flags |= FORWARD;
    if (forward < 0.0f) flags |= BACKWARD;
    if (rightInput < 0.0f) flags |= LEFT;
    if (rightInput > 0.0f) flags |= RIGHT;
    if (jumpHeldActive.load()) flags |= JUMP;
    if (sneakActive.load()) flags |= SHIFT;
    if (sprintActive.load()) flags |= SPRINT;
    return flags;
}

void TickLoop::DrainLocalMotion()
{
    std::vector<LocalMotionEvent> events;
    {
        std::lock_guard<std::mutex> lock(state->localMotion.mutex);
        events.swap(state->localMotion.events);
    }

    for (const LocalMotionEvent& event : events)
    {
        switch (event.kind)
        {
        case LocalMotionEvent::Kind::SetVelocity:
            velocity = event.vector;
            break;
        case LocalMotionEvent::Kind::AddVelocity:
            velocity += event.vector;
            break;
        case LocalMotionEvent::Kind::AttackSlowdown:
            velocity.x *= event.vector.x;
            velocity.z *= event.vector.x;
            sprinting = false;
            break;
        }
    }
}

void TickLoop::UpdateSprinting(bool inputAllowedNow, float forwardInput, bool sneaking)
{
    int32_t food;
    {
        std::lock_guard<std::mutex> lock(state->healthMutex);
        food = state->food;
    }

    bool canKeepSprinting = forwardInput > 0.0f && !sneaking && food > SPRINT_MIN_FOOD && !majorHorizontalCollision;
    if (sprinting)
    {
        if (!canKeepSprinting) sprinting = false;
    }
    else if (canKeepSprinting && inputAllowedNow && sprintActive.load())
    {
        sprinting = true;
    }
}

glm::vec3 TickLoop::GetRenderPosition() const
{
    // Computed fresh every call, not cached
    std::lock_guard<std::mutex> lock(renderPositionMutex);
    float alpha = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastFixedStepTime).count() / FIXED_DT;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return glm::mix(renderPreviousPosition, renderCurrentPosition, alpha);
}

float TickLoop::GetPartialTick() const
{
    std::lock_guard<std::mutex> lock(renderPositionMutex);
    float alpha = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastFixedStepTime).count() / FIXED_DT;
    return std::clamp(alpha, 0.0f, 1.0f);
}

TickLoop::ViewBob TickLoop::GetViewBob() const
{
    std::lock_guard<std::mutex> lock(renderPositionMutex);
    float alpha = std::chrono::duration<float>(std::chrono::steady_clock::now() - lastFixedStepTime).count() / FIXED_DT;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    ViewBob bob;
    bob.walkDistance = renderBobCurrent.walkDistance + (renderBobCurrent.walkDistance - renderBobPrevious.walkDistance) * alpha;
    bob.bob = glm::mix(renderBobPrevious.bob, renderBobCurrent.bob, alpha);
    return bob;
}

// Vanilla's Player: the walk grows by 0.6 of the distance covered, and the bob
// eases toward the speed (at most 0.1) on the ground, and toward 0 off it.
void TickLoop::StepViewBob()
{
    bobPrevious = bobCurrent;
    float moved = glm::length(glm::vec2(currentPosition.x - previousPosition.x, currentPosition.z - previousPosition.z));
    bobCurrent.walkDistance += moved * 0.6f;
    float target = grounded.load(std::memory_order_relaxed) ? std::min(0.1f, moved) : 0.0f;
    bobCurrent.bob += (target - bobCurrent.bob) * 0.4f;
}

void TickLoop::UpdateCrosshairProgress()
{
    float progress = -1.0f;
    int32_t required = state->miningTicksRequired.load(std::memory_order_relaxed);
    if (required > 0 && !state->canInstabuild.load())
    {
        int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        double elapsedTicks = static_cast<double>(nowNs - state->miningStartNs.load(std::memory_order_relaxed)) * 1e-9 * 20.0;
        progress = std::clamp(static_cast<float>(elapsedTicks / required), 0.0f, 1.0f);
    }
    else
    {
        float strength = state->interaction->GetAttackStrengthScale(0.0f);
        if (strength < 1.0f) progress = strength;
    }

    float current = state->crosshairProgressCurrent.load(std::memory_order_relaxed);
    state->crosshairProgressCurrent.store(progress, std::memory_order_relaxed);
    // Hidden last tick: snap in rather than sweeping from nothing.
    state->crosshairProgressPrevious.store(current < 0.0f ? progress : current, std::memory_order_relaxed);
}

void TickLoop::SyncToPlayerPosition()
{
    previousPosition = currentPosition = state->player->GetPosition();
    velocity = glm::vec3(0.0f);
    grounded = false;
    horizontalCollision = false;
    sprinting = false;
    fallDistance = 0.0f;
    horizontalCollisionAxes = 0;
    majorHorizontalCollision = false;
    moveMode = MoveMode::Walk;
    swimAmount = 0.0f;
    swimming = false;
    eyeInWater = false;
    collisionHeight = PLAYER_HEIGHT;
    swimStrideDistance = 0.0f;
    inWaterLast = false;
    eyeInWaterLast = false;
    if (state->blockSounds != nullptr) state->blockSounds->SetUnderwaterLoop(false);
    accumulator = 0.0f;
    lastTickTime = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(renderPositionMutex);
        renderPreviousPosition = renderCurrentPosition = currentPosition;
        lastFixedStepTime = lastTickTime;
    }
}

void TickLoop::QueueTeleport(glm::vec3 position)
{
    std::lock_guard<std::mutex> lock(teleportMutex);
    pendingTeleport = position;
}

void TickLoop::PlayStepSound()
{
    if (state->blockSounds == nullptr) return;

    // Probe slightly below the feet so standing exactly on a block's top
    // face still hits that block.
    Block block = state->world->GetBlock(
        static_cast<int>(std::floor(currentPosition.x)),
        static_cast<int>(std::floor(currentPosition.y - 0.2f)),
        static_cast<int>(std::floor(currentPosition.z)));
    state->blockSounds->PlayStep(block);
}

void TickLoop::UpdateWaterSounds(bool inWater, glm::vec3 enterVelocity)
{
    BlockSounds* sounds = state->blockSounds;
    if (sounds == nullptr) return;

    // Vanilla's splash scale; the entry velocity is the pre-move one.
    float speed = std::min(1.0f, std::sqrt(enterVelocity.x * enterVelocity.x * 0.2f
        + enterVelocity.y * enterVelocity.y + enterVelocity.z * enterVelocity.z * 0.2f) * 0.2f);

    if (inWater && !inWaterLast) sounds->PlaySplash(speed);
    else if (!inWater && inWaterLast) sounds->PlayWaterExit();

    if (eyeInWater && !eyeInWaterLast)
    {
        sounds->PlayUnderwaterEnter();
        sounds->SetUnderwaterLoop(true);
    }
    else if (!eyeInWater && eyeInWaterLast)
    {
        sounds->PlayUnderwaterExit();
        sounds->SetUnderwaterLoop(false);
    }

    // Stroke every SWIM_STRIDE_DISTANCE blocks travelled through water.
    if (inWater)
    {
        swimStrideDistance += glm::length(currentPosition - previousPosition);
        if (swimStrideDistance >= SWIM_STRIDE_DISTANCE)
        {
            swimStrideDistance = 0.0f;
            sounds->PlaySwimStroke(glm::length(velocity) * 1.75f);
        }
    }
    else
    {
        swimStrideDistance = 0.0f;
    }

    inWaterLast = inWater;
    eyeInWaterLast = eyeInWater;
}

void TickLoop::StepDroppedItems()
{
    constexpr float BOUNCE = -0.5f, GROUND_FRICTION = 0.6f, VOID_Y = -70.0f, MAX_SUBSTEP = 0.5f;
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(state->entitiesMutex);
    for (auto& [id, entity] : state->entities)
    {
        if (!entity.simulated || entity.position.y < VOID_Y) continue;
        const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
        if (info == nullptr) continue;
        MotionProfile profile = MotionFor(info->name);
        if (!profile.simulated) continue;

        auto cell = [&](const glm::vec3& at) {
            return state->world->GetBlock(static_cast<int>(std::floor(at.x)),
                static_cast<int>(std::floor(at.y)), static_cast<int>(std::floor(at.z)));
        };

        // Water, lava and the like move things their own way; leave those to the server.
        Block inside = cell(entity.position);
        if (!inside.isAir() && !inside.hasCollision()) continue;

        glm::vec3 position = entity.position;
        glm::vec3 velocity = entity.velocity;
        bool onGround = cell(position - glm::vec3(0.0f, 0.001f, 0.0f)).hasCollision();
        bool resting = velocity == glm::vec3(0.0f);
        if (profile.stopsOnBlocks && resting) continue; // Stuck in a block or the ground.
        if (profile.groundFriction && onGround && velocity.y <= 0.0f
            && glm::length(glm::vec2(velocity.x, velocity.z)) < 0.0001f) continue;

        velocity.y -= profile.gravity;

        // Move in sub-steps no longer than half a block so a fast arrow can't skip through a wall.
        int steps = std::max(1, static_cast<int>(std::ceil(glm::length(velocity) / MAX_SUBSTEP)));
        glm::vec3 stepVelocity = velocity / static_cast<float>(steps);
        onGround = false;
        bool hit = false;
        for (int step = 0; step < steps && !hit; step++)
        {
            glm::vec3 next = position + glm::vec3(stepVelocity.x, 0.0f, 0.0f);
            if (cell(next).hasCollision()) { hit = true; velocity.x = 0.0f; stepVelocity.x = 0.0f; } else position.x = next.x;
            next = position + glm::vec3(0.0f, 0.0f, stepVelocity.z);
            if (cell(next).hasCollision()) { hit = true; velocity.z = 0.0f; stepVelocity.z = 0.0f; } else position.z = next.z;

            next = position + glm::vec3(0.0f, stepVelocity.y, 0.0f);
            if (cell(next).hasCollision())
            {
                hit = true;
                if (stepVelocity.y < 0.0f)
                {
                    position.y = std::floor(next.y) + 1.0f;
                    onGround = true;
                }
                velocity.y = 0.0f;
                stepVelocity.y = 0.0f;
            }
            else
            {
                position.y = next.y;
            }
        }

        if (profile.stopsOnBlocks && hit)
        {
            velocity = glm::vec3(0.0f); // Stuck where it landed.
        }
        else
        {
            float friction = profile.groundFriction && onGround ? GROUND_FRICTION * profile.drag : profile.drag;
            velocity.x *= friction;
            velocity.z *= friction;
            velocity.y *= profile.drag;
            if (profile.groundFriction && onGround && velocity.y < 0.0f) velocity.y *= BOUNCE;
        }

        entity.previousPosition = entity.position;
        entity.position = position;
        entity.velocity = velocity;
        entity.interpSeconds = 1.0f / 20.0f;
        entity.lastUpdateTime = now;
    }
}

void TickLoop::FixedStep(bool inputAllowedNow)
{
    state->clientTicks.fetch_add(1, std::memory_order_relaxed);
    if (state->modLoader != nullptr) state->modLoader->Fire(EventType::TICK);
    StepDroppedItems();
    previousPosition = currentPosition;
    horizontalCollision = false; // Re-derived from this step's own MoveAxis calls below.

    // Riding: the vehicle owns movement; the player just sits at the seat.
    if (state->ridingEntityId.load() >= 0 && state->rideController != nullptr)
    {
        RideController::Input rideInput;
        rideInput.forward = inputAllowedNow ? moveForwardInput.load() : 0.0f;
        rideInput.right = inputAllowedNow ? moveRightInput.load() : 0.0f;
        rideInput.jumpHeld = inputAllowedNow && jumpHeldActive.load();
        rideInput.inputAllowed = inputAllowedNow;
        RideController::Result ride = state->rideController->Step(rideInput);
        if (ride.riding)
        {
            currentPosition = ride.playerPosition;
            velocity = glm::vec3(0.0f);
            grounded = false;
            sprinting = false;
            swimming = false;
            fallDistance = 0.0f;
            collisionHeight = PLAYER_HEIGHT;
            moveMode = MoveMode::Ride;
            swimAmount = 0.0f;
            state->player->camera.ApplyMouseDelta(ride.yawDelta, 0.0f, 1.0f);
            state->player->SetPosition(currentPosition);
            return;
        }
    }

    // See GlobalState::attackStrengthTicker's own comment
    int32_t mainhandItemId;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        mainhandItemId = state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot).itemId;
    }
    if (mainhandItemId != lastMainhandItemId)
    {
        lastMainhandItemId = mainhandItemId;
        state->attackStrengthTicker.store(0, std::memory_order_relaxed);
    }
    state->attackStrengthTicker.fetch_add(1, std::memory_order_relaxed);
    UpdateCrosshairProgress();

    // Horizontal movement follows vanilla's real per-tick model: accelerate
    // toward the input direction, then apply momentum-preserving friction,
    // rather than snapping straight to a target speed
    float forwardInput = inputAllowedNow ? moveForwardInput.load() : 0.0f;
    float rightInput = inputAllowedNow ? moveRightInput.load() : 0.0f;

    float yaw = glm::radians(state->player->camera.GetYaw());
    glm::vec3 forward{ sin(yaw), 0.0f, cos(yaw) };
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::vec3 horizontalDir = forward * forwardInput + right * rightInput;
    if (glm::length(horizontalDir) > 1.0f) horizontalDir = glm::normalize(horizontalDir);

    constexpr float GROUND_FRICTION = 0.546f; // default block slipperiness (0.6) * 0.91
    constexpr float AIR_FRICTION = 0.91f;
    constexpr float AIR_ACCELERATION = 0.02f; // flat, unlike ground accel below

    // Sprint/sneak speed: vanilla applies these as attribute modifiers, but
    // here it's simplest to just scale the speed used below directly.
    // Sprinting itself is real state, not just "the key is held"
    bool sneaking = sneakActive.load();
    UpdateSprinting(inputAllowedNow, forwardInput, sneaking);
    // Using an item (eating, blocking, ...) ends the sprint and slows to 0.2x.
    bool usingItem = state->usingItem.load(std::memory_order_relaxed);
    if (usingItem) sprinting = false;
    float speedMultiplier = sneaking ? 0.3f : (sprinting ? 1.3f : 1.0f);
    if (usingItem) speedMultiplier *= 0.2f;

    // Fluid state at the start of the step. A mode == Ride step would skip
    // all of the walk and water physics below (the vehicle code owns it).
    FluidQuery fluids(state->world);
    float currentEyeHeightNow = state->player->camera.eyeHeight.load(std::memory_order_relaxed);
    float submerged = fluids.SubmergedFraction(currentPosition, collisionHeight);
    bool inWater = submerged > 0.0f;
    eyeInWater = fluids.Contains(currentPosition + glm::vec3(0.0f, currentEyeHeightNow, 0.0f));
    float waterDepth = submerged * collisionHeight;
    constexpr float WADE_DEPTH = 0.4f; // Vanilla's fluid jump threshold: shallower than this you still walk-jump.
    bool wading = grounded && waterDepth <= WADE_DEPTH;

    // Vanilla's isSwimming: starts submerged while sprinting, lasts while in water and sprinting.
    bool wasSwimming = swimming;
    swimming = sprinting && inWater && (swimming || submerged >= 0.5f);
    if (wasSwimming && !swimming && !inWater && !grounded) sprinting = false;
    if (inWater) fallDistance = 0.0f;

    float movementSpeed = static_cast<float>(state->attributes->GetDouble("generic.movement_speed", 0.1)) * speedMultiplier;
    float friction = grounded ? GROUND_FRICTION : AIR_FRICTION;
    // *0.98 reproduces vanilla's verified terminal walking speed (0.2159
    // blocks/tick) from the default 0.1 attribute value.
    float acceleration = inWater ? WATER_ACCELERATION : (grounded ? (movementSpeed * 0.98f) : AIR_ACCELERATION);

    // Crawl box while sprint-swimming; stand back up only with headroom.
    if (swimming) collisionHeight = SWIM_BOX_HEIGHT;
    else if (collisionHeight < PLAYER_HEIGHT && !IntersectsCollision(currentPosition, PLAYER_HEIGHT)) collisionHeight = PLAYER_HEIGHT;

    moveMode = swimming ? MoveMode::SwimSprint : ((inWater && !wading) ? MoveMode::Swim : MoveMode::Walk);
    float swimTarget = (moveMode == MoveMode::Swim || moveMode == MoveMode::SwimSprint) ? 1.0f : 0.0f;
    swimAmount += std::clamp(swimTarget - swimAmount, -SWIM_AMOUNT_STEP, SWIM_AMOUNT_STEP);

    if (inWater && inputAllowedNow)
    {
        bool jumpHeld = jumpHeldActive.load();
        if (swimming)
        {
            // Pitch steers vertical speed toward the look direction.
            float lookY = std::sin(glm::radians(state->player->camera.GetPitch()));
            float scale = lookY < -0.2f ? 0.085f : 0.06f;
            if (lookY <= 0.0f || jumpHeld || fluids.At(static_cast<int>(std::floor(currentPosition.x)),
                static_cast<int>(std::floor(currentPosition.y)), static_cast<int>(std::floor(currentPosition.z))).kind != FluidQuery::NONE)
            {
                velocity.y += (lookY - velocity.y) * scale;
            }
        }
        if (jumpHeld && !wading) velocity.y += WATER_VERTICAL_PUSH;
        if (sneaking) velocity.y -= WATER_VERTICAL_PUSH;
    }

    // Vanilla's own order (LivingEntity.travel): accelerate, move, THEN
    // apply friction. For input-driven walking this gives exactly the same
    // per-tick displacement as friction-first would; what it changes is
    // velocity injected from outside (DrainLocalMotion - knockback,
    // explosions), which now moves the player by its full amount on the
    // first tick instead of being cut by friction before it ever displaces
    // anything, the same way vanilla handles a Set Entity Motion packet.
    velocity.x += horizontalDir.x * acceleration;
    velocity.z += horizontalDir.z * acceleration;
    lastHorizontalDir = horizontalDir;
    LogIfNotFinite("velocity after friction/acceleration", velocity, velocity, grounded);

    // Jump is an instant velocity set, exactly like vanilla's
    // jumpFromGround(). It has to land before this tick's move below, not
    // before gravity/drag, or the jump gets decayed before it ever displaces
    // anything
    if (jumpQueued)
    {
        if (grounded)
        {
            float jumpVelocity = static_cast<float>(state->attributes->GetDouble("generic.jump_strength", 0.42));
            velocity.y = jumpVelocity;
            // Sprint-jumping adds a forward push on top
            if (sprinting)
            {
                velocity.x += forward.x * SPRINT_JUMP_BOOST;
                velocity.z += forward.z * SPRINT_JUMP_BOOST;
            }
            grounded = false;
        }
        jumpQueued = false;
    }

    // velocity is in blocks/tick (matching vanilla) and FIXED_DT is exactly
    // one Minecraft tick, so each component IS this tick's displacement -
    // no further dt scaling needed.
    glm::vec3 pos = currentPosition;
    const float startY = pos.y;
    const glm::vec3 enterVelocity = velocity;
    horizontalCollisionAxes = 0;
    majorHorizontalCollision = false;
    MoveAxis(pos, velocity, 1, velocity.y); // Y first: settles grounded state before horizontal collision.
    MoveAxis(pos, velocity, 0, velocity.x);
    MoveAxis(pos, velocity, 2, velocity.z);

    // Climb-out hop: pushed against a wall with open air 0.6 up means the surface is near.
    if (inWater && horizontalCollision.load(std::memory_order_relaxed)
        && !IntersectsCollision(glm::vec3(pos.x + velocity.x, startY + velocity.y + 0.6f, pos.z + velocity.z), collisionHeight)
        && fluids.SubmergedFraction(glm::vec3(pos.x + velocity.x, startY + velocity.y + 0.6f, pos.z + velocity.z), collisionHeight) == 0.0f)
    {
        velocity.y = WATER_CLIMB_OUT_SPEED;
    }

    currentPosition = pos;
    LogIfNotFinite("position after MoveAxis", currentPosition, velocity, grounded);
    state->player->SetPosition(currentPosition);

    // Footsteps: every 8 ticks while walking on the ground, 1.3x as often
    // while sprinting. Sneaking is silent, as in vanilla.
    constexpr int WALK_STEP_INTERVAL_TICKS = 8;
    const int STEP_SOUND_INTERVAL_TICKS = sprinting
        ? static_cast<int>(std::lround(WALK_STEP_INTERVAL_TICKS / 1.3f)) : WALK_STEP_INTERVAL_TICKS;
    constexpr float STEP_MIN_DISTANCE = 0.1f; // Blocks moved horizontally this tick.
    ticksSinceStepSound++;
    float movedHorizontally = glm::length(glm::vec2(
        currentPosition.x - previousPosition.x, currentPosition.z - previousPosition.z));
    if (grounded && !inWater && !sneaking && movedHorizontally > STEP_MIN_DISTANCE
        && ticksSinceStepSound >= STEP_SOUND_INTERVAL_TICKS)
    {
        ticksSinceStepSound = 0;
        PlayStepSound();
    }

    UpdateWaterSounds(inWater, enterVelocity);

    float gravity = static_cast<float>(state->attributes->GetDouble("generic.gravity", 0.08));
    if (inWater)
    {
        // Vanilla's travelInWater: drag after the move, then a slow sink.
        float drag = sprinting ? SWIM_SPRINT_DRAG : WATER_DRAG;
        velocity.x *= drag;
        velocity.z *= drag;
        velocity.y *= WATER_DRAG;
        if (gravity != 0.0f && !sprinting)
        {
            float sink = gravity / 16.0f;
            if (velocity.y <= 0.0f && std::fabs(velocity.y - 0.005f) >= 0.003f && std::fabs(velocity.y - sink) < 0.003f)
                velocity.y = -0.003f;
            else
                velocity.y -= sink;
        }
    }
    else
    {
        velocity.x *= friction;
        velocity.z *= friction;
    }

    // Fall distance, as vanilla's Entity.checkFallDamage tracks it: every
    // block moved downward while airborne adds up, landing resets it. A
    // critical hit needs this above 0 (see MovementSnapshot).
    if (grounded || inWater) fallDistance = 0.0f;
    else if (currentPosition.y < previousPosition.y) fallDistance += previousPosition.y - currentPosition.y;

    // Scope-limited sneak: just lower the camera, don't touch the collision
    // AABB. Eased toward the target height each tick (see SNEAK_EYE_HEIGHT's
    // own comment) rather than snapped, so the dip reads as a smooth crouch
    // instead of a jump cut.
    // Dying: the view drops to the ground, as vanilla's 0.2-block dying pose does.
    bool dead = false;
    {
        std::lock_guard<std::mutex> lock(state->healthMutex);
        dead = state->health <= 0.0f;
    }
    float targetEyeHeight = dead ? DEAD_EYE_HEIGHT
        : collisionHeight < PLAYER_HEIGHT ? SWIM_EYE_HEIGHT : (sneaking ? SNEAK_EYE_HEIGHT : STANDING_EYE_HEIGHT);
    std::atomic<float>& eyeHeight = state->player->camera.eyeHeight;
    float currentEyeHeight = eyeHeight.load(std::memory_order_relaxed);
    eyeHeight.store(Math::Damp(currentEyeHeight, targetEyeHeight, EYE_HEIGHT_EASE_RATE, FIXED_DT), std::memory_order_relaxed);

    // Gravity + drag apply every tick regardless of grounded state, but only
    // AFTER this tick's move
    constexpr float VERTICAL_DRAG = 0.98f;
    if (!inWater) velocity.y = (velocity.y - gravity) * VERTICAL_DRAG;
    if (std::fabs(velocity.y) < 0.005f && !inWater) velocity.y = 0.0f;
    LogIfNotFinite("velocity after gravity", velocity, velocity, grounded);
}

// Mirrors vanilla's Entity.collide / Shapes.collide: each axis is swept, not
// tested for overlap after the fact. Starting from the full requested
// delta, every collision box that overlaps the player's AABB on the other
// two axes (strictly, no epsilon) and lies ahead along `axis` clamps the move
// to the gap in front of it. Boxes the player already overlaps along `axis`
// are ignored (so a player inside a block can walk out), and gaps under
// 1e-7 count as touching, same as vanilla. Done in double so a float ulp at
// large coordinates can't read as penetration.
double TickLoop::SweepAxis(glm::vec3 position, int axis, double delta) const
{
    constexpr double TOUCH = 1e-7;
    if (delta == 0.0) return 0.0;

    const double extentMin[3] = {
        static_cast<double>(position.x) - PLAYER_HALF_WIDTH,
        static_cast<double>(position.y),
        static_cast<double>(position.z) - PLAYER_HALF_WIDTH };
    const double extentMax[3] = {
        static_cast<double>(position.x) + PLAYER_HALF_WIDTH,
        static_cast<double>(position.y) + collisionHeight,
        static_cast<double>(position.z) + PLAYER_HALF_WIDTH };

    int lo[3], hi[3];
    for (int i = 0; i < 3; i++)
    {
        double a = extentMin[i], b = extentMax[i];
        if (i == axis)
        {
            a = std::min(a, a + delta);
            b = std::max(b, b + delta);
        }
        lo[i] = static_cast<int>(std::floor(a));
        hi[i] = static_cast<int>(std::floor(b));
    }
    lo[1] -= 1; // A collision box can reach up to 0.5 into the cell above its own (fences, walls).

    const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;

    for (int by = lo[1]; by <= hi[1]; by++) {
        for (int bz = lo[2]; bz <= hi[2]; bz++) {
            for (int bx = lo[0]; bx <= hi[0]; bx++) {
                Block block = state->world->GetBlock(bx, by, bz);
                BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);
                if (shape.count == 0) continue;

                const double origin[3] = { static_cast<double>(bx), static_cast<double>(by), static_cast<double>(bz) };
                for (int i = 0; i < shape.count; i++)
                {
                    const BlockRegistry::AABB& box = shape.boxes[static_cast<size_t>(i)];
                    double boxMin[3], boxMax[3];
                    for (int k = 0; k < 3; k++)
                    {
                        boxMin[k] = origin[k] + box.min[k];
                        boxMax[k] = origin[k] + box.max[k];
                    }

                    if (!(extentMin[o1] < boxMax[o1] && extentMax[o1] > boxMin[o1])) continue;
                    if (!(extentMin[o2] < boxMax[o2] && extentMax[o2] > boxMin[o2])) continue;

                    if (delta > 0.0 && extentMax[axis] <= boxMin[axis] + TOUCH)
                        delta = std::min(delta, std::max(boxMin[axis] - extentMax[axis], 0.0));
                    else if (delta < 0.0 && extentMin[axis] >= boxMax[axis] - TOUCH)
                        delta = std::max(delta, std::min(boxMax[axis] - extentMin[axis], 0.0));
                }
            }
        }
    }

    return delta;
}

bool TickLoop::IntersectsCollision(glm::vec3 position, float height) const
{
    const double boxMin[3] = { position.x - PLAYER_HALF_WIDTH, position.y, position.z - PLAYER_HALF_WIDTH };
    const double boxMax[3] = { position.x + PLAYER_HALF_WIDTH, position.y + height, position.z + PLAYER_HALF_WIDTH };

    for (int by = static_cast<int>(std::floor(boxMin[1])) - 1; by <= static_cast<int>(std::floor(boxMax[1])); by++) {
        for (int bz = static_cast<int>(std::floor(boxMin[2])); bz <= static_cast<int>(std::floor(boxMax[2])); bz++) {
            for (int bx = static_cast<int>(std::floor(boxMin[0])); bx <= static_cast<int>(std::floor(boxMax[0])); bx++) {
                BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(state->world->GetBlock(bx, by, bz));
                const double origin[3] = { static_cast<double>(bx), static_cast<double>(by), static_cast<double>(bz) };
                for (int i = 0; i < shape.count; i++)
                {
                    const BlockRegistry::AABB& box = shape.boxes[static_cast<size_t>(i)];
                    bool overlaps = true;
                    for (int k = 0; k < 3 && overlaps; k++)
                    {
                        overlaps = boxMin[k] < origin[k] + box.max[k] && boxMax[k] > origin[k] + box.min[k];
                    }
                    if (overlaps) return true;
                }
            }
        }
    }
    return false;
}

// Positions are stored as floats, but SweepAxis resolves in double. Rounding
// to nearest can land one ulp past the contact surface, which the next sweep
// would then read as already inside the block. Round toward the side the
// player came from instead.
float TickLoop::RoundTowardOrigin(double target, bool movingPositive)
{
    // The step direction is a large finite float, not infinity: this project
    // builds with -ffast-math (-ffinite-math-only), under which an infinity
    // operand is undefined behavior and the optimizer may drop the nudge -
    // leaving the player an ulp (~1e-4 at x~1400) inside the block face,
    // which the server reads as being stuck in it.
    float f = static_cast<float>(target);
    if (movingPositive && static_cast<double>(f) > target)
        f = std::nextafter(f, std::numeric_limits<float>::lowest());
    else if (!movingPositive && static_cast<double>(f) < target)
        f = std::nextafter(f, std::numeric_limits<float>::max());
    return f;
}

void TickLoop::MoveAxis(glm::vec3& position, glm::vec3& vel, int axis, float delta)
{
    if (delta == 0.0f) return;

    const double requested = delta;
    const double allowed = SweepAxis(position, axis, requested);
    const bool blocked = allowed != requested;

    if (axis == 1)
    {
        position.y = RoundTowardOrigin(static_cast<double>(position.y) + allowed, delta > 0.0f);
        if (blocked)
        {
            if (delta < 0.0f) grounded = true; // Vanilla: onGround = moving down and got stopped.
            vel.y = 0.0f;
        }
        else
        {
            grounded = false; // Any real vertical move leaves the ground (falling, knockback, ...).
        }
        return;
    }

    glm::vec3 result = position;
    result[axis] = RoundTowardOrigin(static_cast<double>(position[axis]) + allowed, delta > 0.0f);
    double moved = allowed;

    if (blocked && grounded)
    {
        // Vanilla's step-up: raise by STEP_HEIGHT (as far as headroom
        // allows), make the same horizontal move there, then drop back down
        // onto whatever was stepped on. Kept only if it got further than
        // the flat attempt.
        const double up = SweepAxis(position, 1, STEP_HEIGHT);
        glm::vec3 stepped = position;
        stepped.y = RoundTowardOrigin(static_cast<double>(position.y) + up, true);

        const double steppedAllowed = SweepAxis(stepped, axis, requested);
        if (std::fabs(steppedAllowed) > std::fabs(allowed))
        {
            stepped[axis] = RoundTowardOrigin(static_cast<double>(position[axis]) + steppedAllowed, delta > 0.0f);
            const double down = SweepAxis(stepped, 1, -static_cast<double>(stepped.y - position.y));
            stepped.y = RoundTowardOrigin(static_cast<double>(stepped.y) + down, false);
            result = stepped;
            moved = steppedAllowed;
        }
    }

    position = result;

    if (moved != requested)
    {
        vel[axis] = 0.0f;
        horizontalCollision = true; // MovementFlags bit 1
        horizontalCollisionAxes |= static_cast<uint8_t>(1u << axis);
        if (std::fabs(lastHorizontalDir[axis]) >= MINOR_COLLISION_SIN) majorHorizontalCollision = true;
    }
}

} // namespace Volcano
