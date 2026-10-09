#pragma once
#ifndef CHUNKMESHER_H
#define CHUNKMESHER_H

#include <cstdint>
#include "../models/Mesh.hpp"
#include "../models/SlabBuffer.hpp"
#include "models/Chunk.hpp"
#include "models/World.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

class ChunkMesher {
public:
    // Size of each backing slab buffer (one for vertices, one for indices)
    // that chunk meshes are bump-allocated from.
    static constexpr uint64_t DEFAULT_SLAB_SIZE = 256ull * 1024 * 1024; // 256 MB

    // A helper function to compute vertex AO when meshing chunks for smooth lighting.
    static uint8_t ComputeVertexAO(const Chunk& chunk, const World& world,
        int u, int v, int n,                // Axes.
        int cornerU, int cornerV,           // Corner (0-3).
        int blockU, int blockV, int blockN  // Solid block position.
    );

    // Meshes one 16-tall vertical slice instead of the whole 384-tall column, so a
    // single block edit only costs re-sweeping ~16x16x16, not the entire
    // column. Callers should skip calling this for a section
    // Chunk::IsSectionEmpty reports as empty.
    static Mesh MeshChunkSection(const Chunk& chunk, const World& world, const TextureManager& textureManager, int sectionY);

    // Releases the slab buffers. Must be called before the VMA allocator is
    // destroyed (e.g. from VulkanInit::Cleanup()).
    static void Shutdown();

    // Hands a mesh's slab regions back (reused a few frames later — see
    // SlabBuffer::Free). Returns false if the mesh wasn't from these slabs.
    static bool ReleaseMesh(const Mesh& mesh);

    // Combined used/capacity across the vertex and index slabs, for the
    // debug HUD (GUIController). {0, 0} before the first chunk mesh —
    // slabsInitialized lazily, on first use.
    static SlabStats GetSlabStats();
};

} // namespace Volcano

#endif
