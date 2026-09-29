#pragma once
#ifndef VOLCANO_CHUNK_SECTION_H
#define VOLCANO_CHUNK_SECTION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include "Chunk.hpp"

namespace Volcano {

// A chunk's mesh is now built and stored in 16-tall vertical slices
// ("sections", matching vanilla's own render-chunk granularity) instead of
// one mesh per full 16x384x16 column — see ChunkMesher::MeshChunkSection's
// own comment for why. Block STORAGE (Chunk.hpp/World.hpp) stays
// full-column; only the mesh is sliced.
constexpr int SECTION_SIZE_Y = 16;
constexpr int SECTIONS_PER_CHUNK = CHUNK_SIZE_Y / SECTION_SIZE_Y; // 24

// Identifies one chunk section — the key every section-granularity render
// list (GlobalState::renderList/nonCubicRenderList/translucentRenderList)
// and the dirty-remesh queue (NetworkInbox::dirtySections) use.
struct SectionKey {
    int32_t chunkX;
    int32_t chunkZ;
    int32_t sectionY; // 0..SECTIONS_PER_CHUNK-1

    bool operator==(const SectionKey& other) const
    {
        return chunkX == other.chunkX && chunkZ == other.chunkZ && sectionY == other.sectionY;
    }
};

// One parsed clientbound Update Light packet. Arrays are indexed by section
// (0..SECTIONS_PER_CHUNK-1); each holds 2048 packed nibbles, or is empty if
// that section wasn't in the packet.
struct PendingLightUpdate {
    int32_t chunkX = 0;
    int32_t chunkZ = 0;
    std::array<std::vector<uint8_t>, SECTIONS_PER_CHUNK> skyLight;
    std::array<std::vector<uint8_t>, SECTIONS_PER_CHUNK> blockLight;
};

struct SectionKeyHash {
    size_t operator()(const SectionKey& key) const
    {
        // Plain combine — chunk coordinates and section indices are all
        // small enough (well under 2^20) that a naive shift-and-xor spreads
        // fine; this isn't a hot path worth a fancier mixer.
        size_t h = std::hash<int32_t>()(key.chunkX);
        h = h * 31 + std::hash<int32_t>()(key.chunkZ);
        h = h * 31 + std::hash<int32_t>()(key.sectionY);
        return h;
    }
};

} // namespace Volcano

#endif
