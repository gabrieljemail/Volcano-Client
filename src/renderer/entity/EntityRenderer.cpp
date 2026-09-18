#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include "EntityRenderer.hpp"
#include "models/Entity.hpp"
#include "models/EntityRegistry.hpp"
#include "models/HumanoidModel.hpp"
#include "../VulkanInit.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"

namespace Volcano {

namespace {

// Matches VulkanInit's fixed-size per-frame arrays (cameraSets, commandBuffers,
// etc.) — those are sized 3 even though maxFramesInFlight is 2, so we mirror
// that instead of introducing a second, possibly-inconsistent constant.
constexpr uint32_t FRAMES_IN_FLIGHT = 3;

// Local vertex format for the static placeholder cube — entities need their
// own vertex layout (position + normal) since they don't share the chunk
// mesh's packed-block format at all.
struct EntityVertex {
    glm::vec3 position;
    glm::vec3 normal;
};

// std430 layout: 3 vec4s, 16-byte aligned throughout.
struct GPUEntityInstance {
    glm::vec4 positionYaw;     // xyz: world position (feet), w: yaw radians.
    glm::vec4 halfExtentsHurt; // xyz: half bounding-box extents, w: hurt flash amount (0-1).
    glm::vec4 color;           // rgb: base tint, a: unused.
};

VkPipeline g_pipeline = VK_NULL_HANDLE;
VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_instanceSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_instanceSets[FRAMES_IN_FLIGHT];

AllocatedBuffer g_instanceBuffers[FRAMES_IN_FLIGHT];
void* g_instanceBuffersMapped[FRAMES_IN_FLIGHT];
AllocatedBuffer g_indirectBuffers[FRAMES_IN_FLIGHT];
void* g_indirectBuffersMapped[FRAMES_IN_FLIGHT];

AllocatedBuffer g_cubeVertexBuffer;
AllocatedBuffer g_cubeIndexBuffer;
uint32_t g_cubeIndexCount = 0;

// Humanoid (player/zombie/mannequin) path — a real UV-mapped skeleton mesh
// (HumanoidModel), drawn one entity at a time rather than batched, since
// each draw needs its own bound skin texture. Everything else keeps using
// the flat-tinted placeholder cube above, completely unchanged. See
// EntityRenderer::RecordDraw's own comment for why these entities are
// split out of the cube instance list rather than merged in.
struct PushConstants {
    glm::vec4 positionYaw;
    float hurtAmount;
};

// One loaded skin — its own image/view/sampler/descriptor set, registered
// with g_textureSetLayout's single combined-image-sampler binding. Not
// registered in VulkanInit's global allocatedImages list, for the same
// double-free reason CreateOwnedBuffer's own comment gives for buffers.
struct SkinTexture {
    AllocatedImage image{};
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// One humanoid entity's fully-resolved draw parameters, snapshotted under
// entitiesMutex. Deliberately NOT a `const Entity*`: unlike the cube path
// (which finishes writing its instance buffer before the lock drops), the
// humanoid draw loop runs after RecordDraw releases entitiesMutex, and the
// network thread can erase an entity from state->entities in that window —
// so a pointer into the map would be a use-after-free by the time it's
// dereferenced. The skin pointer is fine to keep: it aims at a file-scope
// global that outlives every frame.
struct HumanoidDraw {
    glm::vec3 position;
    float yaw;
    float hurtAmount;
    const SkinTexture* skin;
};

VkPipeline g_humanoidPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_humanoidPipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_textureSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_textureDescriptorPool = VK_NULL_HANDLE;

AllocatedBuffer g_humanoidVertexBuffer;
AllocatedBuffer g_humanoidIndexBuffer;
uint32_t g_humanoidIndexCount = 0;

SkinTexture g_steveSkin; // player, mannequin — this client has no real skin-fetching yet, see LoadSkins' own comment.
SkinTexture g_zombieSkin;

// Allocates a buffer via VMA without registering it in VulkanInit's global
// `allocatedBuffers` list. EntityRenderer::Shutdown() destroys every buffer
// it creates itself (unmapping first where needed) — going through the
// global list here would mean both it and VulkanInit::Cleanup()'s own sweep
// try to destroy the same VkBuffer/VmaAllocation, which is a double free.
AllocatedBuffer CreateOwnedBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool isUMA)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = isUMA ? VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT : 0;

