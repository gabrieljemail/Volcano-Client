#pragma once
#include <vulkan/vulkan.hpp>
#include "VulkanInit.hpp"

namespace Volcano {

struct ImGuiVulkanContext {
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    
    bool initialized = false;
};

class ImGuiVulkan {
public:
    static void Init(ImGuiVulkanContext& context, VkRenderPass renderPass, uint32_t imageCount);
    static void Shutdown(ImGuiVulkanContext& context);
    static void NewFrame(ImGuiVulkanContext& context);
    static void RenderDrawData(ImGuiVulkanContext& context, VkCommandBuffer commandBuffer);
    static void UpdateBuffers(ImGuiVulkanContext& context, VkCommandBuffer commandBuffer);
    
private:
    static void CreateDescriptorPool(ImGuiVulkanContext& context);
    static void CreateCommandPool(ImGuiVulkanContext& context);
};

} // namespace Volcano