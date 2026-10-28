#pragma once
#ifndef PLAYER_H
#define PLAYER_H

#include "renderer/Camera.hpp"
#include <atomic>
#include <glm/glm.hpp>

namespace Volcano {

class Player {
public:
    // Collision box, feet-centered: vanilla's 0.6 x 1.8 standing player.
    static constexpr float HALF_WIDTH = 0.3f;
    static constexpr float HEIGHT = 1.8f;

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
    // Untouched until MeshingThread applies the server's real spawn
    // position (see GlobalState::worldReady)
    std::atomic<float> x{0.0f}, y{0.0f}, z{0.0f};
};

} // namespace Volcano

#endif
