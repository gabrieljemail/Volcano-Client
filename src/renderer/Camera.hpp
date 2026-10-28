#pragma once
#ifndef CAMERA_H
#define CAMERA_H

#include <atomic>
#include <cstdint>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Volcano {

enum class Perspective : uint8_t {
    FIRST_PERSON,
    THIRD_PERSON,       // Behind the player, looking the way they look.
    THIRD_PERSON_FRONT, // In front of the player, looking back at them.
};

class Camera {
public:
    static constexpr float THIRD_PERSON_DISTANCE = 4.0f;

    std::atomic<Perspective> perspective{Perspective::FIRST_PERSON};

    Perspective GetPerspective() const { return perspective.load(std::memory_order_relaxed); }
    bool IsFirstPerson() const { return GetPerspective() == Perspective::FIRST_PERSON; }

    // F5 order: first person -> behind -> in front -> first person.
    void CyclePerspective()
    {
        switch (GetPerspective())
        {
            case Perspective::FIRST_PERSON: perspective.store(Perspective::THIRD_PERSON); break;
            case Perspective::THIRD_PERSON: perspective.store(Perspective::THIRD_PERSON_FRONT); break;
            default: perspective.store(Perspective::FIRST_PERSON); break;
        }
    }

    // Eased between standing/sneaking height by TickLoop::FixedStep, read by
    // GetEyePosition/GetViewMatrix - both on RenderThread now, but kept
    // atomic since that's cheap and not guaranteed to stay true.
    std::atomic<float> eyeHeight{1.62f}; // standing eye height above feet

    void ApplyMouseDelta(float dx, float dy, float sensitivity)
    {
        yaw.fetch_add(dx * sensitivity, std::memory_order_relaxed);
        float newPitch = std::clamp(pitch.load(std::memory_order_relaxed) + dy * sensitivity,
                                     -89.0f, 89.0f);
        pitch.store(newPitch, std::memory_order_relaxed);
    }

    // Yaw only (no pitch): the horizontal-plane heading used to project
    // WASD-style movement input into world-space forward/right vectors.
    float GetYaw() const { return yaw.load(std::memory_order_relaxed); }
    float GetPitch() const { return pitch.load(std::memory_order_relaxed); }

    glm::vec3 GetEyePosition(glm::vec3 playerFeetPosition) const
    {
        return playerFeetPosition + glm::vec3(0.0f, eyeHeight.load(std::memory_order_relaxed), 0.0f);
    }

    // Where the view is rendered from: the eye in first person, otherwise
    // pulled out along the look direction. GetEyePosition stays the real eye
    // (raycasts and the like use it).
    glm::vec3 GetCameraPosition(glm::vec3 playerFeetPosition) const
    {
        glm::vec3 eye = GetEyePosition(playerFeetPosition);
        switch (GetPerspective())
        {
            case Perspective::THIRD_PERSON: return eye - GetForward() * THIRD_PERSON_DISTANCE;
            case Perspective::THIRD_PERSON_FRONT: return eye + GetForward() * THIRD_PERSON_DISTANCE;
            default: return eye;
        }
    }

    glm::mat4 GetViewMatrix(glm::vec3 playerFeetPosition) const
    {
        glm::vec3 eye = GetEyePosition(playerFeetPosition);
        if (IsFirstPerson()) return glm::lookAt(eye, eye + GetForward(), glm::vec3(0, 1, 0));
        return glm::lookAt(GetCameraPosition(playerFeetPosition), eye, glm::vec3(0, 1, 0));
    }

private:
    glm::vec3 GetForward() const
    {
        float y = yaw.load(std::memory_order_relaxed);
        float p = pitch.load(std::memory_order_relaxed);
        return glm::vec3{
            cos(glm::radians(p)) * sin(glm::radians(y)),
            sin(glm::radians(p)),
            cos(glm::radians(p)) * cos(glm::radians(y))
        };
    }

    std::atomic<float> yaw{0.0f};
    std::atomic<float> pitch{0.0f};
};

} // namespace Volcano

#endif
