#include "TickLoop.hpp"
#include "Logger.hpp"
#include "interaction/InteractionManager.hpp"
#include "audio/SoundRegistry.hpp"
#include "renderer/terrain/models/BlockRegistry.hpp"
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
    // breakpoint, alt-tab, a slow load) — clamp how much time can accumulate so
    // a stall causes a pause, not a burst of catch-up ticks.
    constexpr float MAX_ACCUMULATED_TIME = 0.25f;
    // Vanilla's own step height: obstructions this tall or shorter (slabs,
    // snow layers/carpets, a staircase's first step) are auto-climbed on a
    // horizontal move instead of blocking it — see MoveAxis/TryStepUp.
    constexpr float STEP_HEIGHT = 0.6f;

    // Sneak eye-height dip — half of vanilla's real drop (1.62 standing to 1.27
    // sneaking, a 0.35 dip), since a more subtle dip reads better
    // once eased in over time instead of snapped.
    constexpr float STANDING_EYE_HEIGHT = 1.62f;
    constexpr float SNEAK_EYE_HEIGHT = STANDING_EYE_HEIGHT - (1.62f - 1.27f) * 0.5f;
    // Exponential ease rate applied per-tick below — higher = snappier. Framerate
    // (tick-rate) independent the same way RenderThread's FOV easing is.
    constexpr float EYE_HEIGHT_EASE_RATE = 10.0f;

    bool IsFinite(glm::vec3 v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // A vanilla server disconnects with "Invalid move player packet
    // received" the instant a reported position stops being finite (see
    // ServerGamePacketListenerImpl's own move-player check) — this used to
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
    // intended movement — sin(8 degrees), vanilla LocalPlayer's own "minor
    // horizontal collision" angle.
    constexpr float MINOR_COLLISION_SIN = 0.1392f;
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
    // on — otherwise gravity runs from frame one against an empty World
    // (every lookup reads Air) and the player free-falls before any chunk
    // has had a chance to load. See GlobalState::worldReady for the
    // ordering guarantee that makes reading cached position state below
    // safe once this becomes true.
    //
    // Must come before the lastTickTime read/write below — MeshingThread
    // also writes lastTickTime (via SyncToPlayerPosition), and its right to
    // do so rests on worldReady still being false at that moment.
    if (!state->worldReady.load()) return;

    // Self-timed rather than fed a frame delta: measuring elapsed wall time
    // here keeps this correct even across an unusually long frame.
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastTickTime).count();
    lastTickTime = now;

    // Apply any teleport the server sent since the last call. Done here, on
    // RenderThread, because past this point this thread owns the
    // position/velocity state below — see QueueTeleport's comment.
    //
    // Velocity is deliberately left alone here. SynchronizePlayerPosition
    // (see its handler's comment: "also every later teleport") is the same
    // packet vanilla servers send both for an actual teleport and for a
    // routine mid-flight resync — there's no way to tell those apart from
    // the packet alone, and the server never includes a velocity to
    // substitute in. Zeroing it unconditionally used to reset the player's
    // fall speed to 0 whenever a routine resync happened to land mid-fall,
    // which is most of the time a fall lasts long enough for one to arrive
    // — that was this bug's actual cause, not anything physics-related.
    // Vanilla's own client carries velocity across this packet too; grounded
    // still resets, since the new position's ground contact is unknown.
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
    // call regardless of whether the previous latch was already consumed —
    // so even though FixedStep() unconditionally clears jumpQueued after
    // checking it (discarding a jump attempt made while airborne), holding
    // the key means the very next call queues another attempt, and the
    // first FixedStep() where the player is grounded again actually jumps.
    // Both stay gated behind inputAllowedNow — a screen/chat box being open
    // must not also make the player jump underneath it.
    if (inputAllowedNow) {
        if (jumpRequested.exchange(false)) jumpQueued = true;
        if (jumpHeldActive.load()) jumpQueued = true;
    }

    accumulator += dt;
    if (accumulator > MAX_ACCUMULATED_TIME) accumulator = MAX_ACCUMULATED_TIME;

    while (accumulator >= FIXED_DT)
    {
        FixedStep(inputAllowedNow);
        accumulator -= FIXED_DT;
    }

    // Publish the raw pair GetRenderPosition() mixes, plus the wall-clock
    // instant the last completed FixedStep() corresponds to (now minus
    // however far accumulator has drifted past it).
    {
        std::lock_guard<std::mutex> lock(renderPositionMutex);
        renderPreviousPosition = previousPosition;
        renderCurrentPosition = currentPosition;
        lastFixedStepTime = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(accumulator));
    }

    {
        std::lock_guard<std::mutex> lock(movementSnapshotMutex);
        movementSnapshot.sprinting = sprinting;
        movementSnapshot.grounded = grounded;
        movementSnapshot.fallDistance = fallDistance;
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
    // Computed fresh every call, not cached — FixedStep() only advances at a
    // fixed 20Hz, so a cached mix would sit frozen then visibly snap.
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

// Footstep material for a block, guessed from its top texture's name —
// the client has no block sound-type table, only textures.
static std::string StepMaterialForTexture(const std::string& texture)
{
    auto has = [&](const char* part) { return texture.find(part) != std::string::npos; };

    if (has("sandstone")) return "stone";
    if (has("sand")) return "sand";
    if (has("gravel") || has("dirt") || has("coarse") || has("podzol")) return "gravel";
    if (has("grass") || has("moss") || has("leaves") || has("fern") || has("mycelium")) return "grass";
    if (has("snow")) return "snow";
    if (has("wool") || has("carpet")) return "cloth";
    if (has("coral")) return "coral";
    if (has("log") || has("planks") || has("wood") || has("stem") || has("hyphae") || has("bamboo")
        || has("crafting") || has("bookshelf") || has("barrel")) return "wood";
    return "stone";
}

void TickLoop::PlayStepSound()
{
    if (state->soundRegistry == nullptr || state->audioManager == nullptr) return;

    // Probe slightly below the feet so standing exactly on a block's top
    // face still hits that block.
    Block block = state->world->GetBlock(
        static_cast<int>(std::floor(currentPosition.x)),
        static_cast<int>(std::floor(currentPosition.y - 0.2f)),
        static_cast<int>(std::floor(currentPosition.z)));
    if (block.isAir()) return;

    std::string texture;
    if (block.visualId != 0)
    {
        texture = BlockRegistry::GetFaceTextureName(block.visualId, 0);
    }
    else
    {
        const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
        if (!visual.elements.empty()) texture = visual.elements[0].faceTextures[0];
        else texture = visual.crossTexture;
    }

    // Variants are step/<material>1.ogg, 2, 3, ... — probe for how many exist.
    std::string base = "minecraft:step/" + StepMaterialForTexture(texture);
    std::vector<std::string> variants;
    for (int i = 1; i <= 8; i++)
    {
        std::string name = base + std::to_string(i);
        if (state->soundRegistry->Has(name)) variants.push_back(name);
    }
    if (variants.empty()) return;

    static thread_local std::mt19937 gen{std::random_device{}()};
    std::string sound = variants[std::uniform_int_distribution<size_t>(0, variants.size() - 1)(gen)];
    float pitch = std::uniform_real_distribution<float>(0.9f, 1.1f)(gen);
    float masterVolume = static_cast<float>(std::get<uint32_t>(state->config->Get("Audio.Volume", 100u))) / 100.0f;
    state->soundRegistry->Play(state->audioManager, sound, 0.3f * masterVolume, pitch);
}

void TickLoop::FixedStep(bool inputAllowedNow)
{
    previousPosition = currentPosition;
    horizontalCollision = false; // Re-derived from this step's own MoveAxis calls below.

    // See GlobalState::attackStrengthTicker's own comment — reset by
    // InteractionManager::PrimaryTrigger on a swing, incremented here once
    // per real fixed tick regardless of input, same as vanilla's own
    // per-tick ticker.
    state->attackStrengthTicker.fetch_add(1, std::memory_order_relaxed);
    UpdateCrosshairProgress();

    // Horizontal movement follows vanilla's real per-tick model: accelerate
    // toward the input direction, then apply momentum-preserving friction,
    // rather than snapping straight to a target speed — this is what gives
    // vanilla its short ramp-up/slide-to-a-stop feel instead of an instant
    // start/stop. All constants below are vanilla's own (see
    // PlayerAttributes::Defaults() for the attribute defaults they read):
    // default block slipperiness (0.6) * 0.91 gives the well-known 0.546
    // ground friction, which combined with the 0.098 blocks/tick ground
    // acceleration converges to vanilla's documented terminal speed of
    // 0.2159 blocks/tick (4.317 blocks/sec). Airborne friction/acceleration
    // (0.91 / 0.02) are vanilla's own too — much less momentum lost per
    // tick, and a much smaller push from input, which is exactly why
    // vanilla air control feels so limited compared to ground movement.
    float forwardInput = inputAllowedNow ? moveForwardInput.load() : 0.0f;
    float rightInput = inputAllowedNow ? moveRightInput.load() : 0.0f;

    float yaw = glm::radians(state->player->camera.GetYaw());
    glm::vec3 forward{ sin(yaw), 0.0f, cos(yaw) };
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::vec3 horizontalDir = forward * forwardInput + right * rightInput;
    if (glm::length(horizontalDir) > 1.0f) horizontalDir = glm::normalize(horizontalDir);

    constexpr float GROUND_FRICTION = 0.546f; // default block slipperiness (0.6) * 0.91
    constexpr float AIR_FRICTION = 0.91f;
    constexpr float AIR_ACCELERATION = 0.02f; // flat, unlike ground accel below — not attribute-scaled in vanilla either

    // Sprint/sneak speed: vanilla applies these as attribute modifiers, but
    // here it's simplest to just scale the speed used below directly.
    // Sprinting itself is real state, not just "the key is held" — see
    // UpdateSprinting — and it already excludes sneaking.
    bool sneaking = sneakActive.load();
    UpdateSprinting(inputAllowedNow, forwardInput, sneaking);
    float speedMultiplier = sneaking ? 0.3f : (sprinting ? 1.3f : 1.0f);

    float movementSpeed = static_cast<float>(state->attributes->GetDouble("generic.movement_speed", 0.1)) * speedMultiplier;
    float friction = grounded ? GROUND_FRICTION : AIR_FRICTION;
    // *0.98 reproduces vanilla's verified terminal walking speed (0.2159
    // blocks/tick) from the default 0.1 attribute value.
    float acceleration = grounded ? (movementSpeed * 0.98f) : AIR_ACCELERATION;

    // Vanilla's own order (LivingEntity.travel): accelerate, move, THEN
    // apply friction. For input-driven walking this gives exactly the same
    // per-tick displacement as friction-first would; what it changes is
    // velocity injected from outside (DrainLocalMotion — knockback,
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
    // anything — vanilla moves with the raw 0.42 on the jump tick itself and
    // only decays velocity afterward, in prep for next tick.
    if (jumpQueued)
    {
        if (grounded)
        {
            float jumpVelocity = static_cast<float>(state->attributes->GetDouble("generic.jump_strength", 0.42));
            velocity.y = jumpVelocity;
            // Sprint-jumping adds a forward push on top — what makes it
            // faster than sprinting on foot.
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
    // one Minecraft tick, so each component IS this tick's displacement —
    // no further dt scaling needed.
    glm::vec3 pos = currentPosition;
    horizontalCollisionAxes = 0;
    majorHorizontalCollision = false;
    MoveAxis(pos, velocity, 1, velocity.y); // Y first: settles grounded state before horizontal collision.
    MoveAxis(pos, velocity, 0, velocity.x);
    MoveAxis(pos, velocity, 2, velocity.z);

    currentPosition = pos;
    LogIfNotFinite("position after MoveAxis", currentPosition, velocity, grounded);
    state->player->SetPosition(currentPosition);

    // Footsteps: every 10 ticks (500ms) while walking on the ground. Sneaking
    // is silent, as in vanilla.
    constexpr int STEP_SOUND_INTERVAL_TICKS = 10;
    constexpr float STEP_MIN_DISTANCE = 0.1f; // Blocks moved horizontally this tick.
    ticksSinceStepSound++;
    float movedHorizontally = glm::length(glm::vec2(
        currentPosition.x - previousPosition.x, currentPosition.z - previousPosition.z));
    if (grounded && !sneaking && movedHorizontally > STEP_MIN_DISTANCE
        && ticksSinceStepSound >= STEP_SOUND_INTERVAL_TICKS)
    {
        ticksSinceStepSound = 0;
        PlayStepSound();
    }

    velocity.x *= friction;
    velocity.z *= friction;

    // Fall distance, as vanilla's Entity.checkFallDamage tracks it: every
    // block moved downward while airborne adds up, landing resets it. A
    // critical hit needs this above 0 (see MovementSnapshot).
    if (grounded) fallDistance = 0.0f;
    else if (currentPosition.y < previousPosition.y) fallDistance += previousPosition.y - currentPosition.y;

    // Scope-limited sneak: just lower the camera, don't touch the collision
    // AABB. Eased toward the target height each tick (see SNEAK_EYE_HEIGHT's
    // own comment) rather than snapped, so the dip reads as a smooth crouch
    // instead of a jump cut.
    float targetEyeHeight = sneaking ? SNEAK_EYE_HEIGHT : STANDING_EYE_HEIGHT;
    std::atomic<float>& eyeHeight = state->player->camera.eyeHeight;
    float currentEyeHeight = eyeHeight.load(std::memory_order_relaxed);
    eyeHeight.store(
        currentEyeHeight + (targetEyeHeight - currentEyeHeight) * (1.0f - std::exp(-EYE_HEIGHT_EASE_RATE * FIXED_DT)),
        std::memory_order_relaxed);

    // Gravity + drag apply every tick regardless of grounded state, but only
    // AFTER this tick's move — resting on the ground works because the
    // resulting downward move next tick gets collided and zeroed back out,
    // not because gravity itself stops. Applying this before the move (as a
    // previous version did) decays the jump before it ever moves anything,
    // shaving well over a third off the actual jump height. Constants and
    // the sub-0.005 snap-to-zero are vanilla's own per-tick formula:
    // velocity.y = (velocity.y - gravity) * 0.98.
    float gravity = static_cast<float>(state->attributes->GetDouble("generic.gravity", 0.08));
    constexpr float VERTICAL_DRAG = 0.98f;
    velocity.y = (velocity.y - gravity) * VERTICAL_DRAG;
    if (std::fabs(velocity.y) < 0.005f) velocity.y = 0.0f;
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
        static_cast<double>(position.y) + PLAYER_HEIGHT,
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

// Positions are stored as floats, but SweepAxis resolves in double. Rounding
// to nearest can land one ulp past the contact surface, which the next sweep
// would then read as already inside the block. Round toward the side the
// player came from instead.
float TickLoop::RoundTowardOrigin(double target, bool movingPositive)
{
    float f = static_cast<float>(target);
    if (movingPositive && static_cast<double>(f) > target)
        f = std::nextafter(f, -std::numeric_limits<float>::infinity());
    else if (!movingPositive && static_cast<double>(f) < target)
        f = std::nextafter(f, std::numeric_limits<float>::infinity());
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
        horizontalCollision = true; // MovementFlags bit 1 — see HasHorizontalCollision.
        horizontalCollisionAxes |= static_cast<uint8_t>(1u << axis);
        if (std::fabs(lastHorizontalDir[axis]) >= MINOR_COLLISION_SIN) majorHorizontalCollision = true;
    }
}

} // namespace Volcano