    AllocatedBuffer result{};
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &result.info) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] EntityRenderer failed to allocate a buffer.");
    }
    return result;
}

// Builds a unit cube (local -0.5..0.5 on each axis) with 4 unique vertices
// per face (so each face keeps its own flat normal) — 24 vertices, 36
// indices. Winding follows the same convention as ChunkMesher/terrain.vert:
// per face, u x v == the face normal, and corners run
// (0,0)->(1,0)->(1,1)->(0,1), which is CCW seen from outside — required for
// VulkanInit's VK_FRONT_FACE_CLOCKWISE + VK_CULL_MODE_BACK_BIT rasterizer
// state (see CreateGraphicsPipeline's comment on why) to cull the right
// faces instead of turning every cube inside-out.
void BuildCubeMesh(std::vector<EntityVertex>& vertices, std::vector<uint32_t>& indices)
{
    struct Face { glm::vec3 normal; glm::vec3 u; glm::vec3 v; };
    const std::array<Face, 6> faces = {{
        { glm::vec3( 1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1) },
        { glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1), glm::vec3(0, 1, 0) },
        { glm::vec3( 0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0) },
        { glm::vec3( 0,-1, 0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1) },
        { glm::vec3( 0, 0, 1), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0) },
        { glm::vec3( 0, 0,-1), glm::vec3(0, 1, 0), glm::vec3(1, 0, 0) },
    }};

    for (const Face& face : faces)
    {
        glm::vec3 center = face.normal * 0.5f;
        uint32_t base = static_cast<uint32_t>(vertices.size());

        vertices.push_back({ center - 0.5f * face.u - 0.5f * face.v, face.normal });
        vertices.push_back({ center + 0.5f * face.u - 0.5f * face.v, face.normal });
        vertices.push_back({ center + 0.5f * face.u + 0.5f * face.v, face.normal });
        vertices.push_back({ center - 0.5f * face.u + 0.5f * face.v, face.normal });

        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

void UploadCubeMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    BuildCubeMesh(vertices, indices);
    g_cubeIndexCount = static_cast<uint32_t>(indices.size());

    VkDeviceSize vertexBytes = vertices.size() * sizeof(EntityVertex);
    VkDeviceSize indexBytes = indices.size() * sizeof(uint32_t);

    g_cubeVertexBuffer = CreateOwnedBuffer(vertexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    g_cubeIndexBuffer = CreateOwnedBuffer(indexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), (size_t)vertexBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, indices.data(), (size_t)indexBytes);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_cubeVertexBuffer.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_cubeIndexBuffer.buffer, 1, &indexCopy);

    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

void UploadHumanoidMesh()
{
    std::vector<HumanoidModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    HumanoidModel::BuildMesh(vertices, indices);
    g_humanoidIndexCount = static_cast<uint32_t>(indices.size());

    VkDeviceSize vertexBytes = vertices.size() * sizeof(HumanoidModel::Vertex);
    VkDeviceSize indexBytes = indices.size() * sizeof(uint32_t);

    g_humanoidVertexBuffer = CreateOwnedBuffer(vertexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    g_humanoidIndexBuffer = CreateOwnedBuffer(indexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), (size_t)vertexBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, indices.data(), (size_t)indexBytes);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_humanoidVertexBuffer.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, g_humanoidIndexBuffer.buffer, 1, &indexCopy);

    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

