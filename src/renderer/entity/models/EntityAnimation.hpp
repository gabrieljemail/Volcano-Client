#pragma once
#ifndef VOLCANO_ENTITY_ANIMATION_H
#define VOLCANO_ENTITY_ANIMATION_H

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include "EntityJsonModel.hpp"

namespace Volcano::EntityAnimation {

constexpr size_t MAX_PARTS = 16;
constexpr float SWING_SECONDS = 0.3f; // Vanilla's 6-tick arm swing.
constexpr float ARM_ROLL = glm::radians(5.0f); // Resting angle of the arms away from the body.

// One part's pose in vanilla's own conventions (model units, radians, Y down).
// The renderer converts to world space when it draws.
struct PartPose {
    glm::vec3 offset{ 0.0f };
    glm::vec3 rotation{ 0.0f };
};
using Pose = std::array<PartPose, MAX_PARTS>;

// Which parts of a mesh play which role, by name; -1 where absent.
struct Roles {
    int head = -1, body = -1;
    int rightArm = -1, leftArm = -1;
    int rightLeg = -1, leftLeg = -1;
    int rightHindLeg = -1, leftHindLeg = -1, rightFrontLeg = -1, leftFrontLeg = -1;
    bool legged = false; // Four-legged walkers; spiders keep their splayed legs still.
};

inline Roles FindRoles(const std::vector<EntityJsonModel::PartInfo>& parts)
{
    Roles roles;
    bool spider = false;
    for (size_t i = 0; i < parts.size() && i < MAX_PARTS; i++)
    {
        const std::string& name = parts[i].name;
        int index = static_cast<int>(i);
        if (name == "head") roles.head = index;
        else if (name == "body") roles.body = index;
        else if (name == "right_arm") roles.rightArm = index;
        else if (name == "left_arm") roles.leftArm = index;
        else if (name == "right_leg") roles.rightLeg = index;
        else if (name == "left_leg") roles.leftLeg = index;
        else if (name == "right_hind_leg") roles.rightHindLeg = index;
        else if (name == "left_hind_leg") roles.leftHindLeg = index;
        else if (name == "right_front_leg") roles.rightFrontLeg = index;
        else if (name == "left_front_leg") roles.leftFrontLeg = index;
        else if (name == "right_middle_hind_leg") spider = true;
    }
    roles.legged = !spider;
    return roles;
}

// Whole-body posture. Treading is slow swimming: upright, kicking and paddling.
enum class Posture : uint8_t { Standing, Crouching, Swimming, Treading, Riding };

// Vanilla's WalkAnimationState: position advances by speed each tick, and
// speed eases toward 4x the distance moved (capped at 1).
struct WalkState {
    float position = 0.0f;
    float speed = 0.0f;
    glm::vec3 lastPosition{ 0.0f };
    std::chrono::steady_clock::time_point lastTime{};
    uint64_t lastSeenFrame = 0;
    bool valid = false;

    void Update(glm::vec3 current, std::chrono::steady_clock::time_point now)
    {
        float seconds = std::chrono::duration<float>(now - lastTime).count();
        if (!valid || seconds > 0.25f)
        {
            speed = 0.0f;
        }
        else if (seconds > 0.0f)
        {
            float ticks = seconds * 20.0f;
            float distancePerTick = glm::length(glm::vec2(current.x - lastPosition.x, current.z - lastPosition.z)) / ticks;
            float target = std::min(distancePerTick * 4.0f, 1.0f);
            speed += (target - speed) * (1.0f - std::pow(0.6f, ticks));
            position += speed * ticks;
        }
        valid = true;
        lastPosition = current;
        lastTime = now;
    }

    // Vanilla's swimAmount: eases toward 1 while swimming and back to 0 at 0.09 per tick.
    float swimAmount = 0.0f;
    std::chrono::steady_clock::time_point lastSwimTime{};

    // Posture as the last real draw posed it, for the shadow pass to match.
    Posture posture = Posture::Standing;
    float headXRot = 0.0f;
    bool riding = false;
    bool ridingBoat = false;

