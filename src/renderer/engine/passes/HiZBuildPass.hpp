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
// max-reduction mip chain above it
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
