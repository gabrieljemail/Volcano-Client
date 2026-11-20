#include "PincelRasterizerVulkan.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_set>
#include "../VulkanInit.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"
#include "../engine/vulkan/VulkanRenderEngine.hpp"

namespace Volcano::GUI {

#ifdef _WIN32
std::unique_ptr<PincelRasterizer> CreateD3D11PincelRasterizer();
#endif

namespace {

constexpr const char* VERTEX_SHADER = "resources/shaders/pincel_draw.vert.spv";
constexpr const char* FRAGMENT_SHADER = "resources/shaders/pincel_draw.frag.spv";
constexpr uint32_t TEXTURE_DESCRIPTOR_LIMIT = 1024;

struct PushConstants {
    std::array<float, 4> viewport{};
    std::array<float, 4> rect{};
    std::array<float, 4> borders{};
    Pincel::DrawColor borderColor;
    float radius = 0.0f;
    int32_t kind = 0;
};

static_assert(sizeof(PushConstants) == 72);

VkShaderModule LoadShader(const char* path)
{
    std::vector<char> code = ReadFile(path);
    if (code.empty())
    {
        Log::Error(std::string("PincelRasterizer: missing shader ") + path);
        return VK_NULL_HANDLE;
    }
    return CreateShaderModule(code, path);
}

template<typename Texture>
bool CreateTexture(Texture& texture, uint32_t width, uint32_t height, VkFormat format,
    VkDescriptorSetLayout setLayout, VkDescriptorPool descriptorPool,
    VkSampler linearSampler, VkSampler nearestSampler)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &imageAlloc, &texture.image, &texture.allocation, nullptr) != VK_SUCCESS)
    {
        Log::Error("PincelRasterizer: failed to create a texture image");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &texture.view) != VK_SUCCESS)
    {
        Log::Error("PincelRasterizer: failed to create a texture view");
        return false;
    }

    VkDescriptorSetLayout layouts[] = { setLayout, setLayout };
    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool = descriptorPool;
    setInfo.descriptorSetCount = 2;
    setInfo.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(GetDevice(), &setInfo, texture.descriptors) != VK_SUCCESS)
    {
        Log::Error("PincelRasterizer: failed to allocate texture descriptors");
        return false;
    }

    VkDescriptorImageInfo imageInfos[] = {
        { nearestSampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { linearSampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; i++)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = texture.descriptors[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(GetDevice(), 2, writes, 0, nullptr);
    texture.width = width;
    texture.height = height;
    return true;
}

template<typename Texture>
void DestroyTexture(Texture& texture, VkDescriptorPool descriptorPool)
{
    if (texture.descriptors[0] != VK_NULL_HANDLE)
    {
        vkFreeDescriptorSets(GetDevice(), descriptorPool, 2, texture.descriptors);
    }
    if (texture.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), texture.view, nullptr);
    if (texture.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, texture.image, texture.allocation);
    texture = {};
}

} // namespace

VulkanPincelRasterizer::~VulkanPincelRasterizer()
{
    Destroy();
}

bool VulkanPincelRasterizer::Create(Engine::RenderEngine& engine)
{
    Engine::Vulkan::VulkanRenderEngine* vulkan = engine.GetVulkan();
    if (vulkan == nullptr) return false;
    VkRenderPass targetRenderPass = vulkan->GetMainRenderPass();
    if (targetRenderPass == VK_NULL_HANDLE) return false;
    renderPass = targetRenderPass;
    VkDevice dev = GetDevice();

    try
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(dev, &samplerInfo, nullptr, &linearSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create the linear sampler");
        }

        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        if (vkCreateSampler(dev, &samplerInfo, nullptr, &nearestSampler) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create the nearest sampler");
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
            throw std::runtime_error("failed to create the descriptor set layout");
        }

        VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TEXTURE_DESCRIPTOR_LIMIT * 2 + 4 };
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = TEXTURE_DESCRIPTOR_LIMIT * 2 + 4;
        if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create the descriptor pool");
        }
        if (!CreateTexture(fallbackTexture, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                setLayout, descriptorPool, linearSampler, nearestSampler))
        {
            throw std::runtime_error("failed to create the fallback texture");
        }

        VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants) };
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &setLayout;
        pipelineLayoutInfo.pushConstantRangeCount = 1;
        pipelineLayoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(dev, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("failed to create the pipeline layout");
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

        VkVertexInputBindingDescription vertexBinding{};
        vertexBinding.binding = 0;
        vertexBinding.stride = sizeof(Pincel::DrawVertex);
        vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        std::array<VkVertexInputAttributeDescription, 3> attributes{};
        attributes[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Pincel::DrawVertex, x) };
        attributes[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Pincel::DrawVertex, u) };
        attributes[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
            offsetof(Pincel::DrawVertex, color) + offsetof(Pincel::DrawColor, r) };
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

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

        std::array<VkDynamicState, 2> dynamicStates{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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
        if (built != VK_SUCCESS) throw std::runtime_error("failed to create the graphics pipeline");
    }
    catch (const std::exception& error)
    {
        Log::Error(std::string("PincelRasterizer: ") + error.what());
        Destroy();
        return false;
    }
    return true;
}

void VulkanPincelRasterizer::Destroy()
{
    VkDevice dev = GetDevice();
    if (dev != VK_NULL_HANDLE && graphicsQueue != VK_NULL_HANDLE) vkQueueWaitIdle(graphicsQueue);

    for (auto& [id, texture] : imageTextures)
    {
        (void)id;
        DestroyTexture(texture, descriptorPool);
    }
    imageTextures.clear();
    DestroyTexture(fontTexture, descriptorPool);
    DestroyTexture(fallbackTexture, descriptorPool);

    for (FrameBuffers& frame : frames)
    {
        if (frame.vertexMapped != nullptr) vmaUnmapMemory(vmaAllocator, frame.vertexBuffer.allocation);
        if (frame.vertexBuffer.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(vmaAllocator, frame.vertexBuffer.buffer, frame.vertexBuffer.allocation);
        if (frame.indexMapped != nullptr) vmaUnmapMemory(vmaAllocator, frame.indexBuffer.allocation);
        if (frame.indexBuffer.buffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(vmaAllocator, frame.indexBuffer.buffer, frame.indexBuffer.allocation);
        frame = {};
    }

    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, descriptorPool, nullptr);
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    if (linearSampler != VK_NULL_HANDLE) vkDestroySampler(dev, linearSampler, nullptr);
    if (nearestSampler != VK_NULL_HANDLE) vkDestroySampler(dev, nearestSampler, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    descriptorPool = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    linearSampler = VK_NULL_HANDLE;
    nearestSampler = VK_NULL_HANDLE;
    uploadedVersion = 0;
    currentFrame = 0;
    renderPass = VK_NULL_HANDLE;
}

void VulkanPincelRasterizer::Upload(const Pincel::DrawData& data)
{
    if (pipeline == VK_NULL_HANDLE) return;

    FrameBuffers& frame = frames[currentFrame];
    struct PendingUpload {
        GpuTexture* texture = nullptr;
        AllocatedBuffer staging;
    };
    std::vector<PendingUpload> pending;
    bool fontUploadQueued = false;
    auto queueUpload = [&](GpuTexture& texture, const uint8_t* pixels, size_t byteCount) -> bool
    {
        AllocatedBuffer staging = CreateBuffer(byteCount, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
        if (staging.buffer == VK_NULL_HANDLE)
        {
            Log::Error("PincelRasterizer: failed to create a staging buffer");
            return false;
        }
        void* mapped = nullptr;
        if (vmaMapMemory(vmaAllocator, staging.allocation, &mapped) != VK_SUCCESS)
        {
            vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
            Log::Error("PincelRasterizer: failed to map a staging buffer");
            return false;
        }
        std::memcpy(mapped, pixels, byteCount);
        vmaFlushAllocation(vmaAllocator, staging.allocation, 0, byteCount);
        vmaUnmapMemory(vmaAllocator, staging.allocation);
        pending.push_back({ &texture, staging });
        return true;
    };

    static constexpr std::array<uint8_t, 4> whitePixel{ 255, 255, 255, 255 };
    if (!fallbackTexture.hasContent && !queueUpload(fallbackTexture, whitePixel.data(), whitePixel.size())) return;

    std::unordered_set<uint64_t> liveImages;
    for (const Pincel::ImageTextureView& source : data.images)
    {
        if (source.id == 0 || source.pixels == nullptr || source.width == 0 || source.height == 0
            || source.width > std::numeric_limits<size_t>::max() / 4 / source.height
            || source.byteCount != static_cast<size_t>(source.width) * source.height * 4)
        {
            Log::Error("PincelRasterizer: received an invalid image texture");
            continue;
        }

        liveImages.insert(source.id);
        GpuTexture& texture = imageTextures[source.id];
        if (texture.image == VK_NULL_HANDLE)
        {
            if (imageTextures.size() > TEXTURE_DESCRIPTOR_LIMIT)
            {
                Log::Error("PincelRasterizer: texture descriptor limit reached");
                continue;
            }
            if (!CreateTexture(texture, source.width, source.height, VK_FORMAT_R8G8B8A8_UNORM,
                    setLayout, descriptorPool, linearSampler, nearestSampler))
            {
                DestroyTexture(texture, descriptorPool);
                continue;
            }
        }
        else if (texture.width != source.width || texture.height != source.height)
        {
            Log::Error("PincelRasterizer: an image id changed dimensions");
            continue;
        }

        if (!texture.hasContent) queueUpload(texture, source.pixels, source.byteCount);
    }

    if (data.fontAtlas.pixels != nullptr && data.fontAtlas.width > 0 && data.fontAtlas.height > 0
        && (fontTexture.image == VK_NULL_HANDLE || fontTexture.width != data.fontAtlas.width
            || fontTexture.height != data.fontAtlas.height || fontTextureVersion != data.fontAtlas.version))
    {
        if (data.fontAtlas.width > std::numeric_limits<size_t>::max() / data.fontAtlas.height)
        {
            Log::Error("PincelRasterizer: font atlas dimensions overflow");
        }
        else
        {
            if (fontTexture.image != VK_NULL_HANDLE
                && (fontTexture.width != data.fontAtlas.width || fontTexture.height != data.fontAtlas.height))
            {
                vkQueueWaitIdle(graphicsQueue);
                DestroyTexture(fontTexture, descriptorPool);
                fontTextureVersion = 0;
            }
            if (fontTexture.image == VK_NULL_HANDLE
                && !CreateTexture(fontTexture, data.fontAtlas.width, data.fontAtlas.height, VK_FORMAT_R8_UNORM,
                    setLayout, descriptorPool, linearSampler, nearestSampler))
            {
                DestroyTexture(fontTexture, descriptorPool);
            }
            if (fontTexture.image != VK_NULL_HANDLE)
            {
                size_t byteCount = static_cast<size_t>(data.fontAtlas.width) * data.fontAtlas.height;
                fontUploadQueued = queueUpload(fontTexture, data.fontAtlas.pixels, byteCount);
            }
        }
    }

    if (!pending.empty())
    {
        try
        {
            VkCommandBuffer commandBuffer = BeginOneShotCommands();
            for (const PendingUpload& upload : pending)
            {
                VkImageMemoryBarrier toTransfer{};
                toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toTransfer.oldLayout = upload.texture->hasContent
                    ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
                toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toTransfer.srcAccessMask = upload.texture->hasContent ? VK_ACCESS_SHADER_READ_BIT : 0;
                toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toTransfer.image = upload.texture->image;
                toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                vkCmdPipelineBarrier(commandBuffer,
                    upload.texture->hasContent ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

                VkBufferImageCopy region{};
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageExtent = { upload.texture->width, upload.texture->height, 1 };
                vkCmdCopyBufferToImage(commandBuffer, upload.staging.buffer, upload.texture->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                VkImageMemoryBarrier toShader = toTransfer;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
            }
            EndOneShotCommands(commandBuffer);
            for (const PendingUpload& upload : pending) upload.texture->hasContent = true;
            if (fontUploadQueued) fontTextureVersion = data.fontAtlas.version;
        }
        catch (const std::exception& error)
        {
            Log::Error(std::string("PincelRasterizer: texture upload failed: ") + error.what());
        }
    }

    for (const PendingUpload& upload : pending)
    {
        vmaDestroyBuffer(vmaAllocator, upload.staging.buffer, upload.staging.allocation);
    }

    bool removedTexture = std::any_of(imageTextures.begin(), imageTextures.end(),
        [&](const auto& entry) { return !liveImages.contains(entry.first); });
    if (removedTexture) vkQueueWaitIdle(graphicsQueue);
    for (auto it = imageTextures.begin(); it != imageTextures.end();)
    {
        if (!liveImages.contains(it->first))
        {
            DestroyTexture(it->second, descriptorPool);
            it = imageTextures.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (data.vertices.size() > std::numeric_limits<uint32_t>::max() - 4
        || data.indices.size() > std::numeric_limits<uint32_t>::max() - 6)
    {
        Log::Error("PincelRasterizer: draw data exceeds the 32-bit index limit");
        return;
    }

    std::vector<Pincel::DrawVertex> vertices = data.vertices;
    std::vector<uint32_t> indices = data.indices;
    frame.backgroundIndexCount = 0;
    if (data.background.a > 0.0f && data.width > 0 && data.height > 0)
    {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(), {
            { 0.0f, 0.0f, 0.0f, 0.0f, data.background },
            { static_cast<float>(data.width), 0.0f, 1.0f, 0.0f, data.background },
            { static_cast<float>(data.width), static_cast<float>(data.height), 1.0f, 1.0f, data.background },
            { 0.0f, static_cast<float>(data.height), 0.0f, 1.0f, data.background },
        });
        frame.backgroundFirstIndex = static_cast<uint32_t>(indices.size());
        indices.insert(indices.end(), { base, base + 1, base + 2, base + 2, base + 3, base });
        frame.backgroundIndexCount = 6;
    }

    auto uploadBuffer = [&](AllocatedBuffer& buffer, void*& mapped, size_t& capacity,
        const void* source, size_t byteCount, VkBufferUsageFlags usage) -> bool
    {
        if (byteCount == 0) return true;
        if (buffer.buffer == VK_NULL_HANDLE || capacity < byteCount)
        {
            if (mapped != nullptr) vmaUnmapMemory(vmaAllocator, buffer.allocation);
            if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
            buffer = CreateBuffer(byteCount, usage, true);
            mapped = nullptr;
            capacity = 0;
            if (buffer.buffer == VK_NULL_HANDLE || vmaMapMemory(vmaAllocator, buffer.allocation, &mapped) != VK_SUCCESS)
            {
                if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
                buffer = {};
                mapped = nullptr;
                Log::Error("PincelRasterizer: failed to allocate a draw buffer");
                return false;
            }
            capacity = byteCount;
        }
        std::memcpy(mapped, source, byteCount);
        vmaFlushAllocation(vmaAllocator, buffer.allocation, 0, byteCount);
        return true;
    };

    if (!uploadBuffer(frame.vertexBuffer, frame.vertexMapped, frame.vertexCapacity,
            vertices.data(), vertices.size() * sizeof(Pincel::DrawVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        || !uploadBuffer(frame.indexBuffer, frame.indexMapped, frame.indexCapacity,
            indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
    {
        return;
    }
    frame.indexCount = static_cast<uint32_t>(indices.size());
    frame.canvasWidth = data.width;
    frame.canvasHeight = data.height;
    frame.commands = data.commands;
    uploadedVersion++;
}

void VulkanPincelRasterizer::Record(Engine::FrameContext& frameContext)
{
    VkCommandBuffer commandBuffer = frameContext.commandBuffer;
    if (commandBuffer == VK_NULL_HANDLE) return;
    if (pipeline == VK_NULL_HANDLE || uploadedVersion == 0) return;
    const FrameBuffers& frame = frames[currentFrame];
    if (frame.vertexBuffer.buffer == VK_NULL_HANDLE || frame.indexBuffer.buffer == VK_NULL_HANDLE
        || frame.canvasWidth == 0 || frame.canvasHeight == 0)
    {
        currentFrame = (currentFrame + 1) % FRAME_COUNT;
        return;
    }

    VkViewport viewport{ 0.0f, 0.0f, static_cast<float>(frame.canvasWidth),
        static_cast<float>(frame.canvasHeight), 0.0f, 1.0f };
    VkRect2D fullScissor{ { 0, 0 }, { frame.canvasWidth, frame.canvasHeight } };
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &fullScissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &frame.vertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(commandBuffer, frame.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    auto draw = [&](uint32_t firstIndex, uint32_t indexCount, const Pincel::DrawCommand* command,
        VkDescriptorSet descriptor)
    {
        if (indexCount == 0 || descriptor == VK_NULL_HANDLE || indexCount > frame.indexCount
            || firstIndex > frame.indexCount - indexCount) return;

        VkRect2D scissor = fullScissor;
        if (command != nullptr && command->hasClip)
        {
            int64_t left = std::max<int64_t>(0, command->clip.x);
            int64_t top = std::max<int64_t>(0, command->clip.y);
            int64_t right = std::min<int64_t>(frame.canvasWidth,
                static_cast<int64_t>(command->clip.x) + std::max(command->clip.width, 0));
            int64_t bottom = std::min<int64_t>(frame.canvasHeight,
                static_cast<int64_t>(command->clip.y) + std::max(command->clip.height, 0));
            if (right <= left || bottom <= top) return;
            scissor.offset = { static_cast<int32_t>(left), static_cast<int32_t>(top) };
            scissor.extent = { static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top) };
        }
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            0, 1, &descriptor, 0, nullptr);

        PushConstants push;
        push.viewport = { static_cast<float>(frame.canvasWidth), static_cast<float>(frame.canvasHeight), 0.0f, 0.0f };
        if (command != nullptr)
        {
            push.rect = { static_cast<float>(command->rect.x), static_cast<float>(command->rect.y),
                static_cast<float>(command->rect.width), static_cast<float>(command->rect.height) };
            for (size_t i = 0; i < command->border.size(); i++) push.borders[i] = static_cast<float>(command->border[i]);
            push.borderColor = command->borderColor;
            push.radius = static_cast<float>(command->radius);
            push.kind = command->kind == Pincel::DrawCommandKind::Box ? 0
                : command->kind == Pincel::DrawCommandKind::Image ? 1 : 2;
        }
        else
        {
            push.rect = { 0.0f, 0.0f, static_cast<float>(frame.canvasWidth), static_cast<float>(frame.canvasHeight) };
        }
        vkCmdPushConstants(commandBuffer, pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDrawIndexed(commandBuffer, indexCount, 1, firstIndex, 0, 0);
    };

    if (frame.backgroundIndexCount != 0)
    {
        draw(frame.backgroundFirstIndex, frame.backgroundIndexCount, nullptr, fallbackTexture.descriptors[0]);
    }

    for (const Pincel::DrawCommand& command : frame.commands)
    {
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        if (command.kind == Pincel::DrawCommandKind::Box)
        {
            descriptor = fallbackTexture.descriptors[0];
        }
        else if (command.kind == Pincel::DrawCommandKind::Glyph)
        {
            if (fontTexture.hasContent) descriptor = fontTexture.descriptors[0];
        }
        else if (command.kind == Pincel::DrawCommandKind::Image)
        {
            auto found = imageTextures.find(command.imageId);
            if (found != imageTextures.end() && found->second.hasContent)
            {
                descriptor = found->second.descriptors[command.imageFilter == 1 ? 0 : 1];
            }
        }
        draw(static_cast<uint32_t>(command.firstIndex), static_cast<uint32_t>(command.indexCount), &command, descriptor);
    }

    currentFrame = (currentFrame + 1) % FRAME_COUNT;
}

std::unique_ptr<PincelRasterizer> CreatePincelRasterizer(Engine::RenderEngine& engine)
{
    if (engine.GetVulkan() != nullptr)
    {
        return std::make_unique<VulkanPincelRasterizer>();
    }
#ifdef _WIN32
    if (engine.GetD3D11() != nullptr)
    {
        return CreateD3D11PincelRasterizer();
    }
#endif
    return nullptr;
}

} // namespace Volcano::GUI