    float EaseSwim(bool swimming, std::chrono::steady_clock::time_point now)
    {
        float seconds = std::min(std::chrono::duration<float>(now - lastSwimTime).count(), 0.25f);
        lastSwimTime = now;
        float step = 0.09f * seconds * 20.0f;
        swimAmount = swimming ? std::min(swimAmount + step, 1.0f) : std::max(swimAmount - step, 0.0f);
        return swimAmount;
    }
};

// What a pose is computed from.
struct Inputs {
    Posture posture = Posture::Standing;
    float swimAmount = 0.0f;   // 0..1; tilts the body flat for Posture::Swimming.
    bool riding = false;       // Seated; the walk cycle is off.
    bool ridingBoat = false;   // Seated in a boat rather than on a mount.
    float walkPosition = 0.0f;
    float walkSpeed = 0.0f;
    float attackTime = 0.0f;   // 0 (idle) .. 1 through a swing.
    float ageTicks = 0.0f;
    float headXRot = 0.0f;     // Vanilla's head xRot (pitch, down positive) and yRot (relative yaw), radians.
    float headYRot = 0.0f;
    bool holdingMainhand = false;
    bool holdingOffhand = false;
};

inline Pose ComputePose(const Roles& roles, const Inputs& in)
{
    Pose pose{};
    auto rot = [&](int part) -> glm::vec3* { return part >= 0 ? &pose[static_cast<size_t>(part)].rotation : nullptr; };
    auto off = [&](int part) -> glm::vec3* { return part >= 0 ? &pose[static_cast<size_t>(part)].offset : nullptr; };

    constexpr float PI = glm::pi<float>();
    float cycle = in.walkPosition * 0.6662f;
    bool seated = in.riding || in.posture == Posture::Riding;
    float walkSpeed = seated ? 0.0f : in.walkSpeed;
    float swim = in.posture == Posture::Swimming && !seated ? in.swimAmount : 0.0f;
    bool treading = in.posture == Posture::Treading && !seated;
    bool crouching = in.posture == Posture::Crouching && !seated;

    if (glm::vec3* head = rot(roles.head))
    {
        head->x = in.headXRot + (-PI / 4.0f - in.headXRot) * swim;
        head->y = in.headYRot;
    }

    // Four-legged walkers: diagonal pairs swing together.
    if (roles.legged)
    {
        float a = std::cos(cycle) * 1.4f * walkSpeed;
        float b = std::cos(cycle + PI) * 1.4f * walkSpeed;
        if (glm::vec3* r = rot(roles.rightHindLeg)) r->x = a;
        if (glm::vec3* r = rot(roles.leftHindLeg)) r->x = b;
        if (glm::vec3* r = rot(roles.rightFrontLeg)) r->x = b;
        if (glm::vec3* r = rot(roles.leftFrontLeg)) r->x = a;
    }

    // Two-legged walkers.
    glm::vec3* rightLeg = rot(roles.rightLeg);
    glm::vec3* leftLeg = rot(roles.leftLeg);
    if (rightLeg != nullptr) rightLeg->x = std::cos(cycle) * 1.4f * walkSpeed;
    if (leftLeg != nullptr) leftLeg->x = std::cos(cycle + PI) * 1.4f * walkSpeed;

    glm::vec3* rightArm = rot(roles.rightArm);
    glm::vec3* leftArm = rot(roles.leftArm);
    if (rightArm != nullptr) rightArm->x = std::cos(cycle + PI) * walkSpeed;
    if (leftArm != nullptr) leftArm->x = std::cos(cycle) * walkSpeed;

    // Arms rest slightly out from the torso (90 degrees makes a T pose, for testing).
    if (rightArm != nullptr) rightArm->z = ARM_ROLL;
    if (leftArm != nullptr) leftArm->z = -ARM_ROLL;

    // Treading water: a slow scissor kick and an alternating paddle.
    if (treading)
    {
        float kick = std::cos(in.ageTicks * 0.3f) * 0.5f;
        if (rightLeg != nullptr) rightLeg->x = kick;
        if (leftLeg != nullptr) leftLeg->x = -kick;
        float paddle = std::sin(in.ageTicks * 0.25f);
        if (rightArm != nullptr)
        {
            rightArm->x = -0.5f + paddle * 0.4f;
            rightArm->z = 0.35f;
        }
        if (leftArm != nullptr)
        {
            leftArm->x = -0.5f - paddle * 0.4f;
            leftArm->z = -0.35f;
        }
    }

    // Crawl: arms circle overhead in 26-tick strokes, legs flutter (vanilla's swimAmount blend).
    if (swim > 0.0f)
    {
        float strokePosition = std::fmod(in.walkPosition, 26.0f);
        float armX, armZ;
        if (strokePosition < 14.0f)
        {
            auto quadratic = [](float x) { return -65.0f * x + x * x; };
            armX = 0.0f;
            armZ = 1.8707964f * quadratic(strokePosition) / quadratic(14.0f);
        }
        else if (strokePosition < 22.0f)
        {
            armX = PI / 2.0f * (strokePosition - 14.0f) / 8.0f;
            armZ = 1.8707964f;
        }
        else
        {
            float t = (strokePosition - 22.0f) / 4.0f;
            armX = PI / 2.0f - PI / 2.0f * t;
            armZ = 1.8707964f * (1.0f - t);
        }

        // The swinging arm keeps its attack pose; the other joins the stroke.
        float rightWeight = in.attackTime > 0.0f ? 0.0f : swim;
        if (rightArm != nullptr)
        {
            rightArm->x += (armX - rightArm->x) * rightWeight;
            rightArm->y += (PI - rightArm->y) * rightWeight;
            rightArm->z += (PI - armZ - rightArm->z) * rightWeight;
        }
        if (leftArm != nullptr)
        {
            leftArm->x += (armX - leftArm->x) * swim;
            leftArm->y += (PI - leftArm->y) * swim;
            leftArm->z += (PI + armZ - leftArm->z) * swim;
        }
        if (rightLeg != nullptr) rightLeg->x += (0.3f * std::cos(in.walkPosition * 0.33333334f) - rightLeg->x) * swim;
        if (leftLeg != nullptr) leftLeg->x += (0.3f * std::cos(in.walkPosition * 0.33333334f + PI) - leftLeg->x) * swim;
    }

    // Seated: legs forward and apart (tighter in a boat), arms out front.
    if (seated)
    {
        float legSpread = in.ridingBoat ? 0.05f : PI / 10.0f;
        float legRoll = in.ridingBoat ? 0.03f : 0.07853982f;
        float legX = in.ridingBoat ? -1.3f : -1.4137167f;
        float armX = in.ridingBoat ? -0.7f : -PI / 5.0f;
        if (rightLeg != nullptr) *rightLeg = glm::vec3(legX, legSpread, legRoll);
        if (leftLeg != nullptr) *leftLeg = glm::vec3(legX, -legSpread, -legRoll);
        if (rightArm != nullptr) rightArm->x += armX;
        if (leftArm != nullptr) leftArm->x += armX;
    }

    // Sneaking: the torso leans, the limbs and head drop with it (vanilla's crouch offsets).
    if (crouching)
    {
        if (glm::vec3* r = rot(roles.body)) r->x = 0.5f;
        if (glm::vec3* o = off(roles.body)) o->y = 3.2f;
        if (glm::vec3* o = off(roles.head)) o->y = 4.2f;
        if (rightArm != nullptr) rightArm->x += 0.4f;
        if (leftArm != nullptr) leftArm->x += 0.4f;
        if (glm::vec3* o = off(roles.rightArm)) o->y = 3.2f;
        if (glm::vec3* o = off(roles.leftArm)) o->y = 3.2f;
        if (glm::vec3* o = off(roles.rightLeg)) *o = glm::vec3(0.0f, 0.2f, 4.0f);
        if (glm::vec3* o = off(roles.leftLeg)) *o = glm::vec3(0.0f, 0.2f, 4.0f);
    }

    // A held item lowers and steadies the arm.
    if (rightArm != nullptr && in.holdingMainhand && swim == 0.0f) rightArm->x = rightArm->x * 0.5f - PI / 10.0f;
    if (leftArm != nullptr && in.holdingOffhand && swim == 0.0f) leftArm->x = leftArm->x * 0.5f - PI / 10.0f;

    // Swing: the body twists and the main arm (right) chops.
    if (in.attackTime > 0.0f && rightArm != nullptr)
    {
        float bodyTurn = std::sin(std::sqrt(in.attackTime) * 2.0f * PI) * 0.2f;
        if (glm::vec3* body = rot(roles.body)) body->y = bodyTurn;

        // The shoulders swing around with the body (vanilla replaces their x/z).
        if (glm::vec3* o = off(roles.rightArm))
        {
            o->z = std::sin(bodyTurn) * 5.0f;
            o->x = -std::cos(bodyTurn) * 5.0f + 5.0f;
        }
        if (glm::vec3* o = off(roles.leftArm))
        {
            o->z = -std::sin(bodyTurn) * 5.0f;
            o->x = std::cos(bodyTurn) * 5.0f - 5.0f;
        }
        rightArm->y += bodyTurn;
        if (leftArm != nullptr)
        {
            leftArm->y += bodyTurn;
            leftArm->x += bodyTurn;
        }

        float eased = 1.0f - in.attackTime;
        eased = 1.0f - eased * eased * eased * eased;
        float chop = std::sin(eased * PI);
        float lift = std::sin(in.attackTime * PI) * -(in.headXRot - 0.7f) * 0.75f;
        rightArm->x -= chop * 1.2f + lift;
        rightArm->y += bodyTurn * 2.0f;
        rightArm->z += std::sin(in.attackTime * PI) * -0.4f;
    }

    // Idle arm sway.
    float sway = 1.0f - swim;
    if (rightArm != nullptr)
    {
        rightArm->z += (std::cos(in.ageTicks * 0.09f) * 0.05f + 0.05f) * sway;
        rightArm->x += std::sin(in.ageTicks * 0.067f) * 0.05f * sway;
    }
    if (leftArm != nullptr)
    {
        leftArm->z -= (std::cos(in.ageTicks * 0.09f) * 0.05f + 0.05f) * sway;
        leftArm->x -= std::sin(in.ageTicks * 0.067f) * 0.05f * sway;
    }
    return pose;
}

// Lays the whole posed body flat along the look direction when swimming:
// vanilla's setupRotations (turn about the feet, then slide back and up).
// Each part's rotation is composed with the tilt and its offset carries the
// pivot to where the tilt puts it. Works in the model frame (X axis shared
// with the world-local frame, so the angle is the same in both).
inline void TiltForSwim(Pose& pose, const std::vector<EntityJsonModel::PartInfo>& parts, const Inputs& in)
{
    if (in.posture != Posture::Swimming || in.riding || in.swimAmount <= 0.0f) return;

    constexpr float FEET_Y = 24.0f;
    float angle = in.swimAmount * (glm::half_pi<float>() + in.headXRot);
    glm::mat3 tilt(glm::rotate(glm::mat4(1.0f), angle, glm::vec3(1.0f, 0.0f, 0.0f)));
    glm::vec3 shift = tilt * glm::vec3(0.0f, -1.0f, -0.3f) * in.swimAmount;

    for (size_t i = 0; i < parts.size() && i < MAX_PARTS; i++)
    {
        PartPose& part = pose[i];
        glm::vec3 base = parts[i].pivotModel + part.offset;
        glm::vec3 world = glm::vec3(base.x, FEET_Y - base.y, -base.z) / 16.0f;
        glm::vec3 moved = tilt * world + shift;
        part.offset = glm::vec3(moved.x * 16.0f, FEET_Y - moved.y * 16.0f, -moved.z * 16.0f) - parts[i].pivotModel;

        // Rz * Ry * Rx, as the shader builds it, multiplied by the tilt and split back into angles.
        glm::mat4 own = glm::rotate(glm::mat4(1.0f), part.rotation.z, glm::vec3(0, 0, 1));
        own = glm::rotate(own, part.rotation.y, glm::vec3(0, 1, 0));
        own = glm::rotate(own, part.rotation.x, glm::vec3(1, 0, 0));
        glm::mat3 m = tilt * glm::mat3(own);
        float sinY = std::max(-1.0f, std::min(1.0f, -m[0][2]));
        part.rotation.y = std::asin(sinY);
        if (std::abs(sinY) > 0.9999f)
        {
            part.rotation.z = 0.0f;
            part.rotation.x = std::atan2(-m[2][1], m[1][1]);
        }
        else
        {
            part.rotation.z = std::atan2(m[0][1], m[0][0]);
            part.rotation.x = std::atan2(m[1][2], m[2][2]);
        }
    }
}

// The full pose: limbs, then the swim tilt over the mesh's parts.
inline Pose ComputePose(const Roles& roles, const std::vector<EntityJsonModel::PartInfo>& parts, const Inputs& in)
{
    Pose pose = ComputePose(roles, in);
    TiltForSwim(pose, parts, in);
    return pose;
}

// Vanilla ModelPart.translateAndRotate, in block units, in vanilla's model
// frame (Y down). Held items hang off this.
inline glm::mat4 PartMatrix(const EntityJsonModel::PartInfo& part, const PartPose& pose)
{
    glm::vec3 position = (part.pivotModel + pose.offset) / 16.0f;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m = glm::rotate(m, pose.rotation.z, glm::vec3(0, 0, 1));
    m = glm::rotate(m, pose.rotation.y, glm::vec3(0, 1, 0));
    m = glm::rotate(m, pose.rotation.x, glm::vec3(1, 0, 0));
    return m;
}

} // namespace Volcano::EntityAnimation

#endif
