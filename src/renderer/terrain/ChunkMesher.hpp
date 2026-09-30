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

    // `world` lets the greedy sweep see across chunk boundaries (see
    // GreedyMeshAxis's own comment) instead of treating every neighboring
    // chunk as solid air — without it, two adjacent chunks with solid
    // terrain at their shared edge each assume the other side is air and
    // both emit a face there, producing exactly-coincident duplicate
    // geometry (visible as constant z-fighting/flicker at chunk seams).
    // `chunk` itself doesn't need to already be inserted into `world` for
    // this call.
    //
    // Meshes one 16-tall vertical slice (sectionY 0..SECTIONS_PER_CHUNK-1 —
    // see ChunkSection.hpp) instead of the whole 384-tall column, so a
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
