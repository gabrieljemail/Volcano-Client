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

// Helper to create a ShaderModule from raw SPIR-V bytecode
VkShaderModule CreateShaderModule(VkDevice dev, const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(dev, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module!");
    }
    return shaderModule;
}

void CreateGraphicsPipeline()
{
    VkDevice dev = GetDevice();

    // 1. LOAD COMPILED SPIR-V BINARIES
    std::vector<char> vertShaderCode = ReadFile("resources/shaders/terrain.vert.spv");
    std::vector<char> fragShaderCode = ReadFile("resources/shaders/terrain.frag.spv");

    VkShaderModule vertShaderModule = CreateShaderModule(dev, vertShaderCode);
    VkShaderModule fragShaderModule = CreateShaderModule(dev, fragShaderCode);

    // 2. SET UP SHADER STAGES
    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main"; // Entry point name in GLSL

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    // 3. VERTEX INPUT STATE
    // For the Hello Triangle, positions are hardcoded inside the vertex shader,
    // so binding & attribute descriptions are 0 for now.
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 0;
    vertexInputInfo.pVertexBindingDescriptions = nullptr;
    vertexInputInfo.vertexAttributeDescriptionCount = 0;
    vertexInputInfo.pVertexAttributeDescriptions = nullptr;

    // 4. INPUT ASSEMBLY
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    // 5. VIEWPORT & SCISSOR (DYNAMIC STATE SETUP)
    // Making viewport and scissor dynamic means you don't need to rebuild the entire
    // pipeline when resizing the window!
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // 6. RASTERIZATION STATE
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // TODO: Change this ASAP once you get geometry to render.
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    // 7. SINGLE-PASS SAMPLING (MULTISAMPLING)
    // 1 sample per pixel = standard single-pass rendering (no MSAA overhead on HD 530)
    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // 8. COLOR BLENDING
    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | 
                                          VK_COLOR_COMPONENT_G_BIT | 
                                          VK_COLOR_COMPONENT_B_BIT | 
                                          VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE; // Direct write for opaque geometry.

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // 9. PIPELINE LAYOUT (Push Constants & Descriptors)
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 0;
    pipelineLayoutInfo.pSetLayouts = nullptr;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    pipelineLayoutInfo.pPushConstantRanges = nullptr;

    if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("[ERROR] Failed to create pipeline layout.");
    }

    cout << "[INFO] Compiling shaders..." << endl;

    // 10. CREATE GRAPHICS PIPELINE
    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = nullptr; // None needed for 2D triangle test
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicStateInfo;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline!");
    }

    // 11. CLEANUP TEMPORARY SHADER MODULES
    vkDestroyShaderModule(dev, fragShaderModule, nullptr);
    vkDestroyShaderModule(dev, vertShaderModule, nullptr);

    std::cout << "[INFO] Graphics pipeline successfully initialized." << std::endl;
}

// Main function.
void Init()
{
    // On Linux, try to use Wayland instead of X11.
    #ifdef __linux__

        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);

    #endif

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
    glfwWindowHint(GLFW_DECORATED, GLFW_TRUE); // Show system window decorations.

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

    // Make an instance.
    vkb::InstanceBuilder instanceBuilder;
    auto instanceReturn = instanceBuilder
        .set_app_name(APP_NAME)
        .set_app_version(APP_VERSION)
        .set_engine_name(ENGINE_NAME)
        .set_engine_version(ENGINE_VERSION)
        .require_api_version(1,1,0)
        .request_validation_layers()
        .build();
    
    if (!instanceReturn)
    {
        cerr << "[ERROR] Failed to create Vulkan instance: " << instanceReturn.error().message() << endl;
        Cleanup();
        exit(2);
        return;
    }
    instance = instanceReturn.value();

    // Create the surface.
    VkResult error = glfwCreateWindowSurface(instance.instance, window, nullptr, &surface);
    if (error != VK_SUCCESS)
    {
        cerr << "[ERROR] Failed to create window surface." << endl;
        glfwTerminate();
        Cleanup();
        exit(7);
        return;
    }

    // Get a device.
    vkb::PhysicalDeviceSelector physDeviceSelector(instance);
    auto physDeviceSelectorReturn = physDeviceSelector
        .set_surface(surface)
        .select(); // TODO: Read settings.json if the user has a preferred device name.
    
    if (!physDeviceSelectorReturn)
    {
        cerr << "[ERROR] No suitable Vulkan devices: " << physDeviceSelectorReturn.error().message() << endl;
        Cleanup();
        exit(3);
        return;
    }
    physicalDevice = physDeviceSelectorReturn.value();
    cout << "[INFO] Found Vulkan GPU: " << physicalDevice.name << endl;
    cout << "[INFO] Vulkan API version: " << physicalDevice.properties.apiVersion << endl;

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

    // Create the swapchain.
    vkb::SwapchainBuilder swapchainBuilder{device, surface};
    swapchainBuilder.set_desired_extent(WINDOW_WIDTH, WINDOW_HEIGHT);
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

    // Get the queues from vkb::Device.
    graphicsQueue = device.get_queue(vkb::QueueType::graphics).value();
    presentQueue  = device.get_queue(vkb::QueueType::present).value();
    graphicsQueueFamilyIndex = device.get_queue_index(vkb::QueueType::graphics).value();

    // Set swapchainExtent.
    swapchainExtent = swapchain.extent;

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
        
        VkResult createViewResult = vkCreateImageView(device.device, &viewInfo, nullptr, &imageViews[i]);
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

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    VkResult createRenderPassResult = vkCreateRenderPass(device.device, &renderPassInfo, nullptr, &renderPass);
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

        VkResult createFramebufferResult = vkCreateFramebuffer(device.device, &framebufferInfo, nullptr, &framebuffers[i]);
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

    if (vkCreateCommandPool(device.device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool!");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 3;

    if (vkAllocateCommandBuffers(device.device, &allocInfo, commandBuffers)) {
        throw std::runtime_error("Failed to allocate command buffers!");
    }

    // Create semaphores and fences.
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < 3; i++)
    {
        if (vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create image available semaphores.");
        }

        if (vkCreateSemaphore(device.device, &semaphoreInfo, nullptr, &renderFinishedSemaphores[i]) != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create render finished semaphores.");
        }

        if (vkCreateFence(device.device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS)
        {
            throw runtime_error("[ERROR] Failed to create in flight fences.");
        }
    }
}

void Cleanup()
{
    // 1. Destroy swapchain first
    if (swapchain.swapchain != VK_NULL_HANDLE)
    {
        vkb::destroy_swapchain(swapchain);
        swapchain.swapchain = VK_NULL_HANDLE;
    }

    // 2. Destroy logical device
    if (device.device != VK_NULL_HANDLE)
    {
        vkb::destroy_device(device);
        device.device = VK_NULL_HANDLE;
    }

    // 3. Destroy surface BEFORE instance
    if (surface != VK_NULL_HANDLE && instance.instance != VK_NULL_HANDLE)
    {
        vkb::destroy_surface(instance, surface);
        surface = VK_NULL_HANDLE;
    }

    // 4. Destroy instance last
    if (instance.instance != VK_NULL_HANDLE)
    {
        vkb::destroy_instance(instance);
        instance.instance = VK_NULL_HANDLE;
    }

    // 5. Clean up GLFW
    if (window != nullptr)
    {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

// Setters: