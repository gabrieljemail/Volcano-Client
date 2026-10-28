#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <glm/glm.hpp>
#include "ChunkDrawBatch.hpp"
#include "../../VulkanInit.hpp"

namespace Volcano::Engine::Passes {

void ChunkDrawBatch::Create()
{
    for (uint32_t i = 0; i < FRAMES; i++)
    {
        instances[i] = CreateBuffer(sizeof(glm::vec4) * CAPACITY, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        commands[i] = CreateBuffer(sizeof(VkDrawIndexedIndirectCommand) * CAPACITY, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        if (instances[i].buffer == VK_NULL_HANDLE || commands[i].buffer == VK_NULL_HANDLE)
        {
            throw std::runtime_error("[ERROR] Failed to allocate indirect chunk draw buffers.");
        }
        vmaMapMemory(vmaAllocator, instances[i].allocation, &instancesMapped[i]);
        vmaMapMemory(vmaAllocator, commands[i].allocation, &commandsMapped[i]);
    }
}

void ChunkDrawBatch::Destroy()
{
    for (uint32_t i = 0; i < FRAMES; i++)
    {
        if (instances[i].buffer != VK_NULL_HANDLE)
        {
            vmaUnmapMemory(vmaAllocator, instances[i].allocation);
            vmaDestroyBuffer(vmaAllocator, instances[i].buffer, instances[i].allocation);
        }
        if (commands[i].buffer != VK_NULL_HANDLE)
        {
            vmaUnmapMemory(vmaAllocator, commands[i].allocation);
            vmaDestroyBuffer(vmaAllocator, commands[i].buffer, commands[i].allocation);
        }
        instances[i] = AllocatedBuffer{};
        commands[i] = AllocatedBuffer{};
        instancesMapped[i] = nullptr;
        commandsMapped[i] = nullptr;
    }
}

uint64_t ChunkDrawBatch::Draw(VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<const Mesh*>& meshes, VkDeviceSize vertexStride)
{
    if (meshes.empty()) return 0;

    const uint32_t slot = frameIndex % FRAMES;
    auto* instanceData = static_cast<glm::vec4*>(instancesMapped[slot]);
    auto* commandData = static_cast<VkDrawIndexedIndirectCommand*>(commandsMapped[slot]);
    const VkBuffer instanceBuffer = instances[slot].buffer;
    const VkBuffer commandBuffer = commands[slot].buffer;
    const auto now = std::chrono::steady_clock::now();
    constexpr float FADE_SECONDS = 0.75f; // Same as ChunkMeshPass::DrawMesh.

    uint32_t count = 0;
    uint32_t runStart = 0;
    VkBuffer runVertexBuffer = VK_NULL_HANDLE, runIndexBuffer = VK_NULL_HANDLE;
    uint64_t triangles = 0;

    // Meshes normally all share one vertex/index buffer pair, so this is one
    // draw; a pair change (e.g. a slab swapped out) starts another.
    auto flush = [&]()
    {
        if (count == runStart) return;
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &runVertexBuffer, &zero);
        vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuffer, &zero);
        vkCmdBindIndexBuffer(cmd, runIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexedIndirect(cmd, commandBuffer, runStart * sizeof(VkDrawIndexedIndirectCommand),
            count - runStart, sizeof(VkDrawIndexedIndirectCommand));
        runStart = count;
    };

    for (const Mesh* mesh : meshes)
    {
        if (count >= CAPACITY) break;
        if (mesh->indexCount == 0 || mesh->vertexOffset % vertexStride != 0) continue;

        if (count > runStart && (mesh->vertexBuffer != runVertexBuffer || mesh->indexBuffer != runIndexBuffer)) flush();
        if (count == runStart)
        {
            runVertexBuffer = mesh->vertexBuffer;
            runIndexBuffer = mesh->indexBuffer;
        }

        float fade = 1.0f;
        if (mesh->fadeStart.time_since_epoch().count() != 0)
        {
            fade = std::clamp(std::chrono::duration<float>(now - mesh->fadeStart).count() / FADE_SECONDS, 0.0f, 1.0f);
        }

        commandData[count] = {
            mesh->indexCount, 1,
            static_cast<uint32_t>(mesh->indexOffset / sizeof(uint32_t)),
            static_cast<int32_t>(mesh->vertexOffset / vertexStride),
            count
        };
        instanceData[count] = glm::vec4(glm::vec3(mesh->modelMatrix[3]), fade);
        triangles += mesh->indexCount / 3;
        count++;
    }
    flush();

    if (count > 0)
    {
        vmaFlushAllocation(vmaAllocator, instances[slot].allocation, 0, sizeof(glm::vec4) * count);
        vmaFlushAllocation(vmaAllocator, commands[slot].allocation, 0, sizeof(VkDrawIndexedIndirectCommand) * count);
    }
    return triangles;
}

} // namespace Volcano::Engine::Passes
