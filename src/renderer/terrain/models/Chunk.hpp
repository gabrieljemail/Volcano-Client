#pragma once
#ifndef CHUNK_H
#define CHUNK_H

#include <vector>
#include <memory>
#include "Block.hpp"

namespace Volcano {

constexpr int CHUNK_SIZE_X = 16;
constexpr int CHUNK_SIZE_Y = 384;
constexpr int CHUNK_SIZE_Z = 16;

// World Y of the bottom of a chunk (local Y 0). Real overworld chunks span
// world Y -64..319 (24 sections of 16), stored here as local Y 0..383.
constexpr int WORLD_MIN_Y = -64;

class Chunk {
public:
    Chunk(int chunkX, int chunkZ) : x(chunkX), z(chunkZ) {
        blocks.resize(CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z, Block{});
    }

    int getX() const { return x; }
    int getZ() const { return z; }

    Block getBlock(int bx, int by, int bz) const {
        if (bx < 0 || bx >= CHUNK_SIZE_X ||
            by < 0 || by >= CHUNK_SIZE_Y ||
            bz < 0 || bz >= CHUNK_SIZE_Z) {
            return Block{}; // Simple out-of-bounds check for now
        }
        return blocks[bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z];
    }

    void setBlock(int bx, int by, int bz, uint16_t visualId) {
        if (bx >= 0 && bx < CHUNK_SIZE_X &&
            by >= 0 && by < CHUNK_SIZE_Y &&
            bz >= 0 && bz < CHUNK_SIZE_Z) {
            blocks[bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z] = Block{visualId};
        }
    }

    // Full-block overload, for callers (ChunkParser) that also resolved a
    // non-cube visual/collision flag — the uint16_t overload above only ever
    // produces a plain opaque-or-nothing Block.
    void setBlock(int bx, int by, int bz, const Block& block) {
        if (bx >= 0 && bx < CHUNK_SIZE_X &&
            by >= 0 && by < CHUNK_SIZE_Y &&
            bz >= 0 && bz < CHUNK_SIZE_Z) {
            blocks[bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z] = block;
        }
    }

private:
    int x, z; // Chunk coordinates
    std::vector<Block> blocks;
};

} // namespace Volcano

#endif

