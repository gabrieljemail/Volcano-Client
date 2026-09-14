#include "TickLoop.hpp"
#include "renderer/terrain/models/BlockRegistry.hpp"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

namespace Volcano {

namespace {
constexpr float PLAYER_HALF_WIDTH = 0.3f;
constexpr float PLAYER_HEIGHT = 1.8f;
// Fixed-timestep accumulators can spiral if a frame stalls badly (a
// breakpoint, alt-tab, a slow load) — clamp how much time can accumulate so
// a stall causes a pause, not a burst of catch-up ticks.
constexpr float MAX_ACCUMULATED_TIME = 0.25f;
// Vanilla's own step height: obstructions this tall or shorter (slabs,
// snow layers/carpets, a staircase's first step) are auto-climbed on a
// horizontal move instead of blocking it — see MoveAxis/TryStepUp.
constexpr float STEP_HEIGHT = 0.6f;

// Sneak eye-height dip — half of vanilla's real drop (1.62 standing to 1.27
// sneaking, a 0.35 dip) per request, since a more subtle dip reads better
// once eased in over time instead of snapped.
constexpr float STANDING_EYE_HEIGHT = 1.62f;
constexpr float SNEAK_EYE_HEIGHT = STANDING_EYE_HEIGHT - (1.62f - 1.27f) * 0.5f;
// Exponential ease rate applied per-tick below — higher = snappier. Framerate
// (tick-rate) independent the same way RenderThread's FOV easing is.
constexpr float EYE_HEIGHT_EASE_RATE = 10.0f;
} // namespace

TickLoop::TickLoop(GlobalState* stateIn) : state(stateIn)
{
    previousPosition = currentPosition = state->player->GetPosition();
}

void TickLoop::Advance(float frameDeltaTime, bool inputAllowed)
{
    // Hold off simulating entirely until the world has something to stand
    // on — otherwise gravity runs from frame one against an empty World
    // (every lookup reads Air) and the player free-falls before any chunk
    // has had a chance to load. See GlobalState::worldReady for the
    // ordering guarantee that makes reading cached position state below
    // safe once this becomes true.
    if (!state->worldReady.load()) return;

    // Apply any teleport the server sent since the last frame. Done here,
    // on the render thread, because past this point this thread owns the
    // position/velocity state below — see QueueTeleport's comment.
    {
        std::lock_guard<std::mutex> lock(teleportMutex);
        if (pendingTeleport.has_value())
        {
            previousPosition = currentPosition = *pendingTeleport;
            state->player->SetPosition(currentPosition);
            velocity = glm::vec3(0.0f);
            grounded = false;
            accumulator = 0.0f;
            pendingTeleport.reset();
        }
    }

    // WasActivated("Jump") is the edge case: a tap that lands in a frame
    // which runs zero Tick()s must not be lost, so it latches here and stays
    // true until a Tick() actually consumes it (see jumpQueued's comment).
    // IsActive("JumpHold") adds vanilla's repeat-jump-on-landing on top of
    // that: as long as Space is physically down, this re-latches every
    // single frame regardless of whether the previous latch was already
    // consumed — so even though Tick() unconditionally clears jumpQueued
    // after checking it (discarding a jump attempt made while airborne),
    // holding the key means the very next frame queues another attempt, and
    // the first Tick() where the player is grounded again actually jumps.
    // Both stay gated behind inputAllowed — a screen/chat box being open
    // must not also make the player jump underneath it.
    if (inputAllowed) {
        jumpQueued = jumpQueued || state->input->WasActivated("Jump") || state->input->IsActive("JumpHold");
    }

    accumulator += frameDeltaTime;
    if (accumulator > MAX_ACCUMULATED_TIME) accumulator = MAX_ACCUMULATED_TIME;

    while (accumulator >= FIXED_DT)
    {
        Tick(inputAllowed);
        accumulator -= FIXED_DT;
    }
}

glm::vec3 TickLoop::GetRenderPosition() const
{
    float alpha = accumulator / FIXED_DT;
    return glm::mix(previousPosition, currentPosition, alpha);
}

void TickLoop::SyncToPlayerPosition()
{
    previousPosition = currentPosition = state->player->GetPosition();
    velocity = glm::vec3(0.0f);
    grounded = false;
    accumulator = 0.0f;
}

void TickLoop::QueueTeleport(glm::vec3 position)
{
    std::lock_guard<std::mutex> lock(teleportMutex);
    pendingTeleport = position;
}

void TickLoop::Tick(bool inputAllowed)
{
    previousPosition = currentPosition;

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
    float forwardInput = inputAllowed ? state->input->GetAxis("Move.Forward") : 0.0f;
    float rightInput = inputAllowed ? state->input->GetAxis("Move.Right") : 0.0f;

    float yaw = glm::radians(state->player->camera.GetYaw());
    glm::vec3 forward{ sin(yaw), 0.0f, cos(yaw) };
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::vec3 horizontalDir = forward * forwardInput + right * rightInput;
    if (glm::length(horizontalDir) > 1.0f) horizontalDir = glm::normalize(horizontalDir);

    constexpr float GROUND_FRICTION = 0.546f; // default block slipperiness (0.6) * 0.91
    constexpr float AIR_FRICTION = 0.91f;
    constexpr float AIR_ACCELERATION = 0.02f; // flat, unlike ground accel below — not attribute-scaled in vanilla either

    // Client-side-only sprint/sneak: vanilla applies these as attribute
    // modifiers server-side, but here it's simplest to just scale the speed
    // used below directly. Sneak wins if both are held, matching vanilla
    // (can't sprint while sneaking).
    bool sneaking = state->input->IsActive("Sneak");
    bool sprinting = !sneaking && state->input->IsActive("Sprint");
    float speedMultiplier = sneaking ? 0.3f : (sprinting ? 1.3f : 1.0f);

    float movementSpeed = static_cast<float>(state->attributes->GetDouble("generic.movement_speed", 0.1)) * speedMultiplier;
    float friction = grounded ? GROUND_FRICTION : AIR_FRICTION;
    // *0.98 reproduces vanilla's verified terminal walking speed (0.2159
    // blocks/tick) from the default 0.1 attribute value.
    float acceleration = grounded ? (movementSpeed * 0.98f) : AIR_ACCELERATION;

    velocity.x = velocity.x * friction + horizontalDir.x * acceleration;
    velocity.z = velocity.z * friction + horizontalDir.z * acceleration;

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
            grounded = false;
        }
        jumpQueued = false;
    }

    // velocity is in blocks/tick (matching vanilla) and FIXED_DT is exactly
    // one Minecraft tick, so each component IS this tick's displacement —
    // no further dt scaling needed.
    glm::vec3 pos = currentPosition;
    MoveAxis(pos, velocity, 1, velocity.y); // Y first: settles grounded state before horizontal collision.
    MoveAxis(pos, velocity, 0, velocity.x);
    MoveAxis(pos, velocity, 2, velocity.z);

    currentPosition = pos;
    state->player->SetPosition(currentPosition);

    // Scope-limited sneak: just lower the camera, don't touch the collision
    // AABB. Eased toward the target height each tick (see SNEAK_EYE_HEIGHT's
    // own comment) rather than snapped, so the dip reads as a smooth crouch
    // instead of a jump cut.
    float targetEyeHeight = sneaking ? SNEAK_EYE_HEIGHT : STANDING_EYE_HEIGHT;
    float& eyeHeight = state->player->camera.eyeOffset.y;
    eyeHeight += (targetEyeHeight - eyeHeight) * (1.0f - std::exp(-EYE_HEIGHT_EASE_RATE * FIXED_DT));

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
}