// Loads one skin PNG as its own plain 2D texture (no array, no mipmaps —
// unlike TextureManager's block/item array, a skin is viewed close-up on a
// single humanoid-sized entity, so there's no minification pressure worth
// the extra blit-chain complexity) and registers a descriptor set for it
// against g_textureSetLayout. Any size works (unlike TextureManager, which
// only accepts exactly 16x16) — player/zombie skins are both 64x64 in this
// resource pack, but nothing here assumes that.
SkinTexture LoadSkinTexture(const std::string& path)
{
    int width, height, channels;
    uint8_t* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to load skin texture: " + path);
    }

    SkinTexture skin;
    VkDeviceSize imageBytes = static_cast<VkDeviceSize>(width) * height * 4;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &skin.image.image, &skin.image.allocation, nullptr) != VK_SUCCESS)
    {
        stbi_image_free(pixels);
        throw std::runtime_error("[ERROR] EntityRenderer: failed to allocate skin texture image: " + path);
    }

    AllocatedBuffer staging = CreateOwnedBuffer(imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, pixels, static_cast<size_t>(imageBytes));
    vmaUnmapMemory(vmaAllocator, staging.allocation);
    stbi_image_free(pixels);

    VkCommandBuffer cmd = BeginOneShotCommands();
    TransitionImageLayout(cmd, skin.image.image, 0, 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = imageInfo.extent;
    vkCmdCopyBufferToImage(cmd, staging.buffer, skin.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    TransitionImageLayout(cmd, skin.image.image, 0, 1, 0, 1,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = skin.image.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_SRGB;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(GetDevice(), &viewInfo, nullptr, &skin.view) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to create skin texture view: " + path);
    }

    // Nearest-neighbor: skin sheets are hand-painted pixel art viewed close
    // up, same reasoning TextureManager's block/item array would apply if
    // it exposed a filter choice — linear filtering would blur the seams
    // between adjacent UV-unwrapped box faces.
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    if (vkCreateSampler(GetDevice(), &samplerInfo, nullptr, &skin.sampler) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to create skin texture sampler: " + path);
    }

    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = g_textureDescriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &g_textureSetLayout;
    if (vkAllocateDescriptorSets(GetDevice(), &dsAlloc, &skin.descriptorSet) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to allocate skin texture descriptor set: " + path);
    }

    VkDescriptorImageInfo descImage{};
    descImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descImage.imageView = skin.view;
    descImage.sampler = skin.sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = skin.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descImage;
    vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);

    return skin;
}

// This client has no real skin fetching (no Mojang session-server lookup —
// see NetworkClient's own comment on why it doesn't attempt signed chat/
// online-mode auth either), so every player and mannequin uses the same
// bundled default skin. Zombie gets its own vanilla texture — the whole
// point of this pass is "one shared humanoid shape, texture is what tells
// them apart," per the task this was scoped to.
void LoadSkins()
{
    g_steveSkin = LoadSkinTexture("resources/minecraft/assets/minecraft/textures/entity/player/wide/steve.png");
    g_zombieSkin = LoadSkinTexture("resources/minecraft/assets/minecraft/textures/entity/zombie/zombie.png");
}

void DestroySkin(SkinTexture& skin)
{
    if (skin.sampler != VK_NULL_HANDLE) vkDestroySampler(GetDevice(), skin.sampler, nullptr);
    if (skin.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), skin.view, nullptr);
    if (skin.image.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, skin.image.image, skin.image.allocation);
    skin = SkinTexture{};
}

// Which bundled skin a humanoid-shaped entity type uses, or nullptr if
// `typeName` isn't one of the three types this pass covers (everything
// else keeps rendering via the placeholder cube path, unchanged).
const SkinTexture* SkinForEntityType(const std::string& typeName)
{
    if (typeName == "player" || typeName == "mannequin") return &g_steveSkin;
    if (typeName == "zombie") return &g_zombieSkin;
    return nullptr;
}

