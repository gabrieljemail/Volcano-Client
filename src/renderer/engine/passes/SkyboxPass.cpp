#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <stb_image.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "SkyboxPass.hpp"
#include "../ShaderLibrary.hpp"
#include "../../VulkanInit.hpp"
#include "../../../Logger.hpp"

namespace Volcano::Engine::Passes {

namespace {

constexpr const char* PANORAMA_DIR = "resources/minecraft/assets/minecraft/textures/gui/title/background/";

// Cube layer (+X,-X,+Y,-Y,+Z,-Z) -> panorama_N.png, from vanilla's
// CubeMapTexture.SUFFIXES.
constexpr std::array<int, 6> FACE_FILE = { 1, 3, 5, 4, 0, 2 };

// CubeMap.PROJECTION_*, and the fixed downward tilt it renders with.
constexpr float FOV_DEGREES = 85.0f;
constexpr float NEAR_PLANE = 0.05f;
constexpr float FAR_PLANE = 10.0f;
constexpr float PITCH_DEGREES = 10.0f;

// Vanilla's Panorama adds deltaTicks * panoramaSpeed(1.0) * 0.1 degrees per
// tick, at 20 ticks a second.
constexpr float SPIN_DEGREES_PER_SECOND = 2.0f;

struct Push {
    glm::mat4 mvp;
};

} // namespace

void SkyboxPass::Create(const ShaderLibrary& shaders)
{
    if (!LoadCubemap()) return;

    try
    {
        CreateDescriptors();
        CreatePipeline(shaders);
    }
    catch (...)
    {
        Destroy();
        throw;
    }
    Log::Info("[INFO] Skybox pass active.");
}

void SkyboxPass::ReloadShaders(const ShaderLibrary& shaders)
{
    if (cubeView == VK_NULL_HANDLE) return;
    DestroyPipeline();
    CreatePipeline(shaders);
}

bool SkyboxPass::LoadCubemap()
{
    std::array<stbi_uc*, 6> faces{};
    int width = 0, height = 0;
    bool ok = true;

    for (int layer = 0; layer < 6 && ok; layer++)
    {
        std::string path = std::string(PANORAMA_DIR) + "panorama_" + std::to_string(FACE_FILE[layer]) + ".png";
        int w, h, channels;
        faces[layer] = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!faces[layer])
        {
            Log::Error("[ERROR] Skybox: failed to load " + path);
            ok = false;
        }
        else if (layer == 0)
        {
            width = w;
            height = h;
        }
        else if (w != width || h != height)
        {
            Log::Error("[ERROR] Skybox: panorama face sizes differ (" + path + ").");
            ok = false;
        }
    }

    if (ok && width != height)
    {
        Log::Error("[ERROR] Skybox: panorama faces must be square.");
        ok = false;
    }

    if (!ok)
    {
        for (stbi_uc* face : faces) if (face) stbi_image_free(face);
        return false;
    }

    // The client jar ships 1x1 stand-ins here; the real cherry-grove images
    // have to be dropped in over them.
    if (width < 16)
    {
        Log::Info("[WARN] Skybox: panorama faces are only " + std::to_string(width) + "x" + std::to_string(height)
            + " — placeholder textures, the menu background will be a flat color.");
    }

    const VkDeviceSize faceBytes = static_cast<VkDeviceSize>(width) * height * 4;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = faceBytes * 6;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    vmaCreateBuffer(vmaAllocator, &bufferInfo, &stagingAlloc, &staging, &stagingAllocation, nullptr);

