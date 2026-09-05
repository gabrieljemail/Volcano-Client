#pragma once
#ifndef CAMERA_H
#define CAMERA_H

#include <atomic>
#include <algorithm>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Volcano {

class Camera {
public:
    glm::vec3 eyeOffset{0.0f, 1.62f, 0.0f}; // standing eye height above feet

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

    glm::vec3 GetEyePosition(glm::vec3 playerFeetPosition) const
    {
        return playerFeetPosition + eyeOffset;
    }

    glm::mat4 GetViewMatrix(glm::vec3 playerFeetPosition) const
    {
        glm::vec3 eye = GetEyePosition(playerFeetPosition);
        float y = yaw.load(std::memory_order_relaxed);
        float p = pitch.load(std::memory_order_relaxed);

        glm::vec3 forward{
            cos(glm::radians(p)) * sin(glm::radians(y)),
            sin(glm::radians(p)),
            cos(glm::radians(p)) * cos(glm::radians(y))
        };
        return glm::lookAt(eye, eye + forward, glm::vec3(0, 1, 0));
    }

private:
    std::atomic<float> yaw{0.0f};
    std::atomic<float> pitch{0.0f};
};

} // namespace Volcano

#endif
