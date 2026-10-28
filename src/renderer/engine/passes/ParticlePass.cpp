#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>
#include "ParticlePass.hpp"
#include "../ShaderLibrary.hpp"
#include "../../VulkanInit.hpp"
#include "../../../GlobalState.hpp"
#include "../../../Logger.hpp"

namespace Volcano::Engine::Passes {

namespace {

// Must match particle.vert's Push block.
struct Push {
    glm::mat4 viewProj;
    glm::vec4 right;
    glm::vec4 up;
};

} // namespace

void ParticlePass::Create(const ShaderLibrary& shaders)
{
    try
    {
        for (Slot& slot : slots)
        {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = sizeof(ParticleInstance) * ParticleSystem::MAX_PARTICLES;
            bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &slot.buffer, &slot.allocation, &info) != VK_SUCCESS)
            {
                slot.buffer = VK_NULL_HANDLE;
                throw std::runtime_error("[ERROR] Failed to create the particle instance buffer.");
            }
            slot.mapped = static_cast<ParticleInstance*>(info.pMappedData);
        }
        CreateDescriptors();
        CreatePipeline(shaders);
    }
    catch (...)
    {
        Destroy();
        throw;
    }
    Log::Info("Particle pass active.");
}

void ParticlePass::ReloadShaders(const ShaderLibrary& shaders)
{
    if (setLayout == VK_NULL_HANDLE) return;
    DestroyPipeline();
    CreatePipeline(shaders);
}

// The atlas is built again from the sprites the next time it is needed.
void ParticlePass::ReloadTextures(const ShaderLibrary& /*shaders*/)
{
    DestroyAtlas();
    atlasFailed = false;
}

void ParticlePass::Record(FrameContext& frame)
{
    if (pipeline == VK_NULL_HANDLE) return;
    ParticleSystem* system = frame.state->particles;
    if (system == nullptr) return;
    const SceneView& scene = *frame.scene;
    if (scene.menuBackground) return;

    if (!atlasReady)
    {
        if (atlasFailed) return;
        if (!UploadAtlas(system->Sprites()))
        {
            atlasFailed = true;
            return;
        }
    }

    system->Update(frame.state, scene.deltaTime);

    Slot& slot = slots[frame.frameIndex % FRAME_SLOTS];
    size_t count = system->BuildInstances(scene.cameraPosition, scene.dayBrightness, slot.mapped, ParticleSystem::MAX_PARTICLES);
    if (count == 0) return;

    // Positions are camera-relative, so the view contributes only its rotation.
    Push push{};
    push.viewProj = scene.proj * glm::mat4(glm::mat3(scene.view));
    push.right = glm::vec4(scene.view[0][0], scene.view[1][0], scene.view[2][0], 0.0f);
    push.up = glm::vec4(scene.view[0][1], scene.view[1][1], scene.view[2][1], 0.0f);

    VkCommandBuffer cmd = frame.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &slot.buffer, &vertexOffset);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, 6, static_cast<uint32_t>(count), 0, 0);
}

// The sprites as an array of layers, nearest-sampled: pixel art.
bool ParticlePass::UploadAtlas(const ParticleSprites& sprites)
{
    const uint32_t layerCount = sprites.LayerCount();
    const uint32_t size = ParticleSprites::LAYER_SIZE;
    if (layerCount == 0) return false;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(sprites.Pixels().size());

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &stagingAlloc, &staging, &stagingAllocation, nullptr) != VK_SUCCESS)
    {
        Log::Error("[ERROR] Particles: couldn't create the staging buffer.");
        return false;
    }
    void* mapped = nullptr;
    vmaMapMemory(vmaAllocator, stagingAllocation, &mapped);
    std::memcpy(mapped, sprites.Pixels().data(), static_cast<size_t>(bytes));
    vmaUnmapMemory(vmaAllocator, stagingAllocation);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM; // The same gamma-space convention as the block textures.
    imageInfo.extent = { size, size, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = layerCount;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &imageAlloc, &atlasImage, &atlasAllocation, nullptr) != VK_SUCCESS)
    {
        vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);
        atlasImage = VK_NULL_HANDLE;
        atlasAllocation = VK_NULL_HANDLE;
        Log::Error("[ERROR] Particles: couldn't create the atlas image.");
        return false;
    }

    std::vector<VkBufferImageCopy> regions(layerCount);
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(size) * size * 4;
    for (uint32_t layer = 0; layer < layerCount; layer++)
    {
        regions[layer].bufferOffset = layer * layerBytes;
        regions[layer].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        regions[layer].imageExtent = { size, size, 1 };
    }
    VkCommandBuffer cmd = BeginOneShotCommands();
    TransitionImageLayout(cmd, atlasImage, 0, 1, 0, layerCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging, atlasImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount, regions.data());
    TransitionImageLayout(cmd, atlasImage, 0, 1, 0, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneShotCommands(cmd);
    vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = atlasImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &atlasView) != VK_SUCCESS)
    {
        atlasView = VK_NULL_HANDLE;
        DestroyAtlas();
        Log::Error("[ERROR] Particles: couldn't create the atlas view.");
        return false;
    }

    VkDescriptorImageInfo imageDescriptor{ sampler, atlasView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageDescriptor;
    vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);

    atlasReady = true;
    return true;
}

