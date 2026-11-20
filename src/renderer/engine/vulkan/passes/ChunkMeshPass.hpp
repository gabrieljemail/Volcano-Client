#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CHUNK_MESH_PASS_H
#define VOLCANO_ENGINE_PASSES_CHUNK_MESH_PASS_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "../../RenderPass.hpp"
#include "../../shared/ChunkCull.hpp"
#include "../../../VulkanInit.hpp"

namespace Volcano::Engine::Passes {

// Shared Vulkan base for the two passes that draw per-chunk Mesh lists
// (TerrainPass, NonCubicPass): both use VulkanInit's pipelineLayout
// (model-matrix push constant + camera set + texture array set), so binding
// the sets and issuing one mesh's draw are identical between them. The
// backend-neutral culling/sorting/fade logic lives in ChunkCull - see that
// header for what a Direct3D 11 sibling pass would reuse.
class ChunkMeshPass : public RenderPass, public ChunkCull {
public:
    using RenderPass::RenderPass;

protected:
    // Camera UBO for this frame slot (set 0) + the block texture array (set 1).
    void BindWorldDescriptorSets(const FrameContext& frame) const
    {
        VkDescriptorSet sets[] = { cameraSets[frame.frameIndex], textureSet };
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);
    }

    // `category` is which Application::trianglesDrawn bucket this mesh's
    // triangles count toward (debug HUD only, skipped while it's hidden).
    // Mesh::vertexBuffer/indexBuffer are opaque handles - VkBuffer under
    // this backend.
    void DrawMesh(VkCommandBuffer cmd, const Mesh& mesh, Application* state, Application::TriangleCategory category) const
    {
        const VkBuffer vertexBuffer = static_cast<VkBuffer>(mesh.vertexBuffer);
        const VkBuffer indexBuffer = static_cast<VkBuffer>(mesh.indexBuffer);
        VkDeviceSize offsets[] = {mesh.vertexOffset};
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);
        struct PushConstants
        {
            glm::mat4 model;
            float fade;
        } push{mesh.modelMatrix, FadeAmount(mesh, std::chrono::steady_clock::now())};
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);

        if (mesh.indexCount > 0)
        {
            vkCmdBindIndexBuffer(cmd, indexBuffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
            if (state && state->debugHudVisible) state->trianglesDrawn[category] += mesh.indexCount / 3;
        } else
        {
            vkCmdDraw(cmd, mesh.vertexCount, 1, 0, 0);
            if (state && state->debugHudVisible) state->trianglesDrawn[category] += mesh.vertexCount / 3;
        }
    }
};

} // namespace Volcano::Engine::Passes

#endif
