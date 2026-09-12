#pragma once
#ifndef CHUNKMESHER_H
#define CHUNKMESHER_H

#include <cstdint>
#include "../models/Mesh.hpp"
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
    static Mesh MeshChunk(const Chunk& chunk, const World& world, const TextureManager& textureManager);

    // Releases the slab buffers. Must be called before the VMA allocator is
    // destroyed (e.g. from VulkanInit::Cleanup()).
    static void Shutdown();
};

} // namespace Volcano

#endif
