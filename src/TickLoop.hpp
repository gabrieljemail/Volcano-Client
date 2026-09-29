#pragma once
#ifndef VOLCANO_TICK_LOOP_H
#define VOLCANO_TICK_LOOP_H

#include <atomic>
#include <chrono>
#include <glm/glm.hpp>
#include <mutex>
#include <optional>
#include "GlobalState.hpp"

namespace Volcano {

// Fixed-timestep client-side simulation: gravity, jump, and friction-based
// walk-speed movement, resolved via axis-separated AABB collision. Horizontal
// collision also auto-steps sub-0.6-block obstructions — see MoveAxis/TryStepUp.
//
// Tick() runs on RenderThread, once per frame — back to pre-networking
// behavior. An earlier version ran it on a dedicated TickThread instead
// (fixing a position-report cadence issue), but sharing few logical cores
// with MeshingThread's own unyielding bursts made that thread's scheduling
// gaps cause visible hitching. TickThread now only reads position/grounded/
// horizontalCollision to build packets; it never writes movement state.
//
// Not vanilla-exact: sprint/sneak are client-side speed multipliers only, no
// non-solid block special-casing (water, ladders), default slipperiness only.
class TickLoop {
public:
    static constexpr float FIXED_DT = 1.0f / 20.0f;

    explicit TickLoop(GlobalState* state);

    // Called once per RenderThread frame, right after input is sampled.
    // Self-timed (see lastTickTime), runs zero or more fixed FixedStep()s to
    // catch up. Gravity/collision/teleports run regardless of screen state;
    // only WASD/jump/look are gated behind inputAllowed.
    void Tick();

    // This frame's input, published right before Tick() consumes it. Kept
    // atomic mainly to keep PublishInput/Tick() as separate calls, not for
    // cross-thread safety — both now run on RenderThread.
    void PublishInput(bool inputAllowed, float moveForward, float moveRight,
                       bool sneaking, bool sprinting, bool jumpHeld);

    // Grounded state from the last FixedStep(), for TickThread's movement
    // packets — genuinely cross-thread now, hence atomic.
    bool IsGrounded() const { return grounded.load(std::memory_order_relaxed); }

    // Same cross-thread reasoning as IsGrounded — MovementFlags bit 1.
    bool HasHorizontalCollision() const { return horizontalCollision.load(std::memory_order_relaxed); }

    // Current sneak state, as last published by PublishInput — read by
    // InteractionManager::SecondaryTriggerStart for UseEntity's own
    // `sneaking` field (vanilla's interact packet reports it independently
    // of movement). Same cross-thread reasoning as IsGrounded.
    bool IsSneaking() const { return sneakActive.load(std::memory_order_relaxed); }

    // Edge-triggered jump; latches until the next FixedStep() consumes it so
    // a tap between fixed steps can't be dropped.
    void RequestJump() { jumpRequested.store(true); }

    // Interpolated render-frame position — mixes previousPosition/
    // currentPosition using elapsed wall time, computed fresh every call
    // (not cached) since FixedStep() only advances at a fixed 20Hz. Use this
    // for the camera, not Player::GetPosition() directly.
    glm::vec3 GetRenderPosition() const;

    // Re-caches position from state->player after MeshingThread applies the
    // server's spawn, resets velocity/grounded, re-arms lastTickTime. Only
    // valid while GlobalState::worldReady is still false.
    void SyncToPlayerPosition();

    // Thread-safe counterpart to SyncToPlayerPosition for later teleports —
    // parks the position for the next Tick() to apply instead of writing
    // simulation state across threads directly.
    void QueueTeleport(glm::vec3 position);

private:
    GlobalState* state;
    float accumulator = 0.0f;

    // Wall-clock source for Tick()'s self-timed dt. Re-armed by
    // SyncToPlayerPosition() to avoid one huge catch-up dt on world load.
    std::chrono::steady_clock::time_point lastTickTime = std::chrono::steady_clock::now();

    // Latches a jump request across the gap between input frames (render
    // rate) and simulation frames (fixed 20Hz ticks) — see RequestJump's
    // comment for how it's set (the one-shot PRESS edge) and Tick() for how
    // it's consumed alongside jumpHeld (the HOLD repeat).
    bool jumpQueued = false;
    std::atomic<bool> jumpRequested{false};

    // Published by PublishInput() — level-triggered last-write-wins.
    std::atomic<bool> inputAllowed{true};
    std::atomic<float> moveForwardInput{0.0f};
    std::atomic<float> moveRightInput{0.0f};
    std::atomic<bool> sneakActive{false};
    std::atomic<bool> sprintActive{false};
    std::atomic<bool> jumpHeldActive{false};

    // Raw pair + timestamp GetRenderPosition() mixes fresh each call.
    mutable std::mutex renderPositionMutex;
    glm::vec3 renderPreviousPosition{0.0f};
    glm::vec3 renderCurrentPosition{0.0f};
    std::chrono::steady_clock::time_point lastFixedStepTime = std::chrono::steady_clock::now();

    std::mutex teleportMutex;
    std::optional<glm::vec3> pendingTeleport;

    glm::vec3 previousPosition;
    glm::vec3 currentPosition;
    glm::vec3 velocity{0.0f};

    // Read cross-thread via IsGrounded()/HasHorizontalCollision(), hence
    // atomic — unlike previousPosition/currentPosition/velocity above.
    std::atomic<bool> grounded{false};
    std::atomic<bool> horizontalCollision{false};

    // One fixed 1/20s simulation step; Tick() calls zero or more per invocation.
    void FixedStep(bool inputAllowedNow);
    bool AabbOverlapsSolid(glm::vec3 center) const;
    void MoveAxis(glm::vec3& position, glm::vec3& vel, int axis, float delta);
    std::optional<float> TryStepUp(glm::vec3 position, glm::vec3 tentative) const;

    // Exact landing height for a downward move that collided: the topmost
    // solid-box surface (BlockRegistry::GetCollisionBoxes, not a stepped
    // probe like TryStepUp's own snap) under the player's horizontal
    // footprint, within [blockedY, clearY] — i.e. whatever was actually in
    // the way. Analytic rather than stepped specifically because a stepped
    // search quantizes to its own step size and can land short of the true
    // surface by a fraction of that step; that gap used to be harmless
    // (nothing read it precisely) but isn't once the server's own strict
    // collision check cross-references the reported Y against the reported
    // onGround flag every tick — see MoveAxis's own comment.
    float FindGroundContactHeight(float worldX, float worldZ, float clearY, float blockedY) const;

    // Horizontal analogue of FindGroundContactHeight, for a sideways move
    // that collided and couldn't be resolved by stepping up: the coordinate
    // along `axis` (0 = X, 2 = Z) that leaves the player flush against the
    // nearest blocking collision box, rather than frozen wherever the
    // blocked move left them. `position` is the player's current
    // (known-clear) center and `blockedCoord` the axis coordinate the
    // refused move targeted; the direction of travel is taken from the two.
    // Exact for the same reason FindGroundContactHeight is — the server
    // cross-references reported position against its own collision every
    // tick, and an arbitrary gap from the contact surface is what it
    // rejects.
    float FindWallContactCoordinate(int axis, glm::vec3 position, float blockedCoord) const;
};

} // namespace Volcano

#endif
