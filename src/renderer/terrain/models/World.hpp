#pragma once
#ifndef VOLCANO_WORLD_H
#define VOLCANO_WORLD_H

#include <cstdint>
#include <shared_mutex>
#include <unordered_map>
#include "Chunk.hpp"

namespace Volcano {

// World/chunk store. Written from the main thread (placeholder generation
// at startup, and network-thread-parsed chunks handed off and inserted from
// the main thread — see GlobalState::NetworkInbox) and read every tick from
// RenderThread's thread via TickLoop::Advance's collision checks, so the
// chunk map itself is guarded by a shared_mutex: readers (GetBlock) take a
// shared lock, writers (GetOrCreateChunk/InsertChunk) take a unique lock.
// Individual Chunk contents are not separately synchronized — nothing
// mutates a Chunk after it's been inserted/looked up in this pass.
class World {
public:
    Chunk& GetOrCreateChunk(int chunkX, int chunkZ)
    {
        std::unique_lock lock(mutex);
        int64_t key = ChunkKey(chunkX, chunkZ);
        auto it = chunks.find(key);
        if (it != chunks.end()) return it->second;
        return chunks.emplace(key, Chunk(chunkX, chunkZ)).first->second;
    }

    // Inserts a fully-populated chunk (e.g. parsed from a network packet),
    // replacing any existing chunk at the same coordinates.
    Chunk& InsertChunk(int chunkX, int chunkZ, Chunk&& chunk)
    {
        std::unique_lock lock(mutex);
        int64_t key = ChunkKey(chunkX, chunkZ);
        chunks.erase(key);
        return chunks.emplace(key, std::move(chunk)).first->second;
    }

    // Solid-block lookup for collision, in world-block coordinates (not
    // chunk-local). Air (including chunks that don't exist) reads as Air,
    // matching Chunk::getBlock's own out-of-bounds behavior.
    Block GetBlock(int worldX, int worldY, int worldZ) const
    {
        int chunkX = FloorDiv(worldX, CHUNK_SIZE_X);
        int chunkZ = FloorDiv(worldZ, CHUNK_SIZE_Z);

        std::shared_lock lock(mutex);
        auto it = chunks.find(ChunkKey(chunkX, chunkZ));
        if (it == chunks.end()) return Block{};

        int localX = worldX - chunkX * CHUNK_SIZE_X;
        int localZ = worldZ - chunkZ * CHUNK_SIZE_Z;
        return it->second.getBlock(localX, worldY - WORLD_MIN_Y, localZ);
    }

private:
    mutable std::shared_mutex mutex;
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
