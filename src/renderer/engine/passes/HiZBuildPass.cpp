#include "HiZBuildPass.hpp"
#include "../../VulkanInit.hpp"
#include "../../../Logger.hpp"
#include "../../terrain/OcclusionSnapshot.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace Volcano::Engine::Passes {

namespace {
    constexpr uint32_t LOCAL_SIZE = 8; // Matches local_size_x/y in both .comp shaders.
    uint32_t DispatchGroups(uint32_t extent) { return (extent + LOCAL_SIZE - 1) / LOCAL_SIZE; }
}

void HiZBuildPass::Create(const ShaderLibrary& shaders)
{
    CreateHiZPipelines(shaders);

    // Created directly through VMA (not CreateBuffer) since these need
    // host-READ-friendly memory (RANDOM access, i.e. cached), and so they
    // stay out of VulkanInit's allocatedBuffers sweep — freed in Destroy().
    for (Slot& slot : slots)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(float) * READBACK_MAX_DIM * READBACK_MAX_DIM;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &slot.buffer, &slot.allocation, &info) != VK_SUCCESS)
        {
            throw std::runtime_error("[ERROR] Failed to create Hi-Z readback buffer!");
        }
        slot.mapped = info.pMappedData;
        slot.pending = false;
    }
}

void HiZBuildPass::Destroy()
{
    DestroyHiZPipelines();

    for (Slot& slot : slots)
    {
        if (slot.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, slot.buffer, slot.allocation);
        slot = Slot{};
    }
    occlusionSnapshot.valid = false;
}

void HiZBuildPass::BeginFrame(uint32_t frameIndex)
{
    // Periodic stats from the frames since the last log line.
    auto now = std::chrono::steady_clock::now();
    if (now - lastLogTime >= std::chrono::seconds(2))
    {
        lastLogTime = now;
        Log::Info("[HIZ] occlusion: " + std::to_string(occlusionSnapshot.occluded) + " of "
            + std::to_string(occlusionSnapshot.tested) + " in-frustum section draws culled last frame (terrain + non-cubic)"
            + (occlusionSnapshot.valid ? "" : " [no snapshot yet]"));
    }
    occlusionSnapshot.tested = 0;
    occlusionSnapshot.occluded = 0;

    Slot& slot = slots[frameIndex % MAX_SLOTS];
    if (!slot.pending || slot.mapped == nullptr) return;
    slot.pending = false;

    // This slot's fence was just waited on by BeginFrame, so the GPU copy
    // into it is finished — invalidate in case the memory isn't coherent.
    vmaInvalidateAllocation(vmaAllocator, slot.allocation, 0, VK_WHOLE_SIZE);

    occlusionSnapshot.viewProj = slot.viewProj;
    occlusionSnapshot.fullWidth = slot.fullWidth;
    occlusionSnapshot.fullHeight = slot.fullHeight;
    occlusionSnapshot.level = slot.level;
    occlusionSnapshot.width = slot.width;
    occlusionSnapshot.height = slot.height;
    occlusionSnapshot.depth.resize(static_cast<size_t>(slot.width) * slot.height);
    std::memcpy(occlusionSnapshot.depth.data(), slot.mapped, occlusionSnapshot.depth.size() * sizeof(float));
    occlusionSnapshot.valid = true;
}

