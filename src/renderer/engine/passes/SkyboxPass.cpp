#include <algorithm>
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
#include "../../../resources/ImageLoad.hpp"

namespace Volcano::Engine::Passes {

namespace {

constexpr const char* PANORAMA_DIR = "assets/minecraft/textures/gui/title/background/"; // Logical pack paths.
constexpr const char* CELESTIAL_DIR = "assets/minecraft/textures/environment/celestial/";

// Moon phases in vanilla order (phase 0 = full moon).
constexpr std::array<const char*, 8> MOON_PHASES = {
    "full_moon", "waning_gibbous", "third_quarter", "waning_crescent",
    "new_moon", "waxing_crescent", "first_quarter", "waxing_gibbous" };

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

// Must match skybox.frag's Push block.
struct Push {
    glm::mat4 mvp;
    glm::vec4 skyTop;   // w = mode (0 panorama, 1 sky)
    glm::vec4 horizon;  // w = moon phase
    glm::vec4 sunDir;
};

} // namespace

void SkyboxPass::Create(const ShaderLibrary& shaders)
{
    if (!LoadCubemap() || !LoadCelestial())
    {
        Destroy();
        return;
    }

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
    Log::Info("Skybox pass active.");
}

void SkyboxPass::ReloadShaders(const ShaderLibrary& shaders)
{
    if (cubeView == VK_NULL_HANDLE) return;
    DestroyPipeline();
    CreatePipeline(shaders);
}

void SkyboxPass::ReloadTextures(const ShaderLibrary& shaders)
{
    Destroy();
    Create(shaders);
}

bool SkyboxPass::UploadLayers(const std::vector<const uint8_t*>& layers, uint32_t size, bool cube,
                              VkImage& image, VmaAllocation& allocation, VkImageView& view)
{
    const VkDeviceSize layerBytes = static_cast<VkDeviceSize>(size) * size * 4;
    const uint32_t layerCount = static_cast<uint32_t>(layers.size());

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = layerBytes * layerCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo stagingAlloc{};
    stagingAlloc.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAlloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = VK_NULL_HANDLE;
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &stagingAlloc, &staging, &stagingAllocation, nullptr) != VK_SUCCESS)
    {
        return false;
    }

    void* mapped = nullptr;
    vmaMapMemory(vmaAllocator, stagingAllocation, &mapped);
    for (uint32_t layer = 0; layer < layerCount; layer++)
    {
        std::memcpy(static_cast<uint8_t*>(mapped) + layer * layerBytes, layers[layer], static_cast<size_t>(layerBytes));
    }
    vmaUnmapMemory(vmaAllocator, stagingAllocation);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
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

    if (vmaCreateImage(vmaAllocator, &imageInfo, &imageAlloc, &image, &allocation, nullptr) != VK_SUCCESS)
    {
        vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        return false;
    }

    std::vector<VkBufferImageCopy> regions(layerCount);
    for (uint32_t layer = 0; layer < layerCount; layer++)
    {
        regions[layer].bufferOffset = layer * layerBytes;
        regions[layer].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        regions[layer].imageExtent = { size, size, 1 };
    }

    VkCommandBuffer cmd = BeginOneShotCommands();
    TransitionImageLayout(cmd, image, 0, 1, 0, layerCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layerCount, regions.data());
    TransitionImageLayout(cmd, image, 0, 1, 0, layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneShotCommands(cmd);
    vmaDestroyBuffer(vmaAllocator, staging, stagingAllocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &view) != VK_SUCCESS)
    {
        view = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool SkyboxPass::LoadCubemap()
{
    std::array<stbi_uc*, 6> faces{};
    int width = 0, height = 0;
    bool ok = true;

    for (int layer = 0; layer < 6 && ok; layer++)
    {
        std::string path = std::string(PANORAMA_DIR) + "panorama_" + std::to_string(FACE_FILE[layer]) + ".png";
        int w = 0, h = 0;
        faces[layer] = LoadImageRGBA(path, w, h);
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

    // Without a usable panorama, a flat 1x1 cube keeps the in-game sky working.
    static const uint8_t FALLBACK_PIXEL[4] = { 40, 40, 48, 255 };
    std::vector<std::vector<uint8_t>> flipped;
    std::vector<const uint8_t*> layers;
    uint32_t size = 1;
    if (ok)
    {
        // Rows are copied bottom-up, matching vanilla's copyRect(..., flipY = true).
        size = static_cast<uint32_t>(width);
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        for (int layer = 0; layer < 6; layer++)
        {
            std::vector<uint8_t> face(rowBytes * height);
            for (int row = 0; row < height; row++)
            {
                std::memcpy(face.data() + row * rowBytes, faces[layer] + static_cast<size_t>(height - 1 - row) * rowBytes, rowBytes);
            }
            flipped.push_back(std::move(face));
        }
        for (const auto& face : flipped) layers.push_back(face.data());
    }
    else
    {
        layers.assign(6, FALLBACK_PIXEL);
    }
    for (stbi_uc* face : faces) if (face) stbi_image_free(face);

    if (!UploadLayers(layers, size, true, cubeImage, cubeAllocation, cubeView))
    {
        Log::Error("[ERROR] Skybox: failed to create the cubemap.");
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
        Log::Error("[ERROR] Skybox: failed to create the cubemap sampler.");
        return false;
    }
    return true;
}

bool SkyboxPass::LoadCelestial()
{
    std::vector<std::string> paths{ std::string(CELESTIAL_DIR) + "sun.png" };
    for (const char* phase : MOON_PHASES) paths.push_back(std::string(CELESTIAL_DIR) + "moon/" + phase + ".png");

    // A missing texture becomes a transparent layer rather than failing the
    // sky. Every layer shares one size: the largest square texture (32 for
    // vanilla, more for high-resolution packs), smaller ones scaled up nearest.
    std::vector<stbi_uc*> images(paths.size(), nullptr);
    std::vector<int> widths(paths.size(), 0);
    uint32_t SIZE = 32;
    for (size_t i = 0; i < paths.size(); i++)
    {
        int w = 0, h = 0;
        images[i] = LoadImageRGBA(paths[i], w, h);
        if (images[i] && w == h && w > 0)
        {
            widths[i] = w;
            SIZE = std::max(SIZE, static_cast<uint32_t>(w));
        }
        else
        {
            Log::Error("[ERROR] Skybox: couldn't use " + paths[i] + " (missing, or not square).");
        }
    }

    std::vector<std::vector<uint8_t>> pixels(paths.size(), std::vector<uint8_t>(static_cast<size_t>(SIZE) * SIZE * 4, 0));
    for (size_t i = 0; i < paths.size(); i++)
    {
        if (images[i] && widths[i] > 0)
        {
            const uint32_t srcSize = static_cast<uint32_t>(widths[i]);
            for (uint32_t y = 0; y < SIZE; y++)
            {
                for (uint32_t x = 0; x < SIZE; x++)
                {
                    const uint8_t* src = images[i] + (static_cast<size_t>(y * srcSize / SIZE) * srcSize + x * srcSize / SIZE) * 4;
                    std::memcpy(pixels[i].data() + (static_cast<size_t>(y) * SIZE + x) * 4, src, 4);
                }
            }
        }
        if (images[i]) stbi_image_free(images[i]);
    }

    std::vector<const uint8_t*> layers;
    for (const auto& layer : pixels) layers.push_back(layer.data());
    if (!UploadLayers(layers, SIZE, false, celestialImage, celestialAllocation, celestialView))
    {
        Log::Error("[ERROR] Skybox: failed to create the sun/moon texture.");
        return false;
    }

    // Nearest: pixel art.
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
    if (vkCreateSampler(GetDevice(), &samplerInfo, nullptr, &celestialSampler) != VK_SUCCESS)
    {
        celestialSampler = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void SkyboxPass::CreateDescriptors()
{
    VkDevice dev = GetDevice();

    VkDescriptorSetLayoutBinding bindings[2]{};
    for (uint32_t i = 0; i < 2; i++)
    {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &layoutInfo, nullptr, &setLayout) != VK_SUCCESS)
    {
        setLayout = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] Failed to create skybox descriptor set layout.");
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
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

    VkDescriptorImageInfo imageInfos[2]{};
    imageInfos[0] = { sampler, cubeView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    imageInfos[1] = { celestialSampler, celestialView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; i++)
    {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);
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

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push) };
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
    if (pipeline == VK_NULL_HANDLE) return;
    const SceneView& scene = *frame.scene;

    Push push{};
    if (scene.menuBackground)
    {
        float aspect = static_cast<float>(frame.extent.width)
            / static_cast<float>(frame.extent.height > 0 ? frame.extent.height : 1);
        glm::mat4 proj = glm::perspective(glm::radians(FOV_DEGREES), aspect, NEAR_PLANE, FAR_PLANE);
        proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.

        // CubeMap.render: rotationX(pi) . rotateX(pitch) . rotateY(-spin).
        float spin = std::fmod(scene.time * SPIN_DEGREES_PER_SECOND, 360.0f);
        glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::pi<float>(), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(PITCH_DEGREES), glm::vec3(1, 0, 0));
        model = glm::rotate(model, glm::radians(-spin), glm::vec3(0, 1, 0));
        push.mvp = proj * model;
        push.skyTop.w = 0.0f;
    }
    else
    {
        // The cube's corners are world-space directions: the camera's
        // rotation only, so the sky never moves with the player.
        push.mvp = scene.proj * glm::mat4(glm::mat3(scene.view));
        push.skyTop = glm::vec4(scene.skyTopColor, 1.0f);
        push.horizon = glm::vec4(scene.skyColor, static_cast<float>(scene.moonPhase));
        push.sunDir = glm::vec4(scene.sunDirection, 0.0f);
    }

    VkCommandBuffer cmd = frame.commandBuffer;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push), &push);
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
    if (celestialSampler != VK_NULL_HANDLE) vkDestroySampler(dev, celestialSampler, nullptr);
    if (celestialView != VK_NULL_HANDLE) vkDestroyImageView(dev, celestialView, nullptr);
    if (celestialImage != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, celestialImage, celestialAllocation);
    descriptorPool = VK_NULL_HANDLE;
    descriptorSet = VK_NULL_HANDLE;
    setLayout = VK_NULL_HANDLE;
    sampler = VK_NULL_HANDLE;
    cubeView = VK_NULL_HANDLE;
    cubeImage = VK_NULL_HANDLE;
    cubeAllocation = VK_NULL_HANDLE;
    celestialSampler = VK_NULL_HANDLE;
    celestialView = VK_NULL_HANDLE;
    celestialImage = VK_NULL_HANDLE;
    celestialAllocation = VK_NULL_HANDLE;
}

} // namespace Volcano::Engine::Passes
