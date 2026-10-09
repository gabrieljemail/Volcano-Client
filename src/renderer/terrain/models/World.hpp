#pragma once
#ifndef VOLCANO_WORLD_H
#define VOLCANO_WORLD_H

#include <cstdint>
#include <algorithm>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "Chunk.hpp"
#include "ChunkSection.hpp"

namespace Volcano {

// World/chunk store. Written from the main thread (placeholder generation
// at startup, and network-thread-parsed chunks handed off and inserted from
// the main thread — see GlobalState::NetworkInbox) and read every tick from
// RenderThread via TickLoop::Tick's collision checks, so the
// chunk map itself is guarded by a shared_mutex: readers (GetBlock) take a
// shared lock, writers (GetOrCreateChunk/InsertChunk/SetBlock) take a
// unique lock. Individual Chunk contents have no separate synchronization
// of their own — SetBlock's in-place mutation is safe only because it also
// takes this same unique lock (blocking every reader world-wide for the
// duration, not just the one chunk being edited; edits are rare enough
// player-triggered events that this coarseness doesn't matter).
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

    // Drops one chunk (server Unload Chunk). No-op if it isn't loaded.
    void RemoveChunk(int chunkX, int chunkZ)
    {
        std::unique_lock lock(mutex);
        chunks.erase(ChunkKey(chunkX, chunkZ));
    }

    bool HasChunk(int chunkX, int chunkZ) const
    {
        std::shared_lock lock(mutex);
        return chunks.find(ChunkKey(chunkX, chunkZ)) != chunks.end();
    }

