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

    // Same cross-thread reasoning as IsGrounded — MovementFlags bit 1, for
    // TickThread's own movement packets.
    bool HasHorizontalCollision() const { return horizontalCollision.load(std::memory_order_relaxed); }

    // Current sneak state, as last published by PublishInput — read by
    // InteractionManager::SecondaryTriggerStart for UseEntity's own
    // `sneaking` field (vanilla's interact packet reports it independently
    // of movement). Same cross-thread reasoning as IsGrounded.
    bool IsSneaking() const { return sneakActive.load(std::memory_order_relaxed); }

    // Which movement keys are held, as PlayC2S::PlayerInput's bitflags —
    // forward, backward, left, right, jump, shift, sprint from bit 0 up;
    // shift is how the server learns we're sneaking. Backed entirely by
    // atomics (the published-input fields below), so safe to call cross-
    // thread — TickThread reports this whenever it changes.
    uint8_t GetMovementInputFlags() const;

    // Cross-thread snapshot of the movement state vanilla's attack logic
    // reads to decide what kind of hit a swing is (see InteractionManager::
    // ClassifyAttack), and that TickThread reports to the server for
    // PlayC2S::PlayerCommand start/stop_sprinting (the server's sprint flag
    // is what turns a full-strength hit into a knockback hit): sprinting,
    // on the ground, how far the current fall has gone, and horizontal
    // distance covered last tick. Written at the end of every Tick() on
    // RenderThread, read from other threads under movementSnapshotMutex.
    struct MovementSnapshot {
        bool sprinting = false;
        bool grounded = false;
        float fallDistance = 0.0f;       // Blocks fallen since last leaving the ground.
        float horizontalSpeed = 0.0f;    // Blocks moved horizontally in the last fixed step.
    };
    MovementSnapshot GetMovementSnapshot() const;

    // Edge-triggered jump request — call once from PollInputs when
    // WasActivated("Jump") is true. Latches until the next Tick() consumes
    // it (test-and-clear), so a tap landing between two frames can't be
    // dropped the way a plain last-write-wins flag could drop it between
    // two FixedStep()s.
    void RequestJump() { jumpRequested.store(true); }

    // Interpolated render-frame position — mixes previousPosition/
    // currentPosition using elapsed wall time, computed fresh every call
    // (not cached) since FixedStep() only advances at a fixed 20Hz. Use this
    // for the camera, not Player::GetPosition() directly.
    glm::vec3 GetRenderPosition() const;

    // 0..1 fraction of the current fixed tick elapsed (same alpha
    // GetRenderPosition mixes by), for render-side interpolation.
    float GetPartialTick() const;

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

    // Sprint state (see UpdateSprinting), fall distance (for crits — see
    // MovementSnapshot), and which horizontal axes (bit 0 = X, bit 2 = Z)
    // were blocked by a wall in the last fixed step. RenderThread-only,
    // same as previousPosition/currentPosition/velocity above — exposed
    // cross-thread only via the mutex-protected MovementSnapshot.
    bool sprinting = false;
    float fallDistance = 0.0f;
    uint8_t horizontalCollisionAxes = 0;
    // Whether the last wall hit blocked a meaningful part of the intended
    // movement, as opposed to grazing a wall at a shallow angle — only the
    // former stops a sprint (vanilla's "minor horizontal collision").
    bool majorHorizontalCollision = false;
    glm::vec3 lastHorizontalDir{0.0f};

    mutable std::mutex movementSnapshotMutex;
    MovementSnapshot movementSnapshot;

    // Applies everything queued on GlobalState::localMotion since the last
    // call, in order — see LocalMotionInbox's own comment.
    void DrainLocalMotion();

    // Vanilla's sprint rules (LocalPlayer.aiStep): start while the sprint
    // key is held, moving forward, not sneaking, with more than 6 food;
    // once started, keep going even after the key is let go, until forward
    // movement stops, sneaking starts, food runs low, or the player runs
    // head-on into a wall.
    void UpdateSprinting(bool inputAllowedNow, float forwardInput, bool sneaking);

    // One fixed 1/20s simulation step — the per-tick body Tick() calls zero
    // or more times per invocation. Named separately from the public,
    // self-timed Tick() above now that "the thing driving ticks" and "one
    // fixed step" are no longer the same method the way Advance()/Tick()
    // used to be.
    void FixedStep(bool inputAllowedNow);

    // Plays the footstep for the block under the player's feet. Called from
    // FixedStep once enough distance and time have passed since the last one.
    void PlayStepSound();

    // Advances GlobalState's crosshair indicator prev/current pair one tick.
    void UpdateCrosshairProgress();
    int ticksSinceStepSound = 0;
    void MoveAxis(glm::vec3& position, glm::vec3& vel, int axis, float delta);

    // How far a move of `delta` along `axis` (0/1/2) can go before the
    // player's AABB hits a collision box — vanilla's Shapes.collide. Returns
    // a value between 0 and `delta`; anything short of `delta` means blocked.
    double SweepAxis(glm::vec3 position, int axis, double delta) const;

    // Float rounding of a double contact coordinate that never ends up past
    // the contact surface. See the definition.
    static float RoundTowardOrigin(double target, bool movingPositive);
};

} // namespace Volcano

#endif
