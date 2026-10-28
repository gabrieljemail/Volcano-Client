#pragma once
#ifndef VOLCANO_LIGHT_ENGINE_H
#define VOLCANO_LIGHT_ENGINE_H

#include <mutex>
#include <vector>
#include <glm/glm.hpp>
#include "models/Block.hpp"
#include "models/ChunkSection.hpp"
#include "models/World.hpp"

namespace Volcano {

// Client-side light for block changes, like vanilla's own light engine on the
// client: breaking or placing a block re-spreads sky and block light around it
// right away, and the server's Update Light packet corrects it afterwards. The
// protocol has no request for light; the server sends it unprompted.
class LightEngine {
public:
    explicit LightEngine(World* world) : world(world) {}

    // Re-lights around `position` after oldBlock became newBlock (already
    // written to the world) and returns the mesh sections that need a rebuild.
    std::vector<SectionKey> OnBlockChanged(glm::ivec3 position, const Block& oldBlock, const Block& newBlock);

private:
    struct Cell {
        glm::ivec3 position;
        uint8_t level;
    };

    void Relight(bool sky, glm::ivec3 position, const Block& newBlock);

    uint8_t Get(bool sky, glm::ivec3 position) const;
    void Set(bool sky, glm::ivec3 position, uint8_t value);
    bool Loaded(glm::ivec3 position);

    World* world;
    std::mutex mutex; // Called from the interaction and meshing threads; the scratch members below are shared.
    std::vector<glm::ivec3> changed; // Cells written during the current OnBlockChanged.
    int visits = 0;
};

} // namespace Volcano

#endif
