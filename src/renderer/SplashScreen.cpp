#include "SplashScreen.hpp"
#include "VulkanInit.hpp"
#include "../Helpers.hpp"
#include "../Logger.hpp"
#include <GLFW/glfw3.h>
#include <stb_image.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace Volcano::SplashScreen {

namespace {

constexpr const char* SPLASH_IMAGE_PATH = "resources/splash-screen.png";

// Progress bar accent, #FF9100. The swapchain is UNORM, so clear values are
// written as-is with no sRGB encoding.
constexpr VkClearColorValue BAR_COLOR = {{ 1.0f, 145.0f / 255.0f, 0.0f, 1.0f }};
constexpr VkClearColorValue BAR_TRACK_COLOR = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

// Redraw at most this often; the event queue is pumped on every call anyway.
constexpr std::chrono::milliseconds MIN_REDRAW_INTERVAL{ 16 };

// Everything the splash keeps alive between Show() and Hide().
struct SplashState
{
    bool active = false;
    int imageWidth = 0;
    int imageHeight = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation imageAllocation = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    VkSemaphore acquireSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point lastDraw{};
};

SplashState splash;

VkShaderModule CreateShaderModuleFromFile(const std::string& path)
{
    std::vector<char> code = ReadFile(path);
    if (code.empty()) return VK_NULL_HANDLE;

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(GetDevice(), &createInfo, nullptr, &module);
    return module;
}

// Standalone allocation, never registered in VulkanInit's shared
// `allocatedBuffers` list
bool CreateOwnedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool isUMA, AllocatedBuffer& out)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = isUMA ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;

    return vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &out.buffer, &out.allocation, &out.info) == VK_SUCCESS;
}

// Fills a pixel rect of the current color attachment. Used for the progress
// bar so it needs no extra shader or pipeline.
void FillRect(VkCommandBuffer cmd, int32_t x, int32_t y, uint32_t width, uint32_t height, const VkClearColorValue& color)
{
    if (width == 0 || height == 0) return;

    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue.color = color;

    VkClearRect rect{};
    rect.rect.offset = { x, y };
    rect.rect.extent = { width, height };
    rect.layerCount = 1;
    vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
}

// Draws the logo (fit to the window and centered) and the progress bar into
// the next swapchain image and presents it. A frame that can't be acquired
// (resize, minimize) is just skipped; the next call tries again.
void DrawFrame(float progress)
{
    VkDevice dev = GetDevice();
    if (swapchainExtent.width == 0 || swapchainExtent.height == 0) return;

    uint32_t imageIndex = 0;
    VkResult acquired = vkAcquireNextImageKHR(dev, GetSwapchain(), UINT64_MAX, splash.acquireSemaphore, VK_NULL_HANDLE, &imageIndex);
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = framebuffers[imageIndex];
    renderPassInfo.renderArea.extent = swapchainExtent;

    // Black, matching the sampler's border color, so everything outside the
    // fitted logo is a seamless background.
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.2f, 0.2f, 0.2f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splash.pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchainExtent.width);
    viewport.height = static_cast<float>(swapchainExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = swapchainExtent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, splash.pipelineLayout, 0, 1, &splash.descriptorSet, 0, nullptr);

    // "Contain" fit: the quad covers the window, and the sampled UV rect is
    // grown past [0,1] on whichever axis would otherwise overflow, so the
    // whole image shows at its own aspect ratio, centered. The sampler's
    // black border fills the margin.
    float windowAspect = static_cast<float>(swapchainExtent.width) / static_cast<float>(swapchainExtent.height);
    float imageAspect = static_cast<float>(splash.imageWidth) / static_cast<float>(splash.imageHeight);
    glm::vec2 uvScale = (imageAspect > windowAspect)
        ? glm::vec2(1.0f, imageAspect / windowAspect)
        : glm::vec2(windowAspect / imageAspect, 1.0f);
    vkCmdPushConstants(cmd, splash.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uvScale), &uvScale);

    vkCmdDraw(cmd, 4, 1, 0, 0);

    // Minecraft-style bar: 1 outline, 1 gap, fill. Border thickness scales
    // with the window.
    uint32_t border = std::max(2u, swapchainExtent.height / 270u);
    uint32_t barWidth = swapchainExtent.width / 2;
    uint32_t barHeight = std::max(border * 8, swapchainExtent.height / 40);
    int32_t barX = static_cast<int32_t>((swapchainExtent.width - barWidth) / 2);
    int32_t barY = static_cast<int32_t>(swapchainExtent.height * 0.88f);

    FillRect(cmd, barX, barY, barWidth, barHeight, BAR_COLOR);
    FillRect(cmd, barX + border, barY + border, barWidth - border * 2, barHeight - border * 2, BAR_TRACK_COLOR);

    uint32_t fillMaxWidth = barWidth - border * 4;
    uint32_t fillWidth = static_cast<uint32_t>(fillMaxWidth * std::clamp(progress, 0.0f, 1.0f));
    FillRect(cmd, barX + border * 2, barY + border * 2, fillWidth, barHeight - border * 4, BAR_COLOR);

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &splash.acquireSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &splash.renderFinishedSemaphore;
    vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &splash.renderFinishedSemaphore;
    VkSwapchainKHR swapchains[] = { GetSwapchain() };
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;
    vkQueuePresentKHR(presentQueue, &presentInfo);

    // The semaphores are reused next frame, so wait for the GPU to finish.
    vkQueueWaitIdle(graphicsQueue);
    vkFreeCommandBuffers(dev, commandPool, 1, &cmd);

    splash.lastDraw = std::chrono::steady_clock::now();
}

} // namespace

