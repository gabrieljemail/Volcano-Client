#pragma once
#ifndef VOLCANO_RIDE_CONTROLLER_H
#define VOLCANO_RIDE_CONTROLLER_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <glm/glm.hpp>
#include "GlobalState.hpp"
#include "renderer/entity/models/EntitySeat.hpp"

namespace Volcano {

class Connection;

// Steers the vehicle or mount we are riding (GlobalState::ridingEntityId).
// Vehicles are client-authoritative in vanilla: we simulate the boat or horse
// here, write its pose onto the vehicle Entity, and report it to the server
// every tick with vehicle_move. Boats and horses/pigs are simulated; anything
// else (minecarts, striders, unknown mounts) is only followed.
//
// Threads: Step runs on RenderThread (from TickLoop::FixedStep, once per
// fixed step). QueueTickPackets runs on TickThread. ApplyServerMove runs on
// NetworkThread. Step locks entitiesMutex briefly; nothing else here holds
// two locks at once.
class RideController {
public:
    explicit RideController(GlobalState* state) : state(state) {}

    struct Input
    {
        float forward = 0.0f;      // -1..1, positive forward.
        float right = 0.0f;        // -1..1, positive right.
        bool jumpHeld = false;
        bool inputAllowed = true;  // False while a screen or chat has focus.
    };

    struct Result
    {
        bool riding = false;                // False: ridingEntityId is -1, run normal physics.
        glm::vec3 playerPosition{0.0f};     // Where the rider's feet sit this tick (the seat).
        float yawDelta = 0.0f;              // Degrees to add to the camera yaw (boats turn the rider with them).
    };

    // One fixed 1/20s step while ridingEntityId >= 0. Simulates the vehicle,
    // updates its Entity, and returns the seat for the player and camera.
    Result Step(const Input& input);

    // Queues this tick's rider packets: the rotation-only player move,
    // vehicle_move and steer_boat for a steered vehicle, and a queued horse
    // jump. Returns true while riding, in which case the caller must skip its
    // position-bearing player move packets.
    bool QueueTickPackets(Connection& connection);

    // Server snap of the steered vehicle (PlayS2C::VehicleMove). Applied on the
    // next Step. Yaw and pitch are Minecraft degrees.
    void ApplyServerMove(glm::vec3 position, float yawDegrees, float pitchDegrees);

private:
    GlobalState* state;

    // RenderThread-only simulation state. yawDeg is Minecraft's yaw (the
    // camera's, negated); Entity::yaw radians equals -radians(yawDeg).
    int32_t activeId = -1;
    EntitySeat::Kind kind = EntitySeat::Kind::None;
    bool controlled = false;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yawDeg = 0.0f;
    float pitchDeg = 0.0f;
    float deltaRotation = 0.0f;
    bool grounded = false;
    bool horizontalCollision = false;
    float halfWidth = 0.3f;
    float height = 1.0f;
    float stepHeight = 0.0f;
    float movementSpeed = 0.0f;
    float jumpStrength = 0.0f;
    bool paddleLeft = false;
    bool paddleRight = false;

    // Horse jump charge (vanilla LocalPlayer.handleRidingJump).
    bool wasJumpHeld = false;
    int jumpHoldTicks = 0;
    float pendingJump = 0.0f;
    int queuedJumpBoost = -1;

    // What TickThread reports; written by Step, read by QueueTickPackets.
    struct Packet
    {
        bool steered = false;
        bool boat = false;
        glm::vec3 position{0.0f};
        float yawDeg = 0.0f;
        float pitchDeg = 0.0f;
        bool onGround = false;
        bool horizontalCollision = false;
        bool paddleLeft = false;
        bool paddleRight = false;
        int jumpBoost = -1;
    };
    std::mutex packetMutex;
    Packet packet;

    struct Correction
    {
        glm::vec3 position{0.0f};
        float yawDeg = 0.0f;
    };
    std::mutex correctionMutex;
    std::optional<Correction> correction;

    void Begin(const Entity& vehicle);
    void Release();
    bool HoldsSteeringItem() const;
    void StepBoat(const Input& input);
    void StepMount(const Input& input);
    void UpdateJumpCharge(const Input& input);
    double Sweep(glm::vec3 at, int axis, double delta) const;
    void MoveAxis(glm::vec3& at, glm::vec3& vel, int axis, float delta);
    void Move(glm::vec3& at, glm::vec3& vel);
};

} // namespace Volcano

#endif