    // Rows are copied bottom-up, matching vanilla's copyRect(..., flipY = true).
    void* mapped = nullptr;
    vmaMapMemory(vmaAllocator, stagingAllocation, &mapped);
    auto* dst = static_cast<uint8_t*>(mapped);
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int layer = 0; layer < 6; layer++)
    {
        for (int row = 0; row < height; row++)
        {
            std::memcpy(dst + layer * faceBytes + static_cast<size_t>(row) * rowBytes,
                faces[layer] + static_cast<size_t>(height - 1 - row) * rowBytes, rowBytes);
        }
        stbi_image_free(faces[layer]);
    }
    vmaUnmapMemory(vmaAllocator, stagingAllocation);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 6;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo imageAlloc{};
    imageAlloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vmaAllocator, &imageInfo, &imageAlloc, &cubeImage, &cubeAllocation, nullptr) != VK_SUCCESS)
    {
        vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);
        cubeImage = VK_NULL_HANDLE;
        cubeAllocation = VK_NULL_HANDLE;
        Log::Error("[ERROR] Skybox: failed to create the cubemap image.");
        return false;
    }

    std::array<VkBufferImageCopy, 6> regions{};
    for (uint32_t layer = 0; layer < 6; layer++)
    {
        regions[layer].bufferOffset = layer * faceBytes;
        regions[layer].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        regions[layer].imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    }

    VkCommandBuffer cmd = BeginOneShotCommands();
    TransitionImageLayout(cmd, cubeImage, 0, 1, 0, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging, cubeImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        static_cast<uint32_t>(regions.size()), regions.data());
    TransitionImageLayout(cmd, cubeImage, 0, 1, 0, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = cubeImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &cubeView) != VK_SUCCESS)
    {
        cubeView = VK_NULL_HANDLE;
        Destroy();
        Log::Error("[ERROR] Skybox: failed to create the cubemap view.");
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(GetDevice(), &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
    {
        sampler = VK_NULL_HANDLE;
        Destroy();
        Log::Error("[ERROR] Skybox: failed to create the cubemap sampler.");
        return false;
    }
    return true;
}

void SkyboxPass::CreateDescriptors()
{
    VkDevice dev = GetDevice();

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
        throw std::runtime_error("[ERROR] Failed to create skybox descriptor set layout.");
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
        throw std::runtime_error("[ERROR] Failed to create skybox descriptor pool.");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &setLayout;
    if (vkAllocateDescriptorSets(dev, &allocInfo, &descriptorSet) != VK_SUCCESS)
    {
        descriptorSet = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] Failed to allocate skybox descriptor set.");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = sampler;
    imageInfo.imageView = cubeView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

void SkyboxPass::CreatePipeline(const ShaderLibrary& shaders)
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

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Background only: everything after it draws over it, so no depth work.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blendAttachment{};
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
        throw std::runtime_error("[ERROR] Failed to create skybox pipeline.");
    }
}

void SkyboxPass::Record(FrameContext& frame)
{
    if (pipeline == VK_NULL_HANDLE || !frame.scene->menuBackground) return;

    float aspect = static_cast<float>(frame.extent.width)
        / static_cast<float>(frame.extent.height > 0 ? frame.extent.height : 1);
    glm::mat4 proj = glm::perspective(glm::radians(FOV_DEGREES), aspect, NEAR_PLANE, FAR_PLANE);
    proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.

    // CubeMap.render: rotationX(pi) . rotateX(pitch) . rotateY(-spin).
    float spin = std::fmod(frame.scene->time * SPIN_DEGREES_PER_SECOND, 360.0f);
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(PITCH_DEGREES), glm::vec3(1, 0, 0));
    model = glm::rotate(model, glm::radians(-spin), glm::vec3(0, 1, 0));

    Push push{ proj * model };

    VkCommandBuffer cmd = frame.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), &push);
    vkCmdDraw(cmd, 36, 1, 0, 0);
}

void SkyboxPass::DestroyPipeline()
{
    VkDevice dev = GetDevice();
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipeline, nullptr);
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
    pipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
}

void SkyboxPass::Destroy()
{
    VkDevice dev = GetDevice();
    DestroyPipeline();
    if (descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, descriptorPool, nullptr);
    if (setLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
    if (sampler != VK_NULL_HANDLE) vkDestroySampler(dev, sampler, nullptr);
    if (cubeView != VK_NULL_HANDLE) vkDestroyImageView(dev, cubeView, nullptr);
    if (cubeImage != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, cubeImage, cubeAllocation);
    descriptorPool = VK_NULL_HANDLE;
    descriptorSet = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
    cubeView = VK_NULL_HANDLE;
    cubeImage = VK_NULL_HANDLE;
    cubeAllocation = VK_NULL_HANDLE;
}

} // namespace Volcano::Engine::Passes
