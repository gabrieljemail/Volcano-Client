#pragma once
#ifndef PLAYER_H
#define PLAYER_H

#include "renderer/Camera.hpp"
#include <atomic>
#include <glm/glm.hpp>

namespace Volcano {

class Player {
public:
    Camera camera;

    // Position stored as three atomics per your spec's "high-frequency
    // updates use atomics" rule. Not a single atomic<glm::vec3> since
    // that type isn't lock-free on most platforms.
    glm::vec3 GetPosition() const
    {
        return { x.load(std::memory_order_relaxed),
                 y.load(std::memory_order_relaxed),
                 z.load(std::memory_order_relaxed) };
    }

    void SetPosition(glm::vec3 pos)
    {
        x.store(pos.x, std::memory_order_relaxed);
        y.store(pos.y, std::memory_order_relaxed);
        z.store(pos.z, std::memory_order_relaxed);
    }

private:
    // Spawn just outside the test chunk (which occupies x:[0,16), z:[0,16), y:[0,4)),
    // at block height, facing +Z toward it. Mouse-look isn't wired up yet (yaw/pitch
    // stay 0), so this position/orientation is chosen so the default forward view
    // actually looks straight at the test geometry.
    std::atomic<float> x{8.0f}, y{2.0f}, z{-5.0f};
};

} // namespace Volcano

#endif
