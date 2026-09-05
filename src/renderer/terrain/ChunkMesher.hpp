#pragma once
#ifndef CHUNKMESHER_H
#define CHUNKMESHER_H

#include <cstdint>
#include "../models/Mesh.hpp"
#include "models/Chunk.hpp"
#include "../TextureManager.hpp"

namespace Volcano {

class ChunkMesher {
public:
    // Size of each backing slab buffer (one for vertices, one for indices)
    // that chunk meshes are bump-allocated from.
    static constexpr uint64_t DEFAULT_SLAB_SIZE = 256ull * 1024 * 1024; // 256 MB

    static Mesh MeshChunk(const Chunk& chunk, const TextureManager& textureManager);

    // Releases the slab buffers. Must be called before the VMA allocator is
    // destroyed (e.g. from VulkanInit::Cleanup()).
    static void Shutdown();
};

} // namespace Volcano

#endif
