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

void TickLoop::Tick()
{
    previousPosition = currentPosition;

    // Horizontal velocity is set directly from input each tick (no
    // momentum/friction — see the plan's scope note), rotated by camera
    // yaw the same way the old fly-cam ApplyMovement did.
    float forwardInput = state->input->GetAxis("Move.Forward");
    float rightInput = state->input->GetAxis("Move.Right");

    float yaw = glm::radians(state->player->camera.GetYaw());
    glm::vec3 forward{ sin(yaw), 0.0f, cos(yaw) };
    glm::vec3 right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::vec3 horizontalDir = forward * forwardInput + right * rightInput;
    float walkSpeed = static_cast<float>(state->attributes->GetDouble("generic.movement_speed", 4.317));
    if (glm::length(horizontalDir) > 0.0001f)
    {
        horizontalDir = glm::normalize(horizontalDir);
        velocity.x = horizontalDir.x * walkSpeed;
        velocity.z = horizontalDir.z * walkSpeed;
    }
    else
    {
        velocity.x = 0.0f;
        velocity.z = 0.0f;
    }

    // Gravity applies every tick regardless of grounded state; resting on
    // the ground works because the resulting downward move gets collided
    // and zeroed back out below, every tick, not because gravity itself
    // stops.
    float gravity = static_cast<float>(state->attributes->GetDouble("generic.gravity", 32.0));
    velocity.y -= gravity * FIXED_DT;

    if (jumpQueued)
    {
        if (grounded)
        {
            float jumpStrength = static_cast<float>(state->attributes->GetDouble("generic.jump_strength", 9.0));
            velocity.y = jumpStrength;
            grounded = false;
        }
        jumpQueued = false;
    }

    glm::vec3 pos = currentPosition;
    MoveAxis(pos, velocity, 1, velocity.y * FIXED_DT); // Y first: settles grounded state before horizontal collision.
    MoveAxis(pos, velocity, 0, velocity.x * FIXED_DT);
    MoveAxis(pos, velocity, 2, velocity.z * FIXED_DT);

    currentPosition = pos;
    state->player->SetPosition(currentPosition);
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
    return state->world->GetBlock(x, y, z).isOpaque();
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