void Show()
{
    // Let the window actually realize itself (GLFW/Windows can still be
    // finishing WM_CREATE/first-show handling right after VulkanInit::Init()
    // creates it) before drawing into it.
    glfwPollEvents();

    int imageWidth = 0, imageHeight = 0, channels = 0;
    uint8_t* pixels = stbi_load(SPLASH_IMAGE_PATH, &imageWidth, &imageHeight, &channels, STBI_rgb_alpha);
    if (!pixels)
    {
        Log::Info(std::string("[INFO] No splash screen at ") + SPLASH_IMAGE_PATH + ", skipping.");
        return;
    }

    VkDevice dev = GetDevice();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    AllocatedBuffer staging{};

    // Anything created before a failure is torn down by Hide() (every
    // handle in `splash` starts out VK_NULL_HANDLE).
    try
    {
        // Premultiply against the black background so the image's
        // transparent areas blend into it; the shader just outputs rgb.
        size_t pixelCount = static_cast<size_t>(imageWidth) * imageHeight;
        for (size_t i = 0; i < pixelCount; i++)
        {
            uint8_t* px = pixels + i * 4;
            px[0] = static_cast<uint8_t>(px[0] * px[3] / 255);
            px[1] = static_cast<uint8_t>(px[1] * px[3] / 255);
            px[2] = static_cast<uint8_t>(px[2] * px[3] / 255);
            px[3] = 255;
        }

        splash.imageWidth = imageWidth;
        splash.imageHeight = imageHeight;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent = { static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo imageAllocInfo{};
        imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(vmaAllocator, &imageInfo, &imageAllocInfo, &splash.image, &splash.imageAllocation, nullptr) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash image");
        }

        VkDeviceSize imageSize = static_cast<VkDeviceSize>(imageWidth) * imageHeight * 4;
        if (!CreateOwnedBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, staging))
        {
            throw std::runtime_error("failed to create splash staging buffer");
        }

        void* mapped = nullptr;
        vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
        std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
        vmaUnmapMemory(vmaAllocator, staging.allocation);
        stbi_image_free(pixels);
        pixels = nullptr;

        cmd = BeginOneShotCommands();
        TransitionImageLayout(cmd, splash.image, 0, 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { static_cast<uint32_t>(imageWidth), static_cast<uint32_t>(imageHeight), 1 };
        vkCmdCopyBufferToImage(cmd, staging.buffer, splash.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        TransitionImageLayout(cmd, splash.image, 0, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        EndOneShotCommands(cmd);
        cmd = VK_NULL_HANDLE; // EndOneShotCommands already freed it.

        vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
        staging = AllocatedBuffer{};

        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = splash.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &viewInfo, nullptr, &splash.imageView) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash image view");
        }

        // Clamp-to-border (opaque black) is what fills the margin around the
        // fitted image.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        if (vkCreateSampler(dev, &samplerInfo, nullptr, &splash.sampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash sampler");
        }

        // --- Descriptor set (image + sampler only) ---
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &splash.setLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash descriptor set layout");
        }

        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;
        if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &splash.descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash descriptor pool");
        }

        VkDescriptorSetAllocateInfo setAllocInfo{};
        setAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setAllocInfo.descriptorPool = splash.descriptorPool;
        setAllocInfo.descriptorSetCount = 1;
        setAllocInfo.pSetLayouts = &splash.setLayout;
        if (vkAllocateDescriptorSets(dev, &setAllocInfo, &splash.descriptorSet) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate splash descriptor set");
        }

        VkDescriptorImageInfo descImageInfo{};
        descImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        descImageInfo.imageView = splash.imageView;
        descImageInfo.sampler = splash.sampler;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = splash.descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &descImageInfo;
        vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);

        // --- Pipeline ---
        splash.vertModule = CreateShaderModuleFromFile("resources/shaders/splash.vert.spv");
        splash.fragModule = CreateShaderModuleFromFile("resources/shaders/splash.frag.spv");
        if (splash.vertModule == VK_NULL_HANDLE || splash.fragModule == VK_NULL_HANDLE)
        {
            throw std::runtime_error("failed to read splash shader files");
        }

        VkPipelineShaderStageCreateInfo vertStage{};
        vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertStage.module = splash.vertModule;
        vertStage.pName = "main";

        VkPipelineShaderStageCreateInfo fragStage{};
        fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragStage.module = splash.fragModule;
        fragStage.pName = "main";

        VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

        // No vertex buffer
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
        dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicStateInfo.pDynamicStates = dynamicStates.data();

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // This draws into the same render pass/subpass as the terrain pass,
        // which has a depth attachment
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(glm::vec2);

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &splash.setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &splash.pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash pipeline layout");
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicStateInfo;
        pipelineInfo.layout = splash.pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &splash.pipeline) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash pipeline");
        }

        VkSemaphoreCreateInfo semInfo{};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(dev, &semInfo, nullptr, &splash.acquireSemaphore) != VK_SUCCESS
            || vkCreateSemaphore(dev, &semInfo, nullptr, &splash.renderFinishedSemaphore) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create splash semaphores");
        }

        splash.active = true;
        DrawFrame(0.0f);

        Log::Info("Splash screen shown.");
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("[ERROR] SplashScreen::Show failed, continuing without it: ") + e.what());
        if (pixels) stbi_image_free(pixels);
        if (staging.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
        if (cmd != VK_NULL_HANDLE) vkFreeCommandBuffers(dev, commandPool, 1, &cmd);
        Hide();
    }
}