void HiZBuildPass::Record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::mat4& viewProj)
{
    if (hizCopyPipeline == VK_NULL_HANDLE || hizDownsamplePipeline == VK_NULL_HANDLE) return;

    // depthImage comes out of the main render pass in
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL (its VkAttachmentDescription's
    // finalLayout) — hiz_copy.comp's descriptor set declared it as
    // SHADER_READ_ONLY_OPTIMAL (see CreateHiZResources), so it has to
    // actually be in that layout before the dispatch below reads it. Not
    // transitioned back afterward: the render pass's depth attachment has
    // initialLayout = UNDEFINED, so next frame's BeginRenderPass discards
    // whatever layout/contents are here and transitions it itself.
    VkImageMemoryBarrier depthBarrier{};
    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthBarrier.image = depthImage;
    depthBarrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    // Whether a depth write actually lands at EARLY_FRAGMENT_TESTS or
    // LATE_FRAGMENT_TESTS is implementation-defined (depends on whether the
    // GPU can early-z it) — this render pass has no exit dependency of its
    // own (only one for VK_SUBPASS_EXTERNAL -> 0, entering), so this
    // barrier is the ONLY thing synchronizing the compute read below
    // against the depth write. Missing either stage here means an
    // unsynchronized read on whichever GPUs use that one — this was
    // LATE-only, which is why the pyramid read back as a constant 0
    // (racing ahead of the real writes) instead of real depth values.
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);

    // hizImage's mips are kept in VK_IMAGE_LAYOUT_GENERAL permanently after
    // their one-shot creation-time transition (see CreateHiZResources) —
    // every step below is just an execution/memory dependency (a plain
    // VkMemoryBarrier), never a layout change.
    VkMemoryBarrier computeBarrier{};
    computeBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    computeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    computeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizCopyPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizCopyPipelineLayout, 0, 1, &hizCopySet, 0, nullptr);
    vkCmdDispatch(cmd, DispatchGroups(hizExtent.width), DispatchGroups(hizExtent.height), 1);

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &computeBarrier, 0, nullptr, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizDownsamplePipeline);
    VkExtent2D mipExtent = hizExtent;
    for (uint32_t level = 1; level < hizMipLevels; level++)
    {
        mipExtent.width = std::max(1u, mipExtent.width / 2);
        mipExtent.height = std::max(1u, mipExtent.height / 2);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, hizDownsamplePipelineLayout, 0, 1,
            &hizDownsampleSets[level - 1], 0, nullptr);
        vkCmdDispatch(cmd, DispatchGroups(mipExtent.width), DispatchGroups(mipExtent.height), 1);

        // Every level but the last feeds the next one's read.
        if (level + 1 < hizMipLevels)
        {
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 1, &computeBarrier, 0, nullptr, 0, nullptr);
        }
    }

    // Readback: the first mip no larger than READBACK_MAX_DIM on either
    // side, into this frame's slot. GENERAL is a valid (if not maximally
    // efficient) copy source layout, so only an access-mask barrier here.
    uint32_t level = 0;
    VkExtent2D levelExtent = hizExtent;
    while ((levelExtent.width > READBACK_MAX_DIM || levelExtent.height > READBACK_MAX_DIM) && level + 1 < hizMipLevels)
    {
        level++;
        levelExtent.width = std::max(1u, levelExtent.width / 2);
        levelExtent.height = std::max(1u, levelExtent.height / 2);
    }
    Slot& slot = slots[frameIndex % MAX_SLOTS];
    if (slot.buffer == VK_NULL_HANDLE || levelExtent.width > READBACK_MAX_DIM || levelExtent.height > READBACK_MAX_DIM) return;

    VkMemoryBarrier toTransferBarrier{};
    toTransferBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toTransferBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 1, &toTransferBarrier, 0, nullptr, 0, nullptr);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
    region.imageExtent = { levelExtent.width, levelExtent.height, 1 };
    vkCmdCopyImageToBuffer(cmd, hizImage, VK_IMAGE_LAYOUT_GENERAL, slot.buffer, 1, &region);

    // Make the transfer write visible to host reads once the fence signals.
    VkMemoryBarrier toHostBarrier{};
    toHostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &toHostBarrier, 0, nullptr, 0, nullptr);

    slot.pending = true;
    slot.viewProj = viewProj;
    slot.fullWidth = hizExtent.width;
    slot.fullHeight = hizExtent.height;
    slot.level = level;
    slot.width = levelExtent.width;
    slot.height = levelExtent.height;
}

} // namespace Volcano::Engine::Passes