void TickLoop::MoveAxis(glm::vec3& position, glm::vec3& vel, int axis, float delta)
{
    glm::vec3 tentative = position;
    tentative[axis] += delta;

    if (AabbOverlapsSolid(tentative))
    {
        if (axis == 1)
        {
            if (delta < 0.0f) grounded = true;
            vel[axis] = 0.0f;
            return;
        }

        // Horizontal move blocked — before giving up, try vanilla's auto
        // step-up: if the same move clears at up to STEP_HEIGHT higher, snap
        // onto whatever's in the way and let the move through instead of
        // stopping dead against a slab/stair/snow layer. Never attempted for
        // axis 1 above — this is purely a horizontal-collision affordance
        // and must never touch vertical velocity/gravity resolution.
        std::optional<float> stepUpY = TryStepUp(position, tentative);
        if (stepUpY.has_value())
        {
            tentative.y = *stepUpY;
            position = tentative;
            return; // vel[axis] stays as-is — the move succeeded.
        }

        vel[axis] = 0.0f;
        return;
    }

    position = tentative;
    if (axis == 1 && delta < 0.0f) grounded = false; // still falling
}

std::optional<float> TickLoop::TryStepUp(glm::vec3 position, glm::vec3 tentative) const
{
    // No headroom to rise from where the player is currently standing —
    // stepping up would just trade a horizontal collision for a vertical
    // one, so don't bother probing the destination at all.
    if (AabbOverlapsSolid(glm::vec3(position.x, position.y + STEP_HEIGHT, position.z)))
        return std::nullopt;

    // If the destination is still blocked even fully raised by STEP_HEIGHT,
    // whatever's in the way is taller than a step (a real wall) — refuse it
    // rather than let the player climb it a tick at a time.
    glm::vec3 raised = tentative;
    raised.y = position.y + STEP_HEIGHT;
    if (AabbOverlapsSolid(raised))
        return std::nullopt;

    // Both ends of the range are confirmed clear, so there's a genuine
    // sub-step obstruction somewhere in between (that's the only reason the
    // un-raised move at position.y failed in the first place). Walk down
    // from the raised height in small increments to find the lowest clear
    // height — i.e. the top of whatever's being stepped onto — so the
    // player's feet land flush on it instead of resting at an arbitrary
    // fixed +STEP_HEIGHT offset (which would float above a shorter
    // obstruction like a single snow layer).
    constexpr float SEARCH_STEP = 1.0f / 64.0f;
    float snappedY = raised.y;
    for (float y = raised.y - SEARCH_STEP; y > position.y; y -= SEARCH_STEP)
    {
        glm::vec3 probe = tentative;
        probe.y = y;
        if (AabbOverlapsSolid(probe)) break; // still inside the obstruction; keep the last clear height.
        snappedY = y;
    }

    return snappedY;
}

