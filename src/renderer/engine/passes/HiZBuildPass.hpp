#pragma once
#ifndef VOLCANO_ENGINE_PASSES_HIZ_BUILD_PASS_H
#define VOLCANO_ENGINE_PASSES_HIZ_BUILD_PASS_H

#include <chrono>
#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

namespace Volcano::Engine {
class ShaderLibrary;
}

namespace Volcano::Engine::Passes {

// Builds this frame's Hi-Z occlusion pyramid (VulkanInit's hizImage) from
// the just-finished depth buffer: a same-resolution copy into mip 0, then a
// max-reduction mip chain above it - see hiz_copy.comp/hiz_downsample.comp
// for the per-texel math, and VulkanInit.hpp's own comment on the pyramid
// itself.
//
// One small mip (at most READBACK_MAX_DIM on each side) is then copied into
// a per-frame-slot host buffer. When that slot comes around again, its fence
// has signaled, so BeginFrame() can safely copy it into occlusionSnapshot
// (see OcclusionSnapshot.hpp), which VisibleChunkController tests chunk
// sections against on the CPU.
//
// NOT a RenderPass: compute dispatches can't run inside a render pass
// instance, and this engine's entire pass list is recorded inside one
// single vkCmdBeginRenderPass/vkCmdEndRenderPass pair (see
// VulkanRenderEngine::SubmitFrame). So this is called directly from
// SubmitFrame, after EndMainPass, rather than registered in RenderEngine's
// pass list like every other pass.
class HiZBuildPass {
public:
    static constexpr uint32_t MAX_SLOTS = 3; // >= maxFramesInFlight
    static constexpr uint32_t READBACK_MAX_DIM = 64;

    void Create(const ShaderLibrary& shaders);
    void Destroy();

    // Call after this slot's fence wait and before any pass records:
    // publishes the slot's (now finished) readback into occlusionSnapshot.
    void BeginFrame(uint32_t frameIndex);

    // Copies depthImage into the pyramid, downsamples it, and queues the
    // readback into this frame's slot. Must run outside any render pass
    // instance, after EndMainPass and before vkEndCommandBuffer.
    void Record(VkCommandBuffer cmd, uint32_t frameIndex, const glm::mat4& viewProj);

private:
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        bool pending = false; // a readback was recorded into it
        glm::mat4 viewProj{1.0f};
        uint32_t fullWidth = 0, fullHeight = 0, level = 0, width = 0, height = 0;
    };
    Slot slots[MAX_SLOTS];

    std::chrono::steady_clock::time_point lastLogTime{};
};

} // namespace Volcano::Engine::Passes

#endif
