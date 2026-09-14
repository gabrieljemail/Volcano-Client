#pragma once
#ifndef VOLCANO_TICK_LOOP_H
#define VOLCANO_TICK_LOOP_H

#include <glm/glm.hpp>
#include <mutex>
#include <optional>
#include "GlobalState.hpp"

namespace Volcano {

// Fixed-timestep client-side simulation: gravity, jump, and friction-based
// walk-speed movement (vanilla's real per-tick constants — see Tick()),
// resolved via axis-separated AABB collision against the placeholder World.
// Runs on the render thread (see the tick-loop plan for why) via Advance(),
// which is called once per render frame but may run zero or more fixed
// 1/20s Tick()s internally depending on how much time has accumulated —
// decoupling simulation rate from render framerate, and conveniently
// matching a real Minecraft tick exactly (FIXED_DT == 1/20s), so velocity
// is tracked in vanilla's own blocks/tick units rather than blocks/sec.
//
// Not vanilla-exact in every respect: sprint/sneak are client-side speed
// multipliers only (no sneak edge-detection, no server-side attribute
// modifiers), no non-solid block special-casing (water, ladders, etc.), and
// ground friction/acceleration use default block slipperiness rather than
// reading it per-block — see the plan's scope note.
class TickLoop {
public:
    static constexpr float FIXED_DT = 1.0f / 20.0f;

    explicit TickLoop(GlobalState* state);

    // inputAllowed gates WASD/jump/look — gravity, collision, and applying
    // the server's teleports/position sync still run every call regardless,
    // so a screen (pause, death, chat, ...) being open doesn't freeze the
    // player mid-air or leave a pending respawn teleport stuck unapplied.
    void Advance(float frameDeltaTime, bool inputAllowed);

    // Smoothly interpolated position for rendering this frame. NOT the
    // authoritative tick position (which only updates once per Tick()) —
    // RenderThread should use this for the camera instead of
    // Player::GetPosition() directly, so motion stays smooth between the
    // fixed simulation steps.
    glm::vec3 GetRenderPosition() const;

    // Re-caches position from state->player (called from MeshingThread
    // after it applies the server's spawn position via Player::SetPosition)
    // and resets velocity/grounded for a clean start. Must only be called
    // while GlobalState::worldReady is still false — see its comment for
    // why that ordering makes this race-free against RenderThread's
    // concurrent Advance() calls.
    void SyncToPlayerPosition();

    // Thread-safe counterpart to SyncToPlayerPosition, for the server's
    // later teleports (every Player Position packet after the first — the
    // server re-sends one whenever it disagrees with where we think we are).
    // Once worldReady is set, this thread no longer owns TickLoop's
    // simulation state, so the position is parked here and applied at the
    // top of the next Advance() on the render thread instead of being
    // written across threads.
    void QueueTeleport(glm::vec3 position);

private:
    GlobalState* state;
    float accumulator = 0.0f;
    bool jumpQueued = false;

    std::mutex teleportMutex;
    std::optional<glm::vec3> pendingTeleport;

    glm::vec3 previousPosition;
    glm::vec3 currentPosition;
    glm::vec3 velocity{0.0f};
    bool grounded = false;

    void Tick(bool inputAllowed);
    bool AabbOverlapsSolid(glm::vec3 center) const;
    void MoveAxis(glm::vec3& position, glm::vec3& vel, int axis, float delta);
};

} // namespace Volcano

#endif
