#pragma once
#ifndef VOLCANO_NON_CUBIC_MESHER_H
#define VOLCANO_NON_CUBIC_MESHER_H

#include <cstdint>
#include "../models/Mesh.hpp"
#include "../terrain/models/Chunk.hpp"
#include "../terrain/models/World.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

// Meshes every block in a chunk whose shape isn't a fully opaque cube —
// (slabs, carpets, snow layers, ...), cross-shaped plants (grass, flowers,
// saplings, ...), and liquids (water) — into two Meshes for the engine's
// two non-cubic render passes: `cutout` (see VulkanInit's nonCubicPipeline:
// no backface culling, alpha blended, depth write ON — everything above
// except liquids) and `translucent` (VulkanInit's translucentPipeline: same,
// but depth write OFF, for genuinely see-through geometry — currently just
// NonCubeShape::Liquid). Counterpart to ChunkMesher, which only handles
// fully opaque full-cube blocks; see BlockRegistry::NonCubeVisual for how a
// block resolves to one shape or another.
//
// Unlike ChunkMesher this isn't greedy-meshed — these shapes are far rarer
// per chunk than solid terrain, and their varied geometry (cross planes,
// partial boxes) doesn't merge into rectangles the same way full block
// faces do, so one quad is emitted per face/plane directly.
class NonCubicMesher {
public:
    // Backing slab size for this mesher's own vertex/index buffers (separate
    // from ChunkMesher's — see MeshChunk). Far less non-cube geometry exists
    // per world than opaque terrain, so a smaller slab is enough. Raised
    // from 64 MB after it ran out mid-session. Shared by both the cutout and
    // translucent slab pairs.
    static constexpr uint64_t DEFAULT_SLAB_SIZE = 128ull * 1024 * 1024; // 128 MB

    // One chunk's output for each of the two non-cubic passes — see this
    // class's own comment for what goes in each.
    struct ChunkMeshes {
        Mesh cutout;
        Mesh translucent;
    };

    // world is needed for MeshTransparentCube/MeshLiquid's same-type neighbor
    // culling to see across chunk boundaries — see GetBlockAcrossChunks in
    // the .cpp (mirrors ChunkMesher's own World-backed neighbor lookup).
    //
    // Meshes one 16-tall section at a time, same reasoning and granularity
    // as ChunkMesher::MeshChunkSection (see its own comment) — kept in sync
    // so a chunk's cube/cutout/translucent meshes always cover the same Y
    // range and share one SectionKey.
    static ChunkMeshes MeshChunkSection(const Chunk& chunk, const World& world, const TextureManager& textureManager, int sectionY);

    // Releases the slab buffers. Must be called before the VMA allocator is
    // destroyed (e.g. from VulkanInit::Cleanup()), same requirement as
    // ChunkMesher::Shutdown().
    static void Shutdown();
};

} // namespace Volcano

#endif
