#pragma once
#ifndef VOLCANO_ENGINE_PASSES_CHUNK_MESH_PASS_H
#define VOLCANO_ENGINE_PASSES_CHUNK_MESH_PASS_H

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include "../RenderPass.hpp"
#include "../../VulkanInit.hpp"
#include "../../models/Mesh.hpp"

namespace Volcano::Engine::Passes {

// Shared base for the two passes that draw per-chunk Mesh lists (TerrainPass,
// NonCubicPass): both use VulkanInit's pipelineLayout (model-matrix push
// constant + camera set + texture array set), so binding the sets and
// issuing one mesh's draw are identical between them.
class ChunkMeshPass : public RenderPass {
public:
    using RenderPass::RenderPass;

protected:
    // Camera UBO for this frame slot (set 0) + the block texture array (set 1).
    void BindWorldDescriptorSets(const FrameContext& frame) const
    {
        VkDescriptorSet sets[] = { cameraSets[frame.frameIndex], textureSet };
        vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 2, sets, 0, nullptr);
    }

    void DrawMesh(VkCommandBuffer cmd, const Mesh& mesh) const
    {
        VkDeviceSize offsets[] = {mesh.vertexOffset};
        vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertexBuffer, offsets);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &mesh.modelMatrix);

        if (mesh.indexCount > 0)
        {
            vkCmdBindIndexBuffer(cmd, mesh.indexBuffer, mesh.indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
        } else
        {
            vkCmdDraw(cmd, mesh.vertexCount, 1, 0, 0);
        }
    }
};

} // namespace Volcano::Engine::Passes

#endif
