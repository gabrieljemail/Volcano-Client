#pragma once
#ifndef BLOCK_H
#define BLOCK_H

#include <cstdint>

namespace Volcano {

// TODO: This should be based on the MC protocol, not constants.
enum class BlockType : uint8_t {
    Air = 0,
    Stone = 1,
    Dirt = 2,
    Grass = 3,
    Wood = 4,
    Leaves = 5
};

struct Block {
    BlockType type;

    // Quick helper for meshing
    bool isOpaque() const {
        return type != BlockType::Air; // In the future, leaves/glass might be non-opaque
    }
};

} // namespace Volcano

#endif
