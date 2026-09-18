#pragma once
#ifndef VOLCANO_SELECTION_RENDERER_H
#define VOLCANO_SELECTION_RENDERER_H

#include <vulkan/vulkan.hpp>
#include "../../GlobalState.hpp"

namespace Volcano {

// Draws the block selection outline/fill — a translucent box plus a
// wireframe box around whatever InteractionManager::GetTargetedBlock()
// says the camera is looking at, using its own collision shape (so a slab
// or stair only highlights where its actual shape is, not its full voxel —
// see BlockRegistry::GetCollisionBoxes). No per-frame-in-flight buffers
// needed (unlike EntityRenderer): the geometry is one static unit cube, and
// every per-draw value (the box, the color) goes through a push constant.
class SelectionRenderer {
public:
    // Creates the outline/fill pipelines and the shared unit-cube mesh.
    // Must run after VulkanInit's render pass and camera descriptor set
    // layout exist (i.e. after CreateGraphicsPipeline() in VulkanInit::Init()).
    static void Init();

    // Destroys everything Init() created. Must run before the VMA allocator
    // is destroyed (i.e. from VulkanInit::Cleanup()).
    static void Shutdown();

    // No-op if nothing is targeted. Must be called inside an active render
    // pass with viewport/scissor already set, same contract as
    // EntityRenderer::RecordDraw — drawn after the opaque/entity/non-cubic
    // passes so it blends against whatever's actually there, including
    // translucent surfaces like glass.
    static void RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state);
};

} // namespace Volcano

#endif
