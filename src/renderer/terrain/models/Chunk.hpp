#pragma once
#ifndef CHUNK_H
#define CHUNK_H

#include <vector>
#include <memory>
#include <algorithm>
#include "Block.hpp"

namespace Volcano {

constexpr int CHUNK_SIZE_X = 16;
constexpr int CHUNK_SIZE_Y = 384;
constexpr int CHUNK_SIZE_Z = 16;

// World Y of the bottom of a chunk (local Y 0). Real overworld chunks span
// world Y -64..319 (24 sections of 16), stored here as local Y 0..383.
constexpr int WORLD_MIN_Y = -64;

// Sky and block light kept apart (rather than pre-merged into one value),
// so the renderer can tell "lit by open sky" from "lit by an actual light
// source" — see terrain.frag/misc.frag's own comment on why that split
// drives the bloom-style brightness cap. GetLightAcrossChunks in each mesher
// returns one of these per lookup instead of two separate calls, so a
// cross-chunk sample only pays for one World lookup, not two.
struct LightSample {
    uint8_t sky = 15;
    uint8_t block = 0;
};

class Chunk {
public:
    Chunk(int chunkX, int chunkZ) : x(chunkX), z(chunkZ) {
        blocks.resize(CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z, Block{});
        // Full-bright sky default: placeholder-generated chunks (no server
        // light data ever arrives for them) and any section a light packet
        // leaves uncovered should still render lit rather than going pitch
        // black. Block light defaults to 0 (no artificial light) — nothing
        // should glow until the server actually says something emits light
        // there.
        skyLight.resize(CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z, 15);
        blockLight.resize(CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z, 0);
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

    // 0-15 each, kept separate — see LightSample's own comment.
    uint8_t getSkyLight(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return 15; // Out-of-bounds reads as full-bright, matching the default fill.
        return skyLight[Index(bx, by, bz)];
    }

    uint8_t getBlockLight(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return 0;
        return blockLight[Index(bx, by, bz)];
    }

    // Both channels in one call — see LightSample's own comment for why this
    // beats calling the two getters above separately when both are needed.
    LightSample getLightSample(int bx, int by, int bz) const {
        if (!InBounds(bx, by, bz)) return LightSample{};
        size_t index = Index(bx, by, bz);
        return LightSample{ skyLight[index], blockLight[index] };
    }

    // Sky light is set directly (ChunkParser applies it whole-section, so
    // there's nothing to combine with yet); block light is applied after
    // and only ever raises the value (a torch in an otherwise-dark cave) —
    // both channels have their own array now, so this max() is just
    // defensive against the same cell being touched twice, not a merge
    // between the two channels the way it used to be.
    void setSkyLight(int bx, int by, int bz, uint8_t value) {
        if (InBounds(bx, by, bz)) skyLight[Index(bx, by, bz)] = value;
    }

    void applyBlockLight(int bx, int by, int bz, uint8_t value) {
        if (InBounds(bx, by, bz)) {
            uint8_t& cell = blockLight[Index(bx, by, bz)];
            cell = std::max(cell, value);
        }
    }

private:
    int x, z; // Chunk coordinates
    std::vector<Block> blocks;
    std::vector<uint8_t> skyLight;
    std::vector<uint8_t> blockLight;

    static bool InBounds(int bx, int by, int bz) {
        return bx >= 0 && bx < CHUNK_SIZE_X && by >= 0 && by < CHUNK_SIZE_Y && bz >= 0 && bz < CHUNK_SIZE_Z;
    }

    static size_t Index(int bx, int by, int bz) {
        return static_cast<size_t>(bx + bz * CHUNK_SIZE_X + by * CHUNK_SIZE_X * CHUNK_SIZE_Z);
    }
};

} // namespace Volcano

#endif

