#pragma once
#ifndef VOLCANO_GUI_PINCEL_RASTERIZER_H
#define VOLCANO_GUI_PINCEL_RASTERIZER_H

#include <cstdint>
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <Pincel/Texture.hpp>

namespace Volcano::GUI {

/**
 * Since Pincel v0.4.1, the primary raster method is DrawData/vertex buffers
 * with a fragment shader. This is the custom Vulkan implementation of that.
 */
class PincelRasterizer {
public:
	PincelRasterizer(VkRenderPass renderPass);
	~PincelRasterizer();

	void Upload(const Pincel::Texture& texture);
	void Record(VkCommandBuffer commandBuffer);

private:
	VkRenderPass renderPass;
	VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation imageAllocation = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    static constexpr uint32_t UPLOAD_SLOTS = 3;
    VkBuffer staging[UPLOAD_SLOTS]{};
    VmaAllocation stagingAllocation[UPLOAD_SLOTS]{};
    void* stagingMapped[UPLOAD_SLOTS]{};
    VkCommandBuffer uploadCommands[UPLOAD_SLOTS]{};
    VkFence uploadFences[UPLOAD_SLOTS]{};
    uint32_t nextSlot = 0;
    bool imageHasContent = false;

    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t uploadedVersion = 0;

	void CreateImage(uint32_t width, uint32_t height);
	void DestroyImage();
};

} // namespace Volcano::GUI

#endif
