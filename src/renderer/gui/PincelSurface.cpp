#include "PincelSurface.hpp"
#include <array>
#include <cstring>
#include "../VulkanInit.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"

namespace Volcano::GUI {

namespace {

constexpr const char* VERTEX_SHADER = "resources/shaders/pincel.vert.spv";
constexpr const char* FRAGMENT_SHADER = "resources/shaders/pincel.frag.spv";

VkShaderModule LoadShader(const char* path)
{
    std::vector<char> code = ReadFile(path);
    if (code.empty())
    {
        Log::Error(std::string("PincelSurface: missing shader ") + path);
        return VK_NULL_HANDLE;
    }
    return CreateShaderModule(code, path);
}

} // namespace

bool PincelSurface::Create(VkRenderPass renderPass)
{
    VkDevice dev = GetDevice();

    try
    {
        // Pincel draws 1:1, so nearest keeps its pixels exact.
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(dev, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create sampler");
        }

        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create descriptor set layout");
        }

        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;
        if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create descriptor pool");
        }

        VkDescriptorSetAllocateInfo setInfo{};
        setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        setInfo.descriptorPool = descriptorPool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &setLayout;
        if (vkAllocateDescriptorSets(dev, &setInfo, &descriptorSet) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to allocate descriptor set");
        }

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create pipeline layout");
        }

        VkShaderModule vertex = LoadShader(VERTEX_SHADER);
        VkShaderModule fragment = LoadShader(FRAGMENT_SHADER);
        if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE)
        {
            if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(dev, vertex, nullptr);
            if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(dev, fragment, nullptr);
            throw std::runtime_error("failed to load shaders");
        }

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        // Drawn last, over everything: no depth test, no depth write.
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
            | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &blend;
        pipelineInfo.pDynamicState = &dynamic;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;

        VkResult built = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(dev, vertex, nullptr);
        vkDestroyShaderModule(dev, fragment, nullptr);
        if (built != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create pipeline");
        }
    }
    catch (const std::exception& error)
    {
        Log::Error(std::string("PincelSurface: ") + error.what());
        Destroy();
        return false;
    }
    return true;
}

void PincelSurface::Destroy()
{
    VkDevice dev = GetDevice();
    DestroyImage();
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, descriptorPool, nullptr); // Frees its set.
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE) vkDestroySampler(dev, sampler, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    descriptorPool = VK_NULL_HANDLE;
    descriptorSet = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
}

void PincelSurface::DestroyImage()
{
    // The last frame may still be reading the old image.
    vkQueueWaitIdle(graphicsQueue);
    VkDevice dev = GetDevice();

    if (imageView != VK_NULL_HANDLE) vkDestroyImageView(dev, imageView, nullptr);
    if (image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, image, imageAllocation);
    if (staging != VK_NULL_HANDLE)
    {
        vmaUnmapMemory(vmaAllocator, stagingAllocation);
        vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);
    }
    imageView = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    imageAllocation = VK_NULL_HANDLE;
    staging = VK_NULL_HANDLE;
    stagingAllocation = VK_NULL_HANDLE;
    stagingMapped = nullptr;
    width = 0;
    height = 0;
    uploadedVersion = 0;
}

bool PincelSurface::CreateImage(uint32_t newWidth, uint32_t newHeight)
{
    DestroyImage();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { newWidth, newHeight, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &imageAlloc, &image, &imageAllocation, nullptr) != VK_SUCCESS)
    {
        Log::Error("PincelSurface: failed to create the image");
        image = VK_NULL_HANDLE;
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = static_cast<VkDeviceSize>(newWidth) * newHeight * 4;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo bufferAlloc{};
    bufferAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    bufferAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &bufferAlloc, &staging, &stagingAllocation, nullptr) != VK_SUCCESS
        || vmaMapMemory(vmaAllocator, stagingAllocation, &stagingMapped) != VK_SUCCESS)
    {
        Log::Error("PincelSurface: failed to create the staging buffer");
        if (staging != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);
        staging = VK_NULL_HANDLE; // So DestroyImage doesn't unmap what never mapped.
        DestroyImage();
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &imageView) != VK_SUCCESS)
    {
        Log::Error("PincelSurface: failed to create the image view");
        imageView = VK_NULL_HANDLE;
        DestroyImage();
        return false;
    }

    VkDescriptorImageInfo descriptorImage{};
    descriptorImage.sampler = sampler;
    descriptorImage.imageView = imageView;
    descriptorImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptorImage;
    vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);

    width = newWidth;
    height = newHeight;
    return true;
}

void PincelSurface::Upload(const Pincel::Texture& texture)
{
    if (pipeline == VK_NULL_HANDLE || texture.data == nullptr || texture.width == 0 || texture.height == 0) return;

    if (texture.width != width || texture.height != height)
    {
        if (!CreateImage(texture.width, texture.height)) return;
    }
    if (texture.version == uploadedVersion) return;

    std::memcpy(stagingMapped, texture.data, texture.Size());

    // The whole image is replaced, so whatever it held doesn't matter.
    VkCommandBuffer cmd = BeginOneShotCommands();
    TransitionImageLayout(cmd, image, 0, 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { width, height, 1 };
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    TransitionImageLayout(cmd, image, 0, 1, 0, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneShotCommands(cmd);

    uploadedVersion = texture.version;
}

void PincelSurface::Record(VkCommandBuffer commandBuffer)
{
    if (pipeline == VK_NULL_HANDLE || uploadedVersion == 0) return;

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchainExtent.width);
    viewport.height = static_cast<float>(swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    VkRect2D scissor{};
    scissor.extent = swapchainExtent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

} // namespace Volcano::GUI
