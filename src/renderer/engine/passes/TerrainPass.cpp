#include <mutex>
#include "TerrainPass.hpp"
#include "../vulkan/VulkanRenderEngine.hpp"
#include "../../../GlobalState.hpp"
#include "../../models/PackedVertex.hpp"
#include "../../terrain/VisibleChunkController.hpp"

namespace Volcano::Engine::Passes {

void TerrainPass::Create(const ShaderLibrary& shaders)
{
    CreateGraphicsPipeline(shaders);
    if (graphicsIndirectPipeline != VK_NULL_HANDLE) batch.Create();
}

void TerrainPass::Destroy()
{
    batch.Destroy();
    cache = VisibleCache{};
    DestroyGraphicsPipeline();
}

void TerrainPass::Record(FrameContext& frame)
{
    VkCommandBuffer cmd = frame.commandBuffer;
    const bool wireframe = frame.scene->wireframe;

    // With a shadow map this frame, the variant of the terrain shaders that
    // receives it replaces the plain ones (which stay free of the extra work).
    Vulkan::VulkanRenderEngine* vulkan = frame.engine->GetVulkan();
    const bool shadowed = !wireframe && vulkan != nullptr && vulkan->AreShadowsActive() && vulkan->CanReceiveShadows();

    VkPipeline indirectPipeline = shadowed ? shadowedIndirectPipeline
        : (wireframe ? wireframeIndirectPipeline : graphicsIndirectPipeline);
    VkPipeline directPipeline = shadowed ? shadowedPipeline : (wireframe ? wireframePipeline : graphicsPipeline);
    const bool indirect = frame.scene->indirectDraw && indirectPipeline != VK_NULL_HANDLE;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, indirect ? indirectPipeline : directPipeline);
    if (shadowed)
    {
        VkDescriptorSet sets[] = { cameraSets[frame.frameIndex], textureSet, vulkan->GetShadowSet() };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowedPipelineLayout, 0, 3, sets, 0, nullptr);
    }
    else
    {
        BindWorldDescriptorSets(frame);
    }

    // Locked against MeshingThread, which appends to renderList as chunks
    // stream in
    std::lock_guard<std::mutex> lock(frame.state->renderListMutex);
    const std::vector<const Mesh*>& visible = GetVisible(cache, frame.state->renderList, frame, false);

    PhaseTimer drawTimer(frame.state, GlobalState::CHUNK_PHASE_DRAW);
    if (indirect)
    {
        uint64_t triangles = batch.Draw(cmd, frame.frameIndex, visible, sizeof(PackedVertex));
        if (frame.state->debugHudVisible) frame.state->trianglesDrawn[GlobalState::TRIANGLES_TERRAIN] += triangles;
        return;
    }

    for (const Mesh* mesh : visible)
    {
        DrawMesh(cmd, *mesh, frame.state, GlobalState::TRIANGLES_TERRAIN);
    }
}

} // namespace Volcano::Engine::Passes
