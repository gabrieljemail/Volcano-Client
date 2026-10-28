#include <mutex>
#include <vector>
#include "NonCubicPass.hpp"
#include "../vulkan/VulkanRenderEngine.hpp"
#include "../../../GlobalState.hpp"
#include "../../misc/MiscVertex.hpp"

namespace Volcano::Engine::Passes {

void NonCubicPass::Create(const ShaderLibrary& shaders)
{
    CreateNonCubicPipeline(shaders);
    CreateTranslucentPipeline(shaders);
    if (nonCubicIndirectPipeline != VK_NULL_HANDLE) cutoutBatch.Create();
    if (translucentIndirectPipeline != VK_NULL_HANDLE) translucentBatch.Create();
}

void NonCubicPass::Destroy()
{
    cutoutBatch.Destroy();
    translucentBatch.Destroy();
    cutoutCache = VisibleCache{};
    translucentCache = VisibleCache{};
    DestroyNonCubicPipeline();
    DestroyTranslucentPipeline();
}

void NonCubicPass::Record(FrameContext& frame)
{
    VkCommandBuffer cmd = frame.commandBuffer;
    const bool wantIndirect = frame.scene->indirectDraw;

    // With a shadow map this frame, each list draws with its shadow-receiving
    // twin (when it built) and the map bound as set 2.
    Vulkan::VulkanRenderEngine* vulkan = frame.engine->GetVulkan();
    const bool shadowsOn = vulkan != nullptr && vulkan->AreShadowsActive() && shadowedPipelineLayout != VK_NULL_HANDLE;
    auto bindPipeline = [&](VkPipeline plain, VkPipeline shadowedVariant)
    {
        bool useShadow = shadowsOn && shadowedVariant != VK_NULL_HANDLE;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, useShadow ? shadowedVariant : plain);
        if (useShadow)
        {
            VkDescriptorSet sets[] = { cameraSets[frame.frameIndex], textureSet, vulkan->GetShadowSet() };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowedPipelineLayout, 0, 3, sets, 0, nullptr);
        }
        else
        {
            BindWorldDescriptorSets(frame);
        }
    };

    // Both lists are sorted back-to-front by chunk distance from the camera
    // (GetVisible with sortFarthest), so two overlapping translucent chunks
    // (e.g. looking through one pane of glass at another) blend in the right
    // order - without this, the chunk that happened to be emitted later always
    // "won" regardless of which was actually nearer, and which chunk that was
    // could silently change from frame to frame (e.g. crossing a chunk
    // boundary), reading as random flicker rather than a consistent-but-wrong
    // order. Depth writes make this ordering irrelevant for the opaque/cutout
    // majority of the pass, but it still matters for whatever genuinely
    // blends, and it's per-CHUNK only: two overlapping translucent surfaces
    // WITHIN one chunk mesh still draw in mesher-emission order. The indirect
    // path keeps this order too - commands are written in list order.
    {
        std::lock_guard<std::mutex> lock(frame.state->nonCubicRenderListMutex);
        const bool indirect = wantIndirect && nonCubicIndirectPipeline != VK_NULL_HANDLE;
        if (indirect) bindPipeline(nonCubicIndirectPipeline, nonCubicShadowedIndirectPipeline);
        else bindPipeline(nonCubicPipeline, nonCubicShadowedPipeline);

        const std::vector<const Mesh*>& meshes = GetVisible(cutoutCache, frame.state->nonCubicRenderList, frame, true);

        PhaseTimer drawTimer(frame.state, GlobalState::CHUNK_PHASE_DRAW);
        if (indirect)
        {
            uint64_t triangles = cutoutBatch.Draw(cmd, frame.frameIndex, meshes, sizeof(MiscVertex));
            if (frame.state->debugHudVisible) frame.state->trianglesDrawn[GlobalState::TRIANGLES_CUTOUT] += triangles;
        }
        else
        {
            for (const Mesh* mesh : meshes)
            {
                if (mesh->vertexCount == 0) continue;
                DrawMesh(cmd, *mesh, frame.state, GlobalState::TRIANGLES_CUTOUT);
            }
        }
    } // nonCubicRenderListMutex released here - the translucent pass below
      // only needs translucentRenderListMutex, and holding both at once
      // needlessly blocks MeshingThread's upsert() into nonCubicRenderList
      // for the whole translucent draw too.

    // Translucent pass: currently water only (NonCubicMesher's Liquid shape)
    // - see VulkanInit's translucentPipeline. Drawn right after the cutout
    // pass above, with depth WRITE off, so water actually blends with
    // whatever's behind it (opaque terrain, entities, glass/slabs/etc. from
    // the cutout pass just drawn, and other translucent water chunks)
    // instead of the depth test picking one winner the way the cutout pass's
    // writes-on approach needs. Same back-to-front per-chunk sort and its
    // same per-chunk-only caveat as the cutout pass above.
    std::lock_guard<std::mutex> translucentLock(frame.state->translucentRenderListMutex);
    const bool indirect = wantIndirect && translucentIndirectPipeline != VK_NULL_HANDLE;
    if (indirect) bindPipeline(translucentIndirectPipeline, translucentShadowedIndirectPipeline);
    else bindPipeline(translucentPipeline, translucentShadowedPipeline);

    const std::vector<const Mesh*>& translucentMeshes =
        GetVisible(translucentCache, frame.state->translucentRenderList, frame, true);

    PhaseTimer drawTimer(frame.state, GlobalState::CHUNK_PHASE_DRAW);
    if (indirect)
    {
        uint64_t triangles = translucentBatch.Draw(cmd, frame.frameIndex, translucentMeshes, sizeof(MiscVertex));
        if (frame.state->debugHudVisible) frame.state->trianglesDrawn[GlobalState::TRIANGLES_TRANSLUCENT] += triangles;
        return;
    }

    for (const Mesh* mesh : translucentMeshes)
    {
        if (mesh->vertexCount == 0) continue;
        DrawMesh(cmd, *mesh, frame.state, GlobalState::TRIANGLES_TRANSLUCENT);
    }
}

} // namespace Volcano::Engine::Passes
