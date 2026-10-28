#pragma once
#ifndef VOLCANO_GUI_PINCEL_SURFACE_H
#define VOLCANO_GUI_PINCEL_SURFACE_H

#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <Pincel/Texture.hpp>

namespace Volcano::GUI {

// The GPU side of a Pincel canvas: a screen-sized texture the canvas's pixels
// are copied into, and the draw that lays it over the frame. Render thread only.
class PincelSurface {
public:
    bool Create(VkRenderPass renderPass);
    void Destroy();

    // Copies the pixels to the GPU if they changed since the last call, and
    // resizes to fit. Outside a render pass; waits for the GPU while it copies.
    void Upload(const Pincel::Texture& texture);

    // Draws the texture over the whole frame, inside the main render pass.
    void Record(VkCommandBuffer commandBuffer);

    bool HasContent() const { return uploadedVersion != 0; }

private:
    bool CreateImage(uint32_t width, uint32_t height);
    void DestroyImage();

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation imageAllocation = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;

    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t uploadedVersion = 0;
};

} // namespace Volcano::GUI

#endif
