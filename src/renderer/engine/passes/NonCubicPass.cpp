#include <algorithm>
#include <mutex>
#include <vector>
#include "NonCubicPass.hpp"
#include "../../../GlobalState.hpp"
#include "../../terrain/VisibleChunkController.hpp"

namespace Volcano::Engine::Passes {

void NonCubicPass::Create(const ShaderLibrary& shaders)
{
    CreateNonCubicPipeline(shaders);
}

void NonCubicPass::Destroy()
{
    DestroyNonCubicPipeline();
}

void NonCubicPass::Record(FrameContext& frame)
{
    VkCommandBuffer cmd = frame.commandBuffer;
    const glm::vec3 cameraPosition = frame.scene->cameraPosition;

    std::lock_guard<std::mutex> lock(frame.state->nonCubicRenderListMutex);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, nonCubicPipeline);
    BindWorldDescriptorSets(frame);

    // Sorted back-to-front by chunk distance from the camera, so two
    // overlapping translucent chunks (e.g. looking through one pane of
    // glass at another) blend in the right order — without this, the chunk
    // that happened to be emitted later always "won" regardless of which
    // was actually nearer, and which chunk that was could silently change
    // from frame to frame (e.g. crossing a chunk boundary), reading as
    // random flicker rather than a consistent-but-wrong order. Depth writes
    // make this ordering irrelevant for the opaque/cutout majority of the
    // pass, but it still matters for whatever genuinely blends, and it's
    // per-CHUNK only: two overlapping translucent surfaces WITHIN one chunk
    // mesh still draw in mesher-emission order.
    std::vector<const Mesh*> meshes = VisibleChunkController::GetVisibleMeshes(frame.state->nonCubicRenderList, frame.viewProj);
    std::sort(meshes.begin(), meshes.end(), [&cameraPosition](const Mesh* a, const Mesh* b)
    {
        glm::vec3 posA(a->modelMatrix[3]);
        glm::vec3 posB(b->modelMatrix[3]);
        float distA = glm::dot(posA - cameraPosition, posA - cameraPosition);
        float distB = glm::dot(posB - cameraPosition, posB - cameraPosition);
        return distA > distB; // Farthest first.
    });

    for (const Mesh* mesh : meshes)
    {
        if (mesh->vertexCount == 0) continue;
        DrawMesh(cmd, *mesh);
    }
}

} // namespace Volcano::Engine::Passes
