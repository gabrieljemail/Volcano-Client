#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CHUNK_DRAW_BATCH_H
#define VOLCANO_ENGINE_PASSES_CHUNK_DRAW_BATCH_H

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>
#include "../../models/AllocatedBuffer.hpp"
#include "../../models/Mesh.hpp"

namespace Volcano::Engine::Passes {

// Draws a list of chunk-section Meshes with one vkCmdDrawIndexedIndirect
// instead of a bind/push-constant/draw per section (Graphics.IndirectDraw).
//
// Every mesh of a list lives in the same two slab buffers (see SlabBuffer), so
// a section is just a VkDrawIndexedIndirectCommand pointing into them
// (firstIndex/vertexOffset from the mesh's slab offsets). What used to be the
// per-draw push constant - the section's world origin and its fade - goes in a
// per-instance vertex attribute instead: one vec4 (xyz origin, w fade) per
// section, selected by firstInstance (needs drawIndirectFirstInstance). See
// terrain_indirect.vert / misc_indirect.vert.
//
// Both buffers are host-visible and rewritten every frame, one set per frame
// in flight so the GPU never reads what the CPU is writing. Render-thread only.
class ChunkDrawBatch {
public:
    static constexpr uint32_t FRAMES = 3;        // >= maxFramesInFlight
    static constexpr uint32_t CAPACITY = 32768;  // sections per list per frame; any beyond are skipped

    void Create();
    void Destroy();

    // Writes the commands/instance data for `meshes` and records the draws.
    // `vertexStride` is the mesh vertex size in bytes (slab vertex offsets are
    // always multiples of it). Returns the triangles submitted.
    uint64_t Draw(VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<const Mesh*>& meshes, VkDeviceSize vertexStride);

private:
    AllocatedBuffer instances[FRAMES]{};
    AllocatedBuffer commands[FRAMES]{};
    void* instancesMapped[FRAMES]{};
    void* commandsMapped[FRAMES]{};
};

} // namespace Volcano::Engine::Passes

#endif
