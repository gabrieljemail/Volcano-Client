#pragma once
#ifndef VOLCANO_WORLD_H
#define VOLCANO_WORLD_H

#include <cstdint>
#include <unordered_map>
#include "Chunk.hpp"

namespace Volcano {

// Placeholder world/chunk store: keeps chunks alive and queryable (for
// TickLoop's collision checks) instead of the current pattern of building a
// Chunk, meshing it, and letting it fall out of scope. Single-threaded —
// only the render thread touches this right now. The real networked World
// (populated by the server, guarded by a shared_mutex for the network
// thread to write into) is a separate, later piece — this one is
// deliberately simple.
class World {
public:
    Chunk& GetOrCreateChunk(int chunkX, int chunkZ)
    {
        int64_t key = ChunkKey(chunkX, chunkZ);
        auto it = chunks.find(key);
        if (it != chunks.end()) return it->second;
        return chunks.emplace(key, Chunk(chunkX, chunkZ)).first->second;
    }

    // Solid-block lookup for collision, in world-block coordinates (not
    // chunk-local). Air (including chunks that don't exist) reads as Air,
    // matching Chunk::getBlock's own out-of-bounds behavior.
    Block GetBlock(int worldX, int worldY, int worldZ) const
    {
        int chunkX = FloorDiv(worldX, CHUNK_SIZE_X);
        int chunkZ = FloorDiv(worldZ, CHUNK_SIZE_Z);

        auto it = chunks.find(ChunkKey(chunkX, chunkZ));
        if (it == chunks.end()) return {BlockType::Air};

        int localX = worldX - chunkX * CHUNK_SIZE_X;
        int localZ = worldZ - chunkZ * CHUNK_SIZE_Z;
        return it->second.getBlock(localX, worldY, localZ);
    }

private:
    std::unordered_map<int64_t, Chunk> chunks;

    static int64_t ChunkKey(int chunkX, int chunkZ)
    {
        return (static_cast<int64_t>(chunkX) << 32) | (static_cast<uint32_t>(chunkZ));
    }

    static int FloorDiv(int value, int divisor)
    {
        int q = value / divisor;
        if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) q--;
        return q;
    }
};

} // namespace Volcano

#endif