    // Drops every chunk — used when returning to the connect screen after a
    // disconnect, so a fresh connection doesn't layer new chunks on top of
    // the previous session's leftover world.
    void Clear()
    {
        std::unique_lock lock(mutex);
        chunks.clear();
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

    bool HasBlockEntity(glm::ivec3 worldPosition) const
    {
        glm::ivec3 chunkPos = ChunkOf(worldPosition);
        glm::ivec3 local = LocalOf(worldPosition);

        std::shared_lock lock(mutex);
        auto it = chunks.find(ChunkKey(chunkPos.x, chunkPos.z));
        return it != chunks.end() && it->second.HasBlockEntity(local.x, local.y, local.z);
    }

    // Same shape as GetBlock, for the meshers' cross-chunk light lookups.
    // Both channels in one lookup/lock — see LightSample's own comment.
    LightSample GetLightSample(int worldX, int worldY, int worldZ) const
    {
        int chunkX = FloorDiv(worldX, CHUNK_SIZE_X);
        int chunkZ = FloorDiv(worldZ, CHUNK_SIZE_Z);

        std::shared_lock lock(mutex);
        auto it = chunks.find(ChunkKey(chunkX, chunkZ));
        if (it == chunks.end()) return LightSample{}; // No chunk loaded there — same default as Chunk's own.

        int localX = worldX - chunkX * CHUNK_SIZE_X;
        int localZ = worldZ - chunkZ * CHUNK_SIZE_Z;
        return it->second.getLightSample(localX, worldY - WORLD_MIN_Y, localZ);
    }

    // Read access to an already-loaded chunk, for MeshingThread re-meshing
    // a dirty section (SetBlock's caller already has the chunk coordinates,
    // not a Chunk — the dirty-remesh queue carries only SectionKeys, not
    // chunk data, since the chunk itself already lives here). nullptr if
    // nothing is loaded at that column.
    const Chunk* GetChunk(int chunkX, int chunkZ) const
    {
        std::shared_lock lock(mutex);
        auto it = chunks.find(ChunkKey(chunkX, chunkZ));
        return it == chunks.end() ? nullptr : &it->second;
    }

    // Mutates one block in an already-loaded chunk and reports which mesh
    // sections need to be rebuilt as a result. No-op (empty return) if
    // `localPosition` is out of the chunk's 16x384x16 bounds or the chunk
    // isn't loaded — there's nothing to edit or remesh either way.
    //
    // `chunkPos` uses this codebase's usual world-axis convention (X/Z
    // horizontal, Y vertical) even though a chunk COLUMN only has an X/Z
    // identity — chunkPos.y is simply unused; the section's own vertical
    // index comes from localPosition.y instead. (glm::ivec3, not vec3 as
    // originally asked — chunk/block indices must be integers, matching
    // every other lookup in this file.)
    //
    // Beyond the edited section itself, a boundary edit invalidates a
    // neighbor's already-built mesh too, the same way the greedy sweep
    // already samples one step past chunk/section edges when building a
    // mesh in the first place: the section above/below if the edit was on
    // a 16-row boundary, and the neighboring CHUNK's same-Y section if the
    // edit was on an X/Z chunk edge (local x/z 0 or 15).
    //
    // Returns SectionKeys rather than enqueueing them itself — World has no
    // GlobalState/queue access, so the caller (e.g. BreakBlockEvent, or
    // MeshingThread draining a server Block Update correction — see
    // NetworkInbox::pendingBlockEdits) is responsible for pushing them into
    // GlobalState::NetworkInbox::dirtySections.
    //
    // uint16_t overload: visualId only ("produces a plain opaque-or-nothing
    // Block" — see Chunk::setBlock's own comment), enough for breaking a
    // block (visualId 0 = air). The Block overload below sets the full
    // visual/collision state, for anything that isn't just "air" (a server
    // Block Update correcting a mispredicted edit to some other block).
    std::vector<SectionKey> SetBlock(glm::ivec3 chunkPos, glm::ivec3 localPosition, uint16_t newState)
    {
        return SetBlockImpl(chunkPos, localPosition, [&](Chunk& chunk) {
            chunk.setBlock(localPosition.x, localPosition.y, localPosition.z, newState);
        });
    }

    std::vector<SectionKey> SetBlock(glm::ivec3 chunkPos, glm::ivec3 localPosition, const Block& newBlock)
    {
        return SetBlockImpl(chunkPos, localPosition, [&](Chunk& chunk) {
            chunk.setBlock(localPosition.x, localPosition.y, localPosition.z, newBlock);
        });
    }

    // World-block-coordinate convenience overloads — floor-divs into
    // chunk+local the same way GetBlock does, for callers that only have a
    // world position (e.g. a Block Update packet's own coordinates).
    std::vector<SectionKey> SetBlock(glm::ivec3 worldPosition, uint16_t newState)
    {
        return SetBlock(ChunkOf(worldPosition), LocalOf(worldPosition), newState);
    }

    std::vector<SectionKey> SetBlock(glm::ivec3 worldPosition, const Block& newBlock)
    {
        return SetBlock(ChunkOf(worldPosition), LocalOf(worldPosition), newBlock);
    }

    // Applies changed light sections and invalidates meshes that sample their
    // boundary values, including adjacent chunk columns and vertical sections.
    std::vector<SectionKey> ApplyLightUpdate(const PendingLightUpdate& update)
    {
        std::vector<SectionKey> dirty;

        std::unique_lock lock(mutex);
        auto it = chunks.find(ChunkKey(update.chunkX, update.chunkZ));
        if (it == chunks.end()) return dirty;

        Chunk& chunk = it->second;
        auto addDirty = [&](int chunkX, int chunkZ, int sectionY)
        {
            if (sectionY < 0 || sectionY >= SECTIONS_PER_CHUNK) return;
            SectionKey key{chunkX, chunkZ, sectionY};
            if (std::find(dirty.begin(), dirty.end(), key) == dirty.end()) dirty.push_back(key);
        };

        for (int section = 0; section < SECTIONS_PER_CHUNK; section++)
        {
            const std::vector<uint8_t>& sky = update.skyLight[static_cast<size_t>(section)];
            const std::vector<uint8_t>& block = update.blockLight[static_cast<size_t>(section)];
            if (sky.size() < 2048 && block.size() < 2048) continue;

            int baseY = section * SECTION_SIZE_Y;
            bool changed = false;
            bool lowerEdgeChanged = false, upperEdgeChanged = false;
            bool westEdgeChanged = false, eastEdgeChanged = false;
            bool northEdgeChanged = false, southEdgeChanged = false;
            for (int i = 0; i < 4096; i++)
            {
                int x = i % 16, y = baseY + i / 256, z = (i / 16) % 16;
                auto nibble = [i](const std::vector<uint8_t>& data) {
                    return (i % 2 == 0) ? (data[static_cast<size_t>(i / 2)] & 0x0Fu) : (data[static_cast<size_t>(i / 2)] >> 4);
                };
                LightSample previous = chunk.getLightSample(x, y, z);
                uint8_t newSky = sky.size() >= 2048 ? static_cast<uint8_t>(nibble(sky)) : previous.sky;
                uint8_t newBlock = block.size() >= 2048 ? static_cast<uint8_t>(nibble(block)) : previous.block;
                if (newSky == previous.sky && newBlock == previous.block) continue;

                changed = true;
                if (y == baseY) lowerEdgeChanged = true;
                if (y == baseY + SECTION_SIZE_Y - 1) upperEdgeChanged = true;
                if (x == 0) westEdgeChanged = true;
                if (x == CHUNK_SIZE_X - 1) eastEdgeChanged = true;
                if (z == 0) northEdgeChanged = true;
                if (z == CHUNK_SIZE_Z - 1) southEdgeChanged = true;
                if (sky.size() >= 2048) chunk.setSkyLight(x, y, z, newSky);
                if (block.size() >= 2048) chunk.setBlockLight(x, y, z, newBlock);
            }
            if (!changed) continue;

            addDirty(update.chunkX, update.chunkZ, section);
            if (lowerEdgeChanged) addDirty(update.chunkX, update.chunkZ, section - 1);
            if (upperEdgeChanged) addDirty(update.chunkX, update.chunkZ, section + 1);

            auto addNeighbor = [&](int neighborX, int neighborZ)
            {
                if (chunks.find(ChunkKey(neighborX, neighborZ)) == chunks.end()) return;
                addDirty(neighborX, neighborZ, section);
                if (lowerEdgeChanged) addDirty(neighborX, neighborZ, section - 1);
                if (upperEdgeChanged) addDirty(neighborX, neighborZ, section + 1);
            };
            if (westEdgeChanged) addNeighbor(update.chunkX - 1, update.chunkZ);
            if (eastEdgeChanged) addNeighbor(update.chunkX + 1, update.chunkZ);
            if (northEdgeChanged) addNeighbor(update.chunkX, update.chunkZ - 1);
            if (southEdgeChanged) addNeighbor(update.chunkX, update.chunkZ + 1);
        }
        return dirty;
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

    // chunkPos.y (see SetBlock's own comment on that field being unused) is
    // always 0 here since a chunk column has no Y identity of its own.
    static glm::ivec3 ChunkOf(glm::ivec3 worldPosition)
    {
        return { FloorDiv(worldPosition.x, CHUNK_SIZE_X), 0, FloorDiv(worldPosition.z, CHUNK_SIZE_Z) };
    }

    static glm::ivec3 LocalOf(glm::ivec3 worldPosition)
    {
        glm::ivec3 chunkPos = ChunkOf(worldPosition);
        return { worldPosition.x - chunkPos.x * CHUNK_SIZE_X, worldPosition.y - WORLD_MIN_Y, worldPosition.z - chunkPos.z * CHUNK_SIZE_Z };
    }

    // Shared by both chunk-local SetBlock overloads: bounds-check, look up
    // the chunk, apply `mutate` (the one line that differs between the
    // uint16_t and full-Block overloads), then compute the same dirty-
    // section set either way — see the public SetBlock's own comment for
    // what that covers.
    template <typename MutateFn>
    std::vector<SectionKey> SetBlockImpl(glm::ivec3 chunkPos, glm::ivec3 localPosition, MutateFn mutate)
    {
        std::vector<SectionKey> dirty;

        if (localPosition.x < 0 || localPosition.x >= CHUNK_SIZE_X
            || localPosition.y < 0 || localPosition.y >= CHUNK_SIZE_Y
            || localPosition.z < 0 || localPosition.z >= CHUNK_SIZE_Z)
        {
            return dirty;
        }

        std::unique_lock lock(mutex);
        auto it = chunks.find(ChunkKey(chunkPos.x, chunkPos.z));
        if (it == chunks.end()) return dirty; // Can't edit a chunk that was never loaded.

        mutate(it->second);

        int sectionY = localPosition.y / SECTION_SIZE_Y;
        dirty.push_back({ chunkPos.x, chunkPos.z, sectionY });

        int localSectionY = localPosition.y % SECTION_SIZE_Y;
        if (localSectionY == 0 && sectionY > 0)
            dirty.push_back({ chunkPos.x, chunkPos.z, sectionY - 1 });
        else if (localSectionY == SECTION_SIZE_Y - 1 && sectionY < SECTIONS_PER_CHUNK - 1)
            dirty.push_back({ chunkPos.x, chunkPos.z, sectionY + 1 });

        if (localPosition.x == 0) dirty.push_back({ chunkPos.x - 1, chunkPos.z, sectionY });
        else if (localPosition.x == CHUNK_SIZE_X - 1) dirty.push_back({ chunkPos.x + 1, chunkPos.z, sectionY });

        if (localPosition.z == 0) dirty.push_back({ chunkPos.x, chunkPos.z - 1, sectionY });
        else if (localPosition.z == CHUNK_SIZE_Z - 1) dirty.push_back({ chunkPos.x, chunkPos.z + 1, sectionY });

        return dirty;
    }
};

} // namespace Volcano

#endif
