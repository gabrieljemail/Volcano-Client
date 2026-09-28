#include <mutex>
#include "TerrainPass.hpp"
#include "../../../GlobalState.hpp"
#include "../../terrain/VisibleChunkController.hpp"

namespace Volcano::Engine::Passes {

void TerrainPass::Create(const ShaderLibrary& shaders)
{
    CreateGraphicsPipeline(shaders);
}

void TerrainPass::Destroy()
{
    DestroyGraphicsPipeline();
}

void TerrainPass::Record(FrameContext& frame)
{
    VkCommandBuffer cmd = frame.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, frame.scene->wireframe ? wireframePipeline : graphicsPipeline);
    BindWorldDescriptorSets(frame);

    // Locked against MeshingThread, which appends to renderList as chunks
    // stream in — see the mutex's comment in GlobalState.hpp. Frustum
    // culling runs inside the lock (cheap: a handful of dot products per
    // chunk) so the filtered list doesn't outlive renderList's own Mesh
    // storage.
    std::lock_guard<std::mutex> lock(frame.state->renderListMutex);
    for (const Mesh* mesh : VisibleChunkController::GetVisibleMeshes(frame.state->renderList, frame.viewProj))
    {
        DrawMesh(cmd, *mesh);
    }
}

} // namespace Volcano::Engine::Passes
