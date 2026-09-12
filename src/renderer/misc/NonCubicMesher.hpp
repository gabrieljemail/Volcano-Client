#pragma once
#ifndef VOLCANO_NON_CUBIC_MESHER_H
#define VOLCANO_NON_CUBIC_MESHER_H

#include <cstdint>
#include "../models/Mesh.hpp"
#include "../terrain/models/Chunk.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

// Meshes every block in a chunk whose shape isn't a fully opaque cube —
// transparent full cubes (glass, slime, ice, ...), partial-volume blocks
// (slabs, carpets, snow layers, ...), and cross-shaped plants (grass,
// flowers, saplings, ...) — into one Mesh for RenderThread's non-cubic
// render pass (see VulkanInit's nonCubicPipeline: no backface culling,
// alpha blended, no depth write). Counterpart to ChunkMesher, which only
// handles fully opaque full-cube blocks; see BlockRegistry::NonCubeVisual
// for how a block resolves to one shape or the other.
//
// Unlike ChunkMesher this isn't greedy-meshed — these shapes are far rarer
// per chunk than solid terrain, and their varied geometry (cross planes,
// partial boxes) doesn't merge into rectangles the same way full block
// faces do, so one quad is emitted per face/plane directly.
class NonCubicMesher {
public:
    // Backing slab size for this mesher's own vertex/index buffers (separate
    // from ChunkMesher's — see MeshChunk). Far less non-cube geometry exists
    // per world than opaque terrain, so a smaller slab is enough.
    static constexpr uint64_t DEFAULT_SLAB_SIZE = 64ull * 1024 * 1024; // 64 MB

    static Mesh MeshChunk(const Chunk& chunk, const TextureManager& textureManager);

    // Releases the slab buffers. Must be called before the VMA allocator is
    // destroyed (e.g. from VulkanInit::Cleanup()), same requirement as
    // ChunkMesher::Shutdown().
    static void Shutdown();
};

} // namespace Volcano

#endif