bool TickLoop::AabbOverlapsSolid(glm::vec3 center) const
{
    constexpr float epsilon = 1e-4f;

    glm::vec3 playerMin(center.x - PLAYER_HALF_WIDTH, center.y, center.z - PLAYER_HALF_WIDTH);
    glm::vec3 playerMax(center.x + PLAYER_HALF_WIDTH, center.y + PLAYER_HEIGHT, center.z + PLAYER_HALF_WIDTH);

    int minX = static_cast<int>(std::floor(playerMin.x + epsilon));
    int maxX = static_cast<int>(std::floor(playerMax.x - epsilon));
    int minY = static_cast<int>(std::floor(playerMin.y + epsilon));
    int maxY = static_cast<int>(std::floor(playerMax.y - epsilon));
    int minZ = static_cast<int>(std::floor(playerMin.z + epsilon));
    int maxZ = static_cast<int>(std::floor(playerMax.z - epsilon));

    // Real per-shape collision (BlockRegistry::GetCollisionBoxes) instead of
    // treating every touched voxel as fully solid — a block cell can hold
    // several small boxes (a stair) or one smaller than the full voxel (a
    // slab, an end rod's post), and the player's AABB must actually overlap
    // one of them, not just share the cell.
    for (int by = minY; by <= maxY; by++) {
        for (int bz = minZ; bz <= maxZ; bz++) {
            for (int bx = minX; bx <= maxX; bx++) {
                Block block = state->world->GetBlock(bx, by, bz);
                BlockRegistry::CollisionBoxes shape = BlockRegistry::GetCollisionBoxes(block);

                glm::vec3 origin(static_cast<float>(bx), static_cast<float>(by), static_cast<float>(bz));
                for (int i = 0; i < shape.count; i++) {
                    glm::vec3 boxMin = origin + shape.boxes[static_cast<size_t>(i)].min;
                    glm::vec3 boxMax = origin + shape.boxes[static_cast<size_t>(i)].max;

                    bool overlaps = playerMin.x < boxMax.x - epsilon && playerMax.x > boxMin.x + epsilon
                        && playerMin.y < boxMax.y - epsilon && playerMax.y > boxMin.y + epsilon
                        && playerMin.z < boxMax.z - epsilon && playerMax.z > boxMin.z + epsilon;
                    if (overlaps) return true;
                }
            }
        }
    }

    return false;
}

} // namespace Volcano