void ParticlePass::DestroyAtlas()
{
    VkDevice dev = GetDevice();
    if (atlasView != VK_NULL_HANDLE) vkDestroyImageView(dev, atlasView, nullptr);
    if (atlasImage != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, atlasImage, atlasAllocation);
    atlasView = VK_NULL_HANDLE;
    atlasImage = VK_NULL_HANDLE;
    atlasAllocation = VK_NULL_HANDLE;
    atlasReady = false;
}

void ParticlePass::CreateDescriptors()
{
    VkDevice dev = GetDevice();

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(dev, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
    {
        sampler = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] Failed to create the particle sampler.");
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
        setLayout = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] Failed to create the particle descriptor set layout.");
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(dev, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS)
    {
        descriptorPool = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] Failed to create the particle descriptor pool.");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(dev, &allocInfo, &descriptorSet) != VK_SUCCESS)
    {
        descriptorSet = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] Failed to allocate the particle descriptor set.");
    }
}

void ParticlePass::CreatePipeline(const ShaderLibrary& shaders)
{
    VkDevice dev = GetDevice();
    VkShaderModule vertModule = CreateShaderModule(shaders, VERTEX_SHADER);
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, FRAGMENT_SHADER);
    }
    catch (...)
    {
        vkDestroyShaderModule(dev, vertModule, nullptr);
        throw;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    // One ParticleInstance per particle, not per vertex.
    VkVertexInputBindingDescription binding{ 0, sizeof(ParticleInstance), VK_VERTEX_INPUT_RATE_INSTANCE };
    VkVertexInputAttributeDescription attributes[4]{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleInstance, positionSize) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleInstance, color) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ParticleInstance, uv) };
    attributes[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ParticleInstance, layer) };
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 4;
    vertexInput.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE; // A billboard faces the camera whichever way its triangles wind.
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Behind terrain and entities, but never in front of them in the depth buffer the Hi-Z pyramid is built from.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push) };
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;

    VkResult result = vkCreatePipelineLayout(dev, &layoutInfo, nullptr, &pipelineLayout);
    if (result == VK_SUCCESS)
    {
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
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        result = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        if (result != VK_SUCCESS) pipeline = VK_NULL_HANDLE;
    }
    else
    {
        pipelineLayout = VK_NULL_HANDLE;
    }

    vkDestroyShaderModule(dev, fragModule, nullptr);
    vkDestroyShaderModule(dev, vertModule, nullptr);

    if (result != VK_SUCCESS)
    {
        DestroyPipeline();
        throw std::runtime_error("[ERROR] Failed to create the particle pipeline.");
    }
}

void ParticlePass::DestroyPipeline()
{
    VkDevice dev = GetDevice();
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
}

void ParticlePass::Destroy()
{
    VkDevice dev = GetDevice();
    DestroyPipeline();
    DestroyAtlas();
    if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, descriptorPool, nullptr); // Frees the set too.
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE) vkDestroySampler(dev, sampler, nullptr);
    descriptorPool = VK_NULL_HANDLE;
    descriptorSet = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
    atlasFailed = false;
    for (Slot& slot : slots)
    {
        if (slot.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, slot.buffer, slot.allocation);
        slot = Slot{};
    }
}

} // namespace Volcano::Engine::Passes