void CreateDescriptorLayoutAndPool()
{
    VkDescriptorSetLayoutBinding instanceBinding{};
    instanceBinding.binding = 0;
    instanceBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    instanceBinding.descriptorCount = 1;
    instanceBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &instanceBinding;
    if (vkCreateDescriptorSetLayout(GetDevice(), &layoutInfo, nullptr, &g_instanceSetLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity instance descriptor set layout.");
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FRAMES_IN_FLIGHT };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = FRAMES_IN_FLIGHT;
    if (vkCreateDescriptorPool(GetDevice(), &poolInfo, nullptr, &g_descriptorPool) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity descriptor pool.");
    }

    // Humanoid skin textures — a separate layout/pool from the instance SSBO
    // above (different descriptor type, and allocated per distinct skin
    // texture rather than per frame-in-flight).
    VkDescriptorSetLayoutBinding textureBinding{};
    textureBinding.binding = 0;
    textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    textureBinding.descriptorCount = 1;
    textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo textureLayoutInfo{};
    textureLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    textureLayoutInfo.bindingCount = 1;
    textureLayoutInfo.pBindings = &textureBinding;
    if (vkCreateDescriptorSetLayout(GetDevice(), &textureLayoutInfo, nullptr, &g_textureSetLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity skin texture descriptor set layout.");
    }

    // Sized generously (8, not 2) since this is a one-descriptor-per-
    // distinct-skin pool for the life of the app — cheap to over-provision,
    // and headroom for more humanoid-skinned types later without touching
    // this again.
    constexpr uint32_t MAX_SKIN_TEXTURES = 8;
    VkDescriptorPoolSize texturePoolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SKIN_TEXTURES };
    VkDescriptorPoolCreateInfo texturePoolInfo{};
    texturePoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    texturePoolInfo.poolSizeCount = 1;
    texturePoolInfo.pPoolSizes = &texturePoolSize;
    texturePoolInfo.maxSets = MAX_SKIN_TEXTURES;
    if (vkCreateDescriptorPool(GetDevice(), &texturePoolInfo, nullptr, &g_textureDescriptorPool) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity skin texture descriptor pool.");
    }
}

void CreatePerFrameBuffers()
{
    VkDeviceSize instanceBytes = EntityRenderer::MAX_VISIBLE_ENTITIES * sizeof(GPUEntityInstance);

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        g_instanceBuffers[i] = CreateOwnedBuffer(instanceBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        vmaMapMemory(vmaAllocator, g_instanceBuffers[i].allocation, &g_instanceBuffersMapped[i]);

        g_indirectBuffers[i] = CreateOwnedBuffer(sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        vmaMapMemory(vmaAllocator, g_indirectBuffers[i].allocation, &g_indirectBuffersMapped[i]);

        VkDrawIndexedIndirectCommand initialCommand{};
        initialCommand.indexCount = g_cubeIndexCount;
        initialCommand.instanceCount = 0;
        memcpy(g_indirectBuffersMapped[i], &initialCommand, sizeof(initialCommand));

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = g_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &g_instanceSetLayout;
        if (vkAllocateDescriptorSets(GetDevice(), &allocInfo, &g_instanceSets[i]) != VK_SUCCESS)
        {
            throw std::runtime_error("[ERROR] Failed to allocate entity instance descriptor set.");
        }

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = g_instanceBuffers[i].buffer;
        bufferInfo.range = instanceBytes;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = g_instanceSets[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(GetDevice(), 1, &write, 0, nullptr);
    }
}

VkShaderModule CreateShaderModuleFromFile(const std::string& path)
{
    std::vector<char> code = ReadFile(path);
    if (code.empty())
    {
        throw std::runtime_error("[ERROR] Failed to read shader file: " + path);
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    if (vkCreateShaderModule(GetDevice(), &createInfo, nullptr, &module) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create shader module: " + path);
    }
    return module;
}

void CreatePipeline()
{
    VkShaderModule vertModule = CreateShaderModuleFromFile("resources/shaders/entity.vert.spv");
    VkShaderModule fragModule = CreateShaderModuleFromFile("resources/shaders/entity.frag.spv");

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(EntityVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(EntityVertex, position);
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(EntityVertex, normal);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicStateInfo{};
    dynamicStateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicStateInfo.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicStateInfo.pDynamicStates = dynamicStates.data();

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    // Same winding/cull convention as the terrain pipeline (see
    // CreateGraphicsPipeline's comment) — BuildCubeMesh's winding matches it.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Entities share the chunk pass's depth attachment within the same
    // subpass/render pass, so depth test/write here is what makes walls
    // correctly occlude entities behind them (and vice versa) regardless of
    // draw order — no extra wiring needed for that culling method.
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, g_instanceSetLayout };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(GetDevice(), &pipelineLayoutInfo, nullptr, &g_pipelineLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity pipeline layout.");
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
    pipelineInfo.layout = g_pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_pipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create entity graphics pipeline.");
    }

    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
}

// Same fixed-function state as CreatePipeline() above (same render pass/
// subpass, same depth test since humanoids share the world's depth
// attachment too, same winding/cull convention — HumanoidModel::BuildMesh
// follows BuildCubeMesh's own winding rule deliberately), differing only in
// vertex layout (+UV, no per-instance SSBO), shaders, and descriptor set 1
// (a skin texture instead of the instance buffer) plus a push constant
// range (this path draws one entity per call, not instanced).
void CreateHumanoidPipeline()
{
    VkShaderModule vertModule = CreateShaderModuleFromFile("resources/shaders/entity_textured.vert.spv");
    VkShaderModule fragModule = CreateShaderModuleFromFile("resources/shaders/entity_textured.frag.spv");

    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = { vertStage, fragStage };

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(HumanoidModel::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(HumanoidModel::Vertex, position);
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(HumanoidModel::Vertex, normal);
    attributes[2].binding = 0;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(HumanoidModel::Vertex, uv);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    std::vector<VkDynamicState> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
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
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(PushConstants);

    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, g_textureSetLayout };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
    if (vkCreatePipelineLayout(GetDevice(), &pipelineLayoutInfo, nullptr, &g_humanoidPipelineLayout) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create humanoid entity pipeline layout.");
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
    pipelineInfo.layout = g_humanoidPipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &g_humanoidPipeline) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] Failed to create humanoid entity graphics pipeline.");
    }

    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
}

// Left/Right/Bottom/Top frustum planes (Gribb/Hartmann) extracted from a
// combined view-projection matrix. Near/far are deliberately skipped: the
// rasterizer's own clip-space clipping already handles anything we'd miss
// there, and the distance-sorted MAX_VISIBLE_ENTITIES cap in RecordDraw is a
// more meaningful "too far/too much" limit than a math-derived far plane.
std::array<glm::vec4, 4> ExtractFrustumPlanes(const glm::mat4& m)
{
    std::array<glm::vec4, 4> planes = {
        glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]), // Left
        glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]), // Right
        glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]), // Bottom
        glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]), // Top
    };

    for (glm::vec4& p : planes)
    {
        float len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }
    return planes;
}

