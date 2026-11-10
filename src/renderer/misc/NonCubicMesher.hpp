#pragma once
#ifndef VOLCANO_NON_CUBIC_MESHER_H
#define VOLCANO_NON_CUBIC_MESHER_H

#include <cstdint>
#include "../models/Mesh.hpp"
#include "../models/SlabBuffer.hpp"
#include "../terrain/models/Chunk.hpp"
#include "../terrain/models/World.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

// Meshes every block in a chunk whose shape isn't a fully opaque cube -
// (slabs, carpets, snow layers, ...), cross-shaped plants (grass, flowers,
// saplings, ...), and liquids (water) - into two Meshes for the engine's
// two non-cubic render passes: `cutout` (see VulkanInit's nonCubicPipeline:
// no backface culling, alpha blended, depth write ON
class NonCubicMesher {
public:
    // Backing slab size for this mesher's own vertex/index buffers (separate
    // from ChunkMesher's
    static constexpr uint64_t DEFAULT_SLAB_SIZE = 32ull * 1024 * 1024; // 32 MB, x4 slabs

    // One chunk's output for each of the two non-cubic passes
    struct ChunkMeshes {
        Mesh cutout;
        Mesh translucent;
    };

    // world is needed for MeshTransparentCube/MeshLiquid's same-type neighbor
    // culling to see across chunk boundaries
    static ChunkMeshes MeshChunkSection(const Chunk& chunk, const World& world, const TextureManager& textureManager, int sectionY);

    // Releases the slab buffers. Must be called before the VMA allocator is
    // destroyed (e.g. from VulkanInit::Cleanup()), same requirement as
    // ChunkMesher::Shutdown().
    static void Shutdown();

    // Same as ChunkMesher::ReleaseMesh, for any of this mesher's four slabs.
    static bool ReleaseMesh(const Mesh& mesh);

    // Combined used/capacity across all four slabs (cutout + translucent,
    // vertex + index), for the debug HUD (GUIController). {0, 0} before the
    // first chunk mesh
    static SlabStats GetSlabStats();
};

} // namespace Volcano

#endif
