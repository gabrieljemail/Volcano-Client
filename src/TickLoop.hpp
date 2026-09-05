#pragma once
#ifndef VOLCANO_TICK_LOOP_H
#define VOLCANO_TICK_LOOP_H

#include <glm/glm.hpp>
#include "GlobalState.hpp"

namespace Volcano {

// Fixed-timestep client-side simulation: gravity, jump, and (no-momentum)
// walk-speed movement, resolved via axis-separated AABB collision against
// the placeholder World. Runs on the render thread (see the tick-loop plan
// for why) via Advance(), which is called once per render frame but may run
// zero or more fixed 1/20s Tick()s internally depending on how much time has
// accumulated — decoupling simulation rate from render framerate.
//
// Deliberately not vanilla-exact physics: no per-tick momentum/friction,
// sprint/sneak multipliers, or non-solid block special-casing (water,
// ladders, etc.) — see the plan's scope note.
class TickLoop {
public:
    static constexpr float FIXED_DT = 1.0f / 20.0f;

    explicit TickLoop(GlobalState* state);

    void Advance(float frameDeltaTime);

    // Smoothly interpolated position for rendering this frame. NOT the
    // authoritative tick position (which only updates once per Tick()) —
    // RenderThread should use this for the camera instead of
    // Player::GetPosition() directly, so motion stays smooth between the
    // fixed simulation steps.
    glm::vec3 GetRenderPosition() const;

private:
    GlobalState* state;
    float accumulator = 0.0f;
    bool jumpQueued = false;

    glm::vec3 previousPosition;
    glm::vec3 currentPosition;
    glm::vec3 velocity{0.0f};
    bool grounded = false;

    void Tick();
    bool IsSolid(int x, int y, int z) const;
    bool AabbOverlapsSolid(glm::vec3 center) const;
    void MoveAxis(glm::vec3& position, glm::vec3& vel, int axis, float delta);
};

} // namespace Volcano

#endif
