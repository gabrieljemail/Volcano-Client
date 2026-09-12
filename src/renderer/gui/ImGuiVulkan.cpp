#include "ImGuiVulkan.hpp"
#include "Logger.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace Volcano {

void ImGuiVulkan::Init(ImGuiVulkanContext& context, VkRenderPass renderPass, uint32_t imageCount) {
    context.renderPass = renderPass;
    
    // Create descriptor pool for ImGui
    CreateDescriptorPool(context);
    
    // Initialize ImGui Vulkan backend
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = instance;
    init_info.PhysicalDevice = physicalDevice;
    init_info.Device = device;
    init_info.QueueFamily = graphicsQueueFamilyIndex;
    init_info.Queue = graphicsQueue;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = context.descriptorPool;
    init_info.MinImageCount = imageCount;
    init_info.ImageCount = imageCount;
    init_info.UseDynamicRendering = false;
    
    // Use the new PipelineInfo structure
    init_info.PipelineInfoMain.RenderPass = renderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    
    if (!ImGui_ImplVulkan_Init(&init_info)) {
        Log::Error("[ERROR] Failed to initialize ImGui Vulkan backend");
        return;
    }

    // The new ImGui backend handles font loading automatically during Init
    // No need to manually upload fonts anymore

    context.initialized = true;
    Log::Info("[INFO] ImGui Vulkan backend initialized");
}

void ImGuiVulkan::Shutdown(ImGuiVulkanContext& context) {
    if (!context.initialized) return;
    
    ImGui_ImplVulkan_Shutdown();
    
    if (context.descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, context.descriptorPool, nullptr);
        context.descriptorPool = VK_NULL_HANDLE;
    }
    
    context.initialized = false;
    Log::Info("[INFO] ImGui Vulkan backend shutdown");
}

void ImGuiVulkan::NewFrame(ImGuiVulkanContext& context) {
    if (!context.initialized) return;
    ImGui_ImplVulkan_NewFrame();
}

void ImGuiVulkan::RenderDrawData(ImGuiVulkanContext& context, VkCommandBuffer commandBuffer) {
    if (!context.initialized) return;
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void ImGuiVulkan::UpdateBuffers(ImGuiVulkanContext& context, VkCommandBuffer commandBuffer) {
    // This is handled automatically by ImGui_ImplVulkan_RenderDrawData
}

void ImGuiVulkan::CreateDescriptorPool(ImGuiVulkanContext& context) {
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };
    
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000;
    pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    
    if (vkCreateDescriptorPool(device, &pool_info, nullptr, &context.descriptorPool) != VK_SUCCESS) {
        Log::Error("[ERROR] Failed to create ImGui descriptor pool");
        return;
    }
}

void ImGuiVulkan::CreateCommandPool(ImGuiVulkanContext& context) {
    // We use the existing command pool from VulkanInit
}

} // namespace Volcano