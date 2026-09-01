#pragma once
#ifndef CHUNK_H
#define CHUNK_H

#include <vector>
#include <memory>
#include "Block.hpp"

namespace Volcano {

constexpr int CHUNK_SIZE_X = 16;
constexpr int CHUNK_SIZE_Y = 64;
constexpr int CHUNK_SIZE_Z = 16;

class Chunk {
public:
    Chunk(int chunkX, int chunkZ) : x(chunkX), z(chunkZ) {
        blocks.resize(CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z, {BlockType::Air});
    }

    int getX() const { return x; }
    int getZ() const { return z; }

    Block getBlock(int bx, int by, int bz) const {
        if (bx < 0 || bx >= CHUNK_SIZE_X || 
            by < 0 || by >= CHUNK_SIZE_Y || 
            bz < 0 || bz >= CHUNK_SIZE_Z) {
            return {BlockType::Air}; // Simple out-of-bounds check for now
        }
        return blocks[bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z];
    }

    void setBlock(int bx, int by, int bz, BlockType type) {
        if (bx >= 0 && bx < CHUNK_SIZE_X && 
            by >= 0 && by < CHUNK_SIZE_Y && 
            bz >= 0 && bz < CHUNK_SIZE_Z) {
            blocks[bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z] = {type};
        }
    }

private:
    int x, z; // Chunk coordinates
    std::vector<Block> blocks;
};

} // namespace Volcano

#endif

