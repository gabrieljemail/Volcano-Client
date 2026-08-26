#include <thread>
#include <memory>
#include <iostream>
#include <vector>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "VulkanInit.hpp"
#include "RenderThread.hpp"
#include "../Helpers.hpp"

using namespace std;

// Creates the graphics pipeline. Used in the main function.
void CreateGraphicsPipeline() {
    // Load compiled SPIR-V binary.
    vector<char>* vertShaderCode = ReadFile("resources/terrain.vert.spv");

    // TODO: Set up the shader stage.
    // TODO: Set up single-pass sampling.
    // TODO: Finish the graphics pipeline.
}

// Main function.
void Init()
{
    // Make an instance.
    vkb::InstanceBuilder instanceBuilder;
    auto instanceReturn = instanceBuilder
        .set_app_name(APP_NAME)
        .set_app_version(APP_VERSION)
        .set_engine_name(ENGINE_NAME)
        .set_engine_version(ENGINE_VERSION)
        .require_api_version(1,0,0)
        .build();
    
    if (!instanceReturn)
    {
        cerr << "[ERROR] Failed to create Vulkan instance: " << instanceReturn.error().message() << endl;
        Cleanup();
        exit(2);
        return;
    }
    instance = instanceReturn.value();

    // Get a device.
    vkb::PhysicalDeviceSelector physDeviceSelector(instance);
    auto physDeviceSelectorReturn = physDeviceSelector
        .select(); // TODO: Read settings.json if the user has a preferred device name.
    
    if (!physDeviceSelectorReturn)
    {
        cerr << "[ERROR] No suitable Vulkan devices." << endl;
        Cleanup();
        exit(3);
        return;
    }
    physicalDevice = physDeviceSelectorReturn.value();

    // Get the logical device.
    vkb::DeviceBuilder deviceBuilder{physicalDevice};
    auto deviceBuilderReturn = deviceBuilder.build();
    
    if (!deviceBuilderReturn)
    {
        cerr << "[ERROR] Device creation failed: " << deviceBuilderReturn.error().message() << endl;
        Cleanup();
        exit(4);
        return;
    }
    device = deviceBuilderReturn.value();
    cout << "[INFO] Vulkan device initialized." << endl;

    // Initialize GLFW.
    if (!glfwInit())
    {
        cerr << "[ERROR] Failed to initialize GLFW." << endl;
        Cleanup();
        exit(5);
        return;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Prevent GLFW from creating an OpenGL context.
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE); // Prevent resizing the window.
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE); // Hide system window decorations.

    // Create the window.
    window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, APP_NAME, nullptr, nullptr);

    if (!window)
    {
        cerr << "[ERROR] Failed to create window." << endl;
        glfwTerminate();
        Cleanup();
        exit(6);
        return;
    }

    // Create the surface.
    VkResult error = glfwCreateWindowSurface(instance, window, nullptr, &surface);
    if (error != VK_SUCCESS)
    {
        cerr << "[ERROR] Failed to create window surface." << endl;
        glfwTerminate();
        Cleanup();
        exit(7);
        return;
    }

    // Create the swapchan.
    vkb::SwapchainBuilder swapchainBuilder{device, surface};
    swapchainBuilder.set_desired_present_mode(PRESENT_MODE);
    swapchainBuilder.set_desired_min_image_count(BUFFER_SIZE);
    auto swapchainBuilderReturn = swapchainBuilder.build();

    if (!swapchainBuilderReturn)
    {
        cerr << "[ERROR] Failed to create swapchain: " << swapchainBuilderReturn.error().message() << endl;
        glfwTerminate();
        Cleanup();
        exit(8);
        return;
    }
    swapchain = swapchainBuilderReturn.value();
    uint32_t imageCount = swapchain.image_count;

    // Create image views.
    imageViews.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchain.get_images().value()[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = swapchain.image_format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        
        VkResult createViewResult = vkCreateImageView(device, &viewInfo, nullptr, &imageViews[i]);
        if (createViewResult != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create swapchain image view.");
        }
    }

    // Create the render pass.
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchain.image_format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription attachments[] = {colorAttachment};

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = attachments;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    VkResult createRenderPassResult = vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass);
    if (createRenderPassResult != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create render pass.");
    }

    // Create framebuffers.
    framebuffers.resize(imageCount);
    for (uint32_t i = 0; i < imageCount; i++)
    {
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &imageViews[i];
        framebufferInfo.width = swapchain.extent.width;
        framebufferInfo.height = swapchain.extent.height;
        framebufferInfo.layers = 1;

        VkResult createFramebufferResult = vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]);
        if (createFramebufferResult != VK_SUCCESS)
        {
            throw std::runtime_error("[ERROR] Failed to create framebuffer.");
        }
    }

    // Create the graphics pipeline.
    CreateGraphicsPipeline();

    // Create command pool and command buffers.
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; // Allow re-recording

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool!");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 3;

    if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffers)) {
        throw std::runtime_error("Failed to allocate command buffers!");
    }

    // Create semaphores and fences.
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < 3; i++) {
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]);
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]);
        vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]);
    }
}

void Cleanup()
{
    if (device != VK_NULL_HANDLE)
    {
        vkb::destroy_device(device);
    }

    if (instance != VK_NULL_HANDLE)
    {
        vkb::destroy_instance(instance);
    }

    if (surface != VK_NULL_HANDLE)
    {
        vkb::destroy_surface(instance, surface);
    }

    if (swapchain != VK_NULL_HANDLE)
    {
        vkb::destroy_swapchain(swapchain);
    }
}

// Setters: