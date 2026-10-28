#pragma once
#ifndef VOLCANO_FLUID_QUERY_H
#define VOLCANO_FLUID_QUERY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include "renderer/terrain/models/BlockRegistry.hpp"
#include "renderer/terrain/models/World.hpp"

namespace Volcano {

// Fluid lookups for the player's physics and sounds. Fluids come from
// BlockRegistry's Liquid visuals (waterlogged blocks are not modeled).
// Reads World, so call from any thread World::GetBlock is safe on.
class FluidQuery {
public:
    static constexpr uint8_t NONE = 0;
    static constexpr uint8_t WATER = 1;
    static constexpr uint8_t LAVA = 2;

    struct Cell
    {
        uint8_t kind = NONE;
        float height = 0.0f; // Surface height within the cell; 1 under more of the same fluid.
    };

    explicit FluidQuery(const World* world) : world(world) {}

    Cell At(int x, int y, int z) const
    {
        Cell cell = Read(x, y, z);
        if (cell.kind != NONE && Read(x, y + 1, z).kind == cell.kind) cell.height = 1.0f;
        return cell;
    }

    // Fraction (0-1) of a box `height` tall standing at `feet` that is under
    // `kind`, sampled down the box's centre column.
    float SubmergedFraction(glm::vec3 feet, float height, uint8_t kind = WATER) const
    {
        constexpr float SHRINK = 0.001f; // Vanilla's box deflation, so touching a surface is not "in" it.
        float bottom = feet.y + SHRINK;
        float top = feet.y + height - SHRINK;
        int x = static_cast<int>(std::floor(feet.x));
        int z = static_cast<int>(std::floor(feet.z));

        float submerged = 0.0f;
        for (int y = static_cast<int>(std::floor(bottom)); y <= static_cast<int>(std::floor(top)); y++)
        {
            Cell cell = At(x, y, z);
            if (cell.kind != kind) continue;
            float low = std::max(static_cast<float>(y), bottom);
            float high = std::min(static_cast<float>(y) + cell.height, top);
            if (high > low) submerged += high - low;
        }
        return std::clamp(submerged / std::max(top - bottom, 0.001f), 0.0f, 1.0f);
    }

    // Whether a point (an eye position) is inside `kind`.
    bool Contains(glm::vec3 point, uint8_t kind = WATER) const
    {
        int y = static_cast<int>(std::floor(point.y));
        Cell cell = At(static_cast<int>(std::floor(point.x)), y, static_cast<int>(std::floor(point.z)));
        return cell.kind == kind && point.y < static_cast<float>(y) + cell.height;
    }

private:
    Cell Read(int x, int y, int z) const
    {
        Cell cell;
        Block block = world->GetBlock(x, y, z);
        if (block.nonCubeVisualId == 0) return cell;
        const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
        if (visual.shape != BlockRegistry::NonCubeShape::Liquid) return cell;
        cell.kind = visual.fluidKind;
        cell.height = visual.fluidHeight;
        return cell;
    }

    const World* world;
};

} // namespace Volcano

#endif
