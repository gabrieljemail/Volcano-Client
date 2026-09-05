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
    // Spawn position in center of 4-chunk grid (chunks 0-1 in both axes)
    // Each chunk is 16 blocks, so center is at (16, 5, 16) to see all terrain
    std::atomic<float> x{16.0f}, y{5.0f}, z{16.0f};
};

} // namespace Volcano

#endif
