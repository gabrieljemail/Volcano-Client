#include "TickLoop.hpp"
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
} // namespace

TickLoop::TickLoop(GlobalState* stateIn) : state(stateIn)
{
    previousPosition = currentPosition = state->player->GetPosition();
}

void TickLoop::Advance(float frameDeltaTime)
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

    jumpQueued = jumpQueued || state->input->WasActivated("Jump");

    accumulator += frameDeltaTime;
    if (accumulator > MAX_ACCUMULATED_TIME) accumulator = MAX_ACCUMULATED_TIME;

    while (accumulator >= FIXED_DT)
    {
        Tick();
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

void TickLoop::Tick()
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
    float forwardInput = state->input->GetAxis("Move.Forward");
    float rightInput = state->input->GetAxis("Move.Right");

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

    // Scope-limited sneak: just lower the camera, don't touch the collision AABB.
    state->player->camera.eyeOffset.y = sneaking ? 1.27f : 1.62f;

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
        if (axis == 1 && delta < 0.0f) grounded = true;
        vel[axis] = 0.0f;
        return;
    }

    position = tentative;
    if (axis == 1 && delta < 0.0f) grounded = false; // still falling
}

bool TickLoop::IsSolid(int x, int y, int z) const
{
    // hasCollision() also covers transparent-cube/partial non-cube shapes
    // (glass, slabs, ...) — only cross-shaped plants (grass, flowers, ...)
    // are non-solid despite having a non-cube visual. See Block::hasCollision.
    return state->world->GetBlock(x, y, z).hasCollision();
}

bool TickLoop::AabbOverlapsSolid(glm::vec3 center) const
{
    constexpr float epsilon = 1e-4f;

    int minX = static_cast<int>(std::floor(center.x - PLAYER_HALF_WIDTH + epsilon));
    int maxX = static_cast<int>(std::floor(center.x + PLAYER_HALF_WIDTH - epsilon));
    int minY = static_cast<int>(std::floor(center.y + epsilon));
    int maxY = static_cast<int>(std::floor(center.y + PLAYER_HEIGHT - epsilon));
    int minZ = static_cast<int>(std::floor(center.z - PLAYER_HALF_WIDTH + epsilon));
    int maxZ = static_cast<int>(std::floor(center.z + PLAYER_HALF_WIDTH - epsilon));

    for (int by = minY; by <= maxY; by++)
        for (int bz = minZ; bz <= maxZ; bz++)
            for (int bx = minX; bx <= maxX; bx++)
                if (IsSolid(bx, by, bz)) return true;

    return false;
}

} // namespace Volcano
