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
    // Vertical eye offset above the feet — eased between standing/sneaking
    // height every fixed step by TickLoop::FixedStep (NetworkThread) and
    // read every frame by GetEyePosition/GetViewMatrix (RenderThread).
    // Atomic for the same cross-thread reason yaw/pitch below are, now that
    // TickLoop runs on NetworkThread instead of sharing RenderThread's own
    // thread — was a plain glm::vec3 (x/z always 0) back when both sides
    // ran on the same thread.
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