bool SphereInFrustum(const std::array<glm::vec4, 4>& planes, const glm::vec3& center, float radius)
{
    for (const glm::vec4& p : planes)
    {
        if (glm::dot(glm::vec3(p), center) + p.w < -radius) return false;
    }
    return true;
}

} // namespace

void EntityRenderer::Init()
{
    UploadCubeMesh();
    CreateDescriptorLayoutAndPool();
    CreatePerFrameBuffers();
    CreatePipeline();

    // Humanoid path — CreateDescriptorLayoutAndPool() above must run first
    // (LoadSkins() allocates each skin's descriptor set from
    // g_textureDescriptorPool against g_textureSetLayout, both created
    // there).
    UploadHumanoidMesh();
    LoadSkins();
    CreateHumanoidPipeline();

    Log::Info("[INFO] Entity renderer initialized.");
}

void EntityRenderer::Shutdown()
{
    if (g_pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_pipeline, nullptr);
        g_pipeline = VK_NULL_HANDLE;
    }
    if (g_pipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetDevice(), g_pipelineLayout, nullptr);
        g_pipelineLayout = VK_NULL_HANDLE;
    }

    // Destroying the pool frees every set allocated from it.
    if (g_descriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(GetDevice(), g_descriptorPool, nullptr);
        g_descriptorPool = VK_NULL_HANDLE;
    }
    if (g_instanceSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(GetDevice(), g_instanceSetLayout, nullptr);
        g_instanceSetLayout = VK_NULL_HANDLE;
    }

    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
    {
        if (g_instanceBuffersMapped[i] != nullptr)
        {
            vmaUnmapMemory(vmaAllocator, g_instanceBuffers[i].allocation);
            g_instanceBuffersMapped[i] = nullptr;
        }
        if (g_instanceBuffers[i].buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, g_instanceBuffers[i].buffer, g_instanceBuffers[i].allocation);
            g_instanceBuffers[i] = AllocatedBuffer{};
        }

        if (g_indirectBuffersMapped[i] != nullptr)
        {
            vmaUnmapMemory(vmaAllocator, g_indirectBuffers[i].allocation);
            g_indirectBuffersMapped[i] = nullptr;
        }
        if (g_indirectBuffers[i].buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, g_indirectBuffers[i].buffer, g_indirectBuffers[i].allocation);
            g_indirectBuffers[i] = AllocatedBuffer{};
        }
    }

    if (g_cubeVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_cubeVertexBuffer.buffer, g_cubeVertexBuffer.allocation);
        g_cubeVertexBuffer = AllocatedBuffer{};
    }
    if (g_cubeIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_cubeIndexBuffer.buffer, g_cubeIndexBuffer.allocation);
        g_cubeIndexBuffer = AllocatedBuffer{};
    }

    // Humanoid path.
    if (g_humanoidPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_humanoidPipeline, nullptr);
        g_humanoidPipeline = VK_NULL_HANDLE;
    }
    if (g_humanoidPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetDevice(), g_humanoidPipelineLayout, nullptr);
        g_humanoidPipelineLayout = VK_NULL_HANDLE;
    }

    DestroySkin(g_steveSkin);
    DestroySkin(g_zombieSkin);

    // Destroying the pool frees every skin's descriptor set allocated from it.
    if (g_textureDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(GetDevice(), g_textureDescriptorPool, nullptr);
        g_textureDescriptorPool = VK_NULL_HANDLE;
    }
    if (g_textureSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(GetDevice(), g_textureSetLayout, nullptr);
        g_textureSetLayout = VK_NULL_HANDLE;
    }

    if (g_humanoidVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_humanoidVertexBuffer.buffer, g_humanoidVertexBuffer.allocation);
        g_humanoidVertexBuffer = AllocatedBuffer{};
    }
    if (g_humanoidIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_humanoidIndexBuffer.buffer, g_humanoidIndexBuffer.allocation);
        g_humanoidIndexBuffer = AllocatedBuffer{};
    }
}