void SetProgress(float progress)
{
    // Always pump, even with no splash: keeps the window responsive (no
    // "Not Responding") through the blocking init steps.
    glfwPollEvents();

    if (!splash.active) return;

    // The per-asset callers can fire thousands of times; only redraw at
    // ~60 Hz, but never skip the final 100%.
    bool finished = progress >= 1.0f;
    if (!finished && std::chrono::steady_clock::now() - splash.lastDraw < MIN_REDRAW_INTERVAL) return;

    DrawFrame(progress);
}

void Hide()
{
    if (splash.imageWidth == 0) return; // Show() never got as far as creating anything.

    VkDevice dev = GetDevice();
    vkDeviceWaitIdle(dev);

    if (splash.renderFinishedSemaphore != VK_NULL_HANDLE) vkDestroySemaphore(dev, splash.renderFinishedSemaphore, nullptr);
    if (splash.acquireSemaphore != VK_NULL_HANDLE) vkDestroySemaphore(dev, splash.acquireSemaphore, nullptr);
    if (splash.pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, splash.pipeline, nullptr);
    if (splash.pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, splash.pipelineLayout, nullptr);
    if (splash.fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(dev, splash.fragModule, nullptr);
    if (splash.vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(dev, splash.vertModule, nullptr);
    if (splash.descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, splash.descriptorPool, nullptr);
    if (splash.setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, splash.setLayout, nullptr);
    if (splash.sampler != VK_NULL_HANDLE) vkDestroySampler(dev, splash.sampler, nullptr);
    if (splash.imageView != VK_NULL_HANDLE) vkDestroyImageView(dev, splash.imageView, nullptr);
    if (splash.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, splash.image, splash.imageAllocation);

    splash = SplashState{};
}

} // namespace Volcano::SplashScreen
