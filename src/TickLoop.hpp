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
// walk-speed movement (vanilla's real per-tick constants — see Tick()),
// resolved via axis-separated AABB collision against the placeholder World.
// Horizontal collision also auto-steps onto sub-0.6-block obstructions
// (slabs, snow layers, a staircase's first step) instead of just blocking —
// see MoveAxis/TryStepUp.
// Runs on NetworkThread (see the tick-loop plan for why — briefly, its
// packet loop is the thread already closest to "the authoritative
// simulation clock," since it's what receives the server's position
// corrections and chunk data movement collides against) via Tick(), which
// is called once per NetworkThread loop iteration but may run zero or more
// fixed 1/20s FixedStep()s internally depending on how much wall-clock time
// has accumulated since the last call — decoupling simulation rate from
// both render framerate and network loop cadence, and conveniently matching
// a real Minecraft tick exactly (FIXED_DT == 1/20s), so velocity is tracked
// in vanilla's own blocks/tick units rather than blocks/sec.
//
// Because Tick() no longer shares a thread with RenderThread::PollInputs,
// every piece of input it needs is published into the atomics below once
// per render frame (PublishInput/RequestJump) instead of being read
// straight from InputHandler — see their own comments.
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

    // Called once per NetworkThread loop iteration (see
    // NetworkClient::RunPlayLoop) — measures real elapsed wall time since
    // the previous call itself (see lastTickTime), feeds that into the
    // fixed-step accumulator below, and runs zero or more FixedStep()s to
    // catch up. Gravity, collision, and applying the server's
    // teleports/position sync run every call regardless of screen state —
    // only WASD/jump/look are gated (via the published inputAllowed flag),
    // so a screen (pause, death, chat, ...) being open doesn't freeze the
    // player mid-air or leave a pending respawn teleport stuck unapplied.
    void Tick();

    // This frame's tick-relevant input, published by RenderThread::
    // PollInputs (Main/Render) for Tick() (NetworkThread) to read — see the
    // class comment. moveForward/moveRight/sneaking/sprinting/jumpHeld are
    // all level-triggered (a stale-by-one-frame read is harmless, unlike an
    // edge — see RequestJump for that case), so plain atomics with
    // last-write-wins are enough; no mutex needed.
    void PublishInput(bool inputAllowed, float moveForward, float moveRight,
                       bool sneaking, bool sprinting, bool jumpHeld);

    // Whether the last FixedStep() left the player resting on solid ground —
    // read by NetworkThread (via NetworkClient::SetTickCallback's return
    // value) right after calling Tick(), for the MovementFlags.onGround bit
    // in the position packets NetworkClient reports every tick. Same-thread
    // read of a same-thread write (Tick() and this are both only ever
    // called from NetworkThread), so no lock/atomic is needed — same
    // reasoning previousPosition/currentPosition/velocity's own comments
    // give.
    bool IsGrounded() const { return grounded; }

    // Edge-triggered jump request — call once from PollInputs when
    // WasActivated("Jump") is true. Latches until the next Tick() consumes
    // it (test-and-clear), so a tap landing between two NetworkThread loop
    // iterations can't be dropped the way a plain last-write-wins flag
    // could drop it between two FixedStep()s.
    void RequestJump() { jumpRequested.store(true); }

    // Smoothly interpolated position for rendering this frame — a cached
    // snapshot written at the end of every Tick() call, not computed live
    // from previousPosition/currentPosition/accumulator on read. Those
    // three are Tick()-thread-owned state now that Tick() runs on
    // NetworkThread; GetRenderPosition() is called every render frame from
    // Main/Render, so reading them directly would be a data race. NOT the
    // authoritative tick position (which only updates once per FixedStep())
    // — RenderThread should use this for the camera instead of
    // Player::GetPosition() directly, so motion stays smooth between the
    // fixed simulation steps.
    glm::vec3 GetRenderPosition() const;

    // Re-caches position from state->player (called from MeshingThread
    // after it applies the server's spawn position via Player::SetPosition),
    // resets velocity/grounded for a clean start, and re-arms lastTickTime
    // so the first real Tick() afterward computes a small, accurate dt
    // instead of one spanning however long the app sat at worldReady ==
    // false. Must only be called while GlobalState::worldReady is still
    // false — see its comment for why that ordering makes this race-free
    // against NetworkThread's concurrent Tick() calls.
    void SyncToPlayerPosition();

    // Thread-safe counterpart to SyncToPlayerPosition, for the server's
    // later teleports (every Player Position packet after the first — the
    // server re-sends one whenever it disagrees with where we think we are).
    // Once worldReady is set, this thread no longer owns TickLoop's
    // simulation state, so the position is parked here and applied at the
    // top of the next Tick() on NetworkThread instead of being written
    // across threads.
    void QueueTeleport(glm::vec3 position);

private:
    GlobalState* state;
    float accumulator = 0.0f;

    // Wall-clock source for Tick()'s self-timed dt — see Tick()'s own
    // comment. Re-armed by SyncToPlayerPosition() so the transition into
    // worldReady never produces one huge catch-up dt.
    std::chrono::steady_clock::time_point lastTickTime = std::chrono::steady_clock::now();

    // Latches a jump request across the gap between input frames (render
    // rate) and simulation frames (fixed 20Hz ticks) — see RequestJump's
    // comment for how it's set (the one-shot PRESS edge) and Tick() for how
    // it's consumed alongside jumpHeld (the HOLD repeat).
    bool jumpQueued = false;
    std::atomic<bool> jumpRequested{false};

    // Published every render frame by PublishInput() (see its own comment)
    // — level-triggered, so plain atomics with last-write-wins are enough.
    std::atomic<bool> inputAllowed{true};
    std::atomic<float> moveForwardInput{0.0f};
    std::atomic<float> moveRightInput{0.0f};
    std::atomic<bool> sneakActive{false};
    std::atomic<bool> sprintActive{false};
    std::atomic<bool> jumpHeldActive{false};

    // See GetRenderPosition()'s own comment on why this is a cached
    // snapshot rather than a live computation.
    mutable std::mutex renderPositionMutex;
    glm::vec3 renderPositionSnapshot{0.0f};

    std::mutex teleportMutex;
    std::optional<glm::vec3> pendingTeleport;

    glm::vec3 previousPosition;
    glm::vec3 currentPosition;
    glm::vec3 velocity{0.0f};
    bool grounded = false;

    // One fixed 1/20s simulation step — the per-tick body Tick() calls zero
    // or more times per invocation. Named separately from the public,
    // self-timed Tick() above now that "the thing NetworkThread calls" and
    // "one fixed step" are no longer the same method the way Advance()/
    // Tick() used to be.
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