void EntityRenderer::RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state,
    const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPosition)
{
    std::array<glm::vec4, 4> frustumPlanes = ExtractFrustumPlanes(proj * view);

    // Frustum-cull first, then keep only the closest MAX_VISIBLE_ENTITIES —
    // the "simple entity limit" culling method from the entity renderer
    // plan, applied after frustum culling so it prioritizes what's actually
    // in view instead of arbitrarily dropping visible entities in favor of
    // off-screen ones.
    auto now = std::chrono::steady_clock::now();

    std::vector<std::pair<float, const Entity*>> visible;
    // Humanoid-shaped entities (player/zombie/mannequin) are pulled out of
    // the batched cube path entirely and drawn separately below via
    // HumanoidModel — they need a real UV-mapped mesh and their own bound
    // skin texture, neither of which the flat-tinted cube instance SSBO (one
    // shared mesh, one shared pipeline, no per-instance texture) has any
    // way to carry. Distance-sorting/MAX_VISIBLE_ENTITIES still applies to
    // them via the same `visible` pass below; they're just routed to a
    // different draw call afterward instead of into `instances`. Holds
    // resolved values rather than Entity pointers — see HumanoidDraw's own
    // comment for why that distinction is load-bearing here.
    std::vector<HumanoidDraw> humanoidVisible;
    uint32_t cubeInstanceCount = 0;
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        visible.reserve(state->entities.size());

        for (const auto& [id, entity] : state->entities)
        {
            if (!entity.visible) continue;

            float halfWidth = entity.boundingBox.x * 0.5f;
            float halfHeight = entity.boundingBox.y * 0.5f;
            glm::vec3 center = entity.InterpolatedPosition(now) + glm::vec3(0.0f, halfHeight, 0.0f);
            float radius = glm::length(glm::vec3(halfWidth, halfHeight, halfWidth));

            if (!SphereInFrustum(frustumPlanes, center, radius)) continue;

            float distanceSq = glm::dot(center - cameraPosition, center - cameraPosition);
            visible.emplace_back(distanceSq, &entity);
        }

        if (visible.size() > MAX_VISIBLE_ENTITIES)
        {
            std::partial_sort(visible.begin(), visible.begin() + MAX_VISIBLE_ENTITIES, visible.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            visible.resize(MAX_VISIBLE_ENTITIES);
        }

        // Build the GPU instance buffer while still holding the lock, since
        // `visible` (and humanoidVisible, built alongside it here) holds raw
        // pointers into state->entities.
        std::vector<GPUEntityInstance> instances;
        instances.reserve(visible.size());
        for (const auto& [distanceSq, entity] : visible)
        {
            const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity->type);
            const SkinTexture* skin = info ? SkinForEntityType(info->name) : nullptr;
            if (skin != nullptr)
            {
                humanoidVisible.push_back(HumanoidDraw{
                    entity->InterpolatedPosition(now), entity->InterpolatedYaw(now), entity->hurtAmount, skin });
                continue;
            }

            GPUEntityInstance gpu{};
            gpu.positionYaw = glm::vec4(entity->InterpolatedPosition(now), entity->InterpolatedYaw(now));
            gpu.halfExtentsHurt = glm::vec4(
                entity->boundingBox.x * 0.5f, entity->boundingBox.y * 0.5f, entity->boundingBox.x * 0.5f,
                entity->hurtAmount);
            gpu.color = glm::vec4(entity->color, 1.0f);
            instances.push_back(gpu);
        }

        if (!instances.empty())
        {
            memcpy(g_instanceBuffersMapped[frameIndex], instances.data(), instances.size() * sizeof(GPUEntityInstance));
        }
        cubeInstanceCount = static_cast<uint32_t>(instances.size());
    }

    VkDrawIndexedIndirectCommand command{};
    command.indexCount = g_cubeIndexCount;
    command.instanceCount = cubeInstanceCount;
    memcpy(g_indirectBuffersMapped[frameIndex], &command, sizeof(command));

    if (cubeInstanceCount > 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

        VkDescriptorSet sets[] = { cameraSets[frameIndex], g_instanceSets[frameIndex] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 2, sets, 0, nullptr);

        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_cubeVertexBuffer.buffer, &vertexOffset);
        vkCmdBindIndexBuffer(cmd, g_cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
    }

    // Humanoid path: one draw per entity (not instanced/indirect — the
    // batched approach above exists for potentially hundreds of flat-tinted
    // cubes; a handful of textured players/zombies/mannequins doesn't need
    // that complexity, and each needs its own bound skin texture anyway).
    // Static bind pose only — position/yaw come straight from the same
    // InterpolatedPosition/InterpolatedYaw every other entity uses, sampled
    // fresh every frame right here on the render thread. See
    // HumanoidModel.hpp's own comment on where a future animated pose
    // should be sampled from (here, not TickLoop).
    if (!humanoidVisible.empty())
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_humanoidPipeline);

        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_humanoidVertexBuffer.buffer, &vertexOffset);
        vkCmdBindIndexBuffer(cmd, g_humanoidIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        for (const HumanoidDraw& draw : humanoidVisible)
        {
            VkDescriptorSet sets[] = { cameraSets[frameIndex], draw.skin->descriptorSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_humanoidPipelineLayout, 0, 2, sets, 0, nullptr);

            PushConstants pc{};
            pc.positionYaw = glm::vec4(draw.position, draw.yaw);
            pc.hurtAmount = draw.hurtAmount;
            vkCmdPushConstants(cmd, g_humanoidPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd, g_humanoidIndexCount, 1, 0, 0, 0);
        }
    }
}

} // namespace Volcano
