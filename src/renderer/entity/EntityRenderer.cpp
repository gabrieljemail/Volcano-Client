#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <stb_image.h>
#include <glm/gtc/matrix_transform.hpp>
#include "EntityRenderer.hpp"
#include "models/Entity.hpp"
#include "models/EntityRegistry.hpp"
#include "../misc/ItemRenderer.hpp"
#include "../../inventory/ItemRegistry.hpp"
#include "models/EntityBoxMesh.hpp"
#include "models/EntityJsonModel.hpp"
#include "models/QuadrupedModel.hpp"
#include "models/GolemModel.hpp"
#include "models/PigModel.hpp"
#include "models/CowModel.hpp"
#include "../terrain/models/BlockRegistry.hpp"
#include "../terrain/OcclusionSnapshot.hpp"
#include "../VulkanInit.hpp"
#include "../../Helpers.hpp"
#include "../../Logger.hpp"

namespace Volcano {

namespace {

// Matches VulkanInit's fixed-size per-frame arrays (cameraSets, commandBuffers,
// etc.) — those are sized 3 even though maxFramesInFlight is 2, so we mirror
// that instead of introducing a second, possibly-inconsistent constant.
constexpr uint32_t FRAMES_IN_FLIGHT = 3;

// Local vertex format for every untextured procedural entity mesh — entities
// need their own vertex layout (position + normal) since they don't share
// the chunk mesh's packed-block format at all. An alias, not a separate
// type: EntityBoxMesh.hpp's AppendBox (shared by the cube/quadruped/golem
// builders below) already produces EntityMeshVertex, and this must stay
// layout-compatible with whatever's actually bound to the pipeline's vertex
// input state.
using EntityVertex = EntityMeshVertex;

// std430 layout: 4 vec4s, 16-byte aligned throughout.
struct GPUEntityInstance {
    glm::vec4 positionYaw; // xyz: world position (feet), w: yaw radians.
    glm::vec4 halfExtents; // xyz: half bounding-box extents, w: unused.
    glm::vec4 color;       // rgb: base tint, a: unused.
    glm::vec4 hurtColor;   // rgb: hit-flash color, a: blend amount (0 when not hurt).
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

// Quadruped/golem meshes — drawn through the very same instanced g_pipeline/
// entity.vert as the cube above (see RecordDraw): just a different bound
// vertex/index buffer and indirect-draw slice per shape, not a separate
// pipeline. Humanoid entities are the one shape with a genuinely separate,
// non-instanced draw path (g_humanoidPipeline below) since they need a bound
// skin texture per draw.
AllocatedBuffer g_quadrupedVertexBuffer;
AllocatedBuffer g_quadrupedIndexBuffer;
uint32_t g_quadrupedIndexCount = 0;

AllocatedBuffer g_golemVertexBuffer;
AllocatedBuffer g_golemIndexBuffer;
uint32_t g_golemIndexCount = 0;

// Real vanilla-derived proportions (PigModel.hpp/CowModel.hpp) rather than
// the generic Quadruped box — same instanced path, just two more shapes.
AllocatedBuffer g_pigVertexBuffer;
AllocatedBuffer g_pigIndexBuffer;
uint32_t g_pigIndexCount = 0;

AllocatedBuffer g_cowVertexBuffer;
AllocatedBuffer g_cowIndexBuffer;
uint32_t g_cowIndexCount = 0;

// Slots into the per-frame indirect-command buffer (see CreatePerFrameBuffers/
// RecordDraw) — one VkDrawIndexedIndirectCommand per instanced shape, all
// reading from the same instance SSBO via a firstInstance offset into it
// rather than needing their own descriptor set.
enum EntityShapeSlot : uint32_t {
    SHAPE_CUBE = 0, SHAPE_QUADRUPED = 1, SHAPE_GOLEM = 2, SHAPE_PIG = 3, SHAPE_COW = 4, SHAPE_COUNT = 5
};

// Humanoid (player/zombie/mannequin/skeleton) path — a real UV-mapped
// skeleton mesh (EntityJsonModel), drawn one entity at a time rather than
// batched, since each draw needs its own bound skin texture. Everything else
// keeps using one of the flat-tinted instanced shapes above (cube/quadruped/
// golem). See EntityRenderer::RecordDraw's own comment for why these
// entities are split out of those instance lists rather than merged in.
struct PushConstants {
    glm::vec4 positionYaw;
    glm::vec4 hurtColor; // Same convention as GPUEntityInstance::hurtColor.
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

// One loaded EntityJsonModel mesh (its own vertex/index buffer pair) — a
// separate GPU mesh per model JSON, not one shared buffer, since different
// species now have genuinely different box dimensions (see skeleton.json's
// thinner arms/legs vs humanoid.json) and each model also declares its own
// textureWidth/textureHeight, so UV is baked correctly per mesh at load
// time instead of needing a runtime per-draw correction.
struct EntityModelMesh {
    AllocatedBuffer vertexBuffer;
    AllocatedBuffer indexBuffer;
    uint32_t indexCount = 0;
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
    glm::vec4 hurtColor;
    const SkinTexture* skin;
    const EntityModelMesh* mesh;
};

// Same snapshot-under-the-lock reasoning as HumanoidDraw.
struct BlockDraw {
    glm::vec3 position;
    glm::vec4 hurtColor;
    int32_t stateId;
};

VkPipeline g_humanoidPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_humanoidPipelineLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_textureSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_textureDescriptorPool = VK_NULL_HANDLE;

EntityModelMesh g_humanoidMesh; // player, mannequin, zombie — resources/volcano/models/entity/humanoid.json
EntityModelMesh g_skeletonMesh; // resources/volcano/models/entity/skeleton.json

SkinTexture g_steveSkin; // player, mannequin — this client has no real skin-fetching yet, see LoadSkins' own comment.
SkinTexture g_zombieSkin;
SkinTexture g_skeletonSkin;

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

// The placeholder box — now just a single unit box through AppendBox (see
// EntityBoxMesh.hpp for the shared winding convention every procedural
// entity mesh, this one included, follows).
void BuildCubeMesh(std::vector<EntityVertex>& vertices, std::vector<uint32_t>& indices)
{
    AppendBox(vertices, indices, glm::vec3(0.0f), glm::vec3(1.0f));
}

// Uploads a host-built mesh (vertices/indices already populated by
// BuildCubeMesh/QuadrupedModel::BuildMesh/GolemModel::BuildMesh) into a
// fresh GPU-only vertex/index buffer pair via a staging buffer — shared by
// all three since they're all static, uploaded-once, EntityVertex-typed
// meshes (EntityJsonModel's own upload stays separate below: different vertex
// type, UV included).
void UploadMesh(const std::vector<EntityVertex>& vertices, const std::vector<uint32_t>& indices,
    AllocatedBuffer& outVertexBuffer, AllocatedBuffer& outIndexBuffer, uint32_t& outIndexCount)
{
    outIndexCount = static_cast<uint32_t>(indices.size());

    VkDeviceSize vertexBytes = vertices.size() * sizeof(EntityVertex);
    VkDeviceSize indexBytes = indices.size() * sizeof(uint32_t);

    outVertexBuffer = CreateOwnedBuffer(vertexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    outIndexBuffer = CreateOwnedBuffer(indexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), (size_t)vertexBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, indices.data(), (size_t)indexBytes);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, outVertexBuffer.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, outIndexBuffer.buffer, 1, &indexCopy);

    EndOneShotCommands(cmd);

    vmaDestroyBuffer(vmaAllocator, staging.buffer, staging.allocation);
}

void UploadCubeMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    BuildCubeMesh(vertices, indices);
    UploadMesh(vertices, indices, g_cubeVertexBuffer, g_cubeIndexBuffer, g_cubeIndexCount);
}

void UploadQuadrupedMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    QuadrupedModel::BuildMesh(vertices, indices);
    UploadMesh(vertices, indices, g_quadrupedVertexBuffer, g_quadrupedIndexBuffer, g_quadrupedIndexCount);
}

void UploadGolemMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    GolemModel::BuildMesh(vertices, indices);
    UploadMesh(vertices, indices, g_golemVertexBuffer, g_golemIndexBuffer, g_golemIndexCount);
}

void UploadPigMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    PigModel::BuildMesh(vertices, indices);
    UploadMesh(vertices, indices, g_pigVertexBuffer, g_pigIndexBuffer, g_pigIndexCount);
}

void UploadCowMesh()
{
    std::vector<EntityVertex> vertices;
    std::vector<uint32_t> indices;
    CowModel::BuildMesh(vertices, indices);
    UploadMesh(vertices, indices, g_cowVertexBuffer, g_cowIndexBuffer, g_cowIndexCount);
}

void UploadTexturedMesh(const std::vector<EntityJsonModel::Vertex>& vertices,
    const std::vector<uint32_t>& indices, EntityModelMesh& out);

void UploadJsonMesh(const std::string& modelPath, EntityModelMesh& out)
{
    EntityJsonModel::ModelDef model = EntityJsonModel::Load(modelPath);
    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    EntityJsonModel::BuildMesh(model, vertices, indices);
    UploadTexturedMesh(vertices, indices, out);
}

void UploadTexturedMesh(const std::vector<EntityJsonModel::Vertex>& vertices,
    const std::vector<uint32_t>& indices, EntityModelMesh& out)
{
    out.indexCount = static_cast<uint32_t>(indices.size());

    VkDeviceSize vertexBytes = vertices.size() * sizeof(EntityJsonModel::Vertex);
    VkDeviceSize indexBytes = indices.size() * sizeof(uint32_t);

    out.vertexBuffer = CreateOwnedBuffer(vertexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, false);
    out.indexBuffer = CreateOwnedBuffer(indexBytes,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false);

    AllocatedBuffer staging = CreateOwnedBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, vertices.data(), (size_t)vertexBytes);
    memcpy(static_cast<uint8_t*>(mapped) + vertexBytes, indices.data(), (size_t)indexBytes);
    vmaUnmapMemory(vmaAllocator, staging.allocation);

    VkCommandBuffer cmd = BeginOneShotCommands();

    VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, out.vertexBuffer.buffer, 1, &vertexCopy);

    VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
    vkCmdCopyBuffer(cmd, staging.buffer, out.indexBuffer.buffer, 1, &indexCopy);

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
SkinTexture CreateTextureFromPixels(const uint8_t* pixels, int width, int height, const std::string& path)
{
    SkinTexture skin;
    VkDeviceSize imageBytes = static_cast<VkDeviceSize>(width) * height * 4;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(vmaAllocator, &imageInfo, &allocInfo, &skin.image.image, &skin.image.allocation, nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to allocate skin texture image: " + path);
    }

    AllocatedBuffer staging = CreateOwnedBuffer(imageBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    void* mapped;
    vmaMapMemory(vmaAllocator, staging.allocation, &mapped);
    memcpy(mapped, pixels, static_cast<size_t>(imageBytes));
    vmaUnmapMemory(vmaAllocator, staging.allocation);

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
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
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

SkinTexture LoadSkinTexture(const std::string& path)
{
    int width, height, channels;
    uint8_t* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels)
    {
        throw std::runtime_error("[ERROR] EntityRenderer: failed to load skin texture: " + path);
    }

    try
    {
        SkinTexture skin = CreateTextureFromPixels(pixels, width, height, path);
        stbi_image_free(pixels);
        return skin;
    }
    catch (...)
    {
        stbi_image_free(pixels);
        throw;
    }
}

// This client has no real skin fetching (no Mojang session-server lookup —
// see NetworkClient's own comment on why it doesn't attempt signed chat/
// online-mode auth either), so every player and mannequin uses the same
// bundled default skin. Zombie and skeleton get their own vanilla textures —
// the whole point of this pass is "one shared humanoid shape, texture is
// what tells them apart," per the task this was scoped to.
void LoadSkins()
{
    g_steveSkin = LoadSkinTexture("resources/minecraft/assets/minecraft/textures/entity/player/wide/steve.png");
    g_zombieSkin = LoadSkinTexture("resources/minecraft/assets/minecraft/textures/entity/zombie/zombie.png");
    g_skeletonSkin = LoadSkinTexture("resources/minecraft/assets/minecraft/textures/entity/skeleton/skeleton.png");
}

void DestroySkin(SkinTexture& skin)
{
    if (skin.sampler != VK_NULL_HANDLE) vkDestroySampler(GetDevice(), skin.sampler, nullptr);
    if (skin.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), skin.view, nullptr);
    if (skin.image.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, skin.image.image, skin.image.allocation);
    skin = SkinTexture{};
}

// Which bundled skin a humanoid-shaped entity type uses, or nullptr if
// `typeName` has no matching skin loaded above (EntityRegistry::ClassifyShape
// may classify a type as Humanoid before a skin exists for it yet — see
// RecordDraw's fallback for that case). Kept in sync with ClassifyShape's own
// HUMANOID_NAMES list — see its comment.
const SkinTexture* SkinForEntityType(const std::string& typeName)
{
    if (typeName == "player" || typeName == "mannequin") return &g_steveSkin;
    if (typeName == "zombie") return &g_zombieSkin;
    if (typeName == "skeleton") return &g_skeletonSkin;
    return nullptr;
}

// Which loaded EntityJsonModel mesh a humanoid-shaped entity type uses.
// humanoid.json (player/zombie/mannequin's shared proportions) is the
// default; skeleton.json is its own mesh (thinner arms/legs — see its own
// comment). Kept in sync with SkinForEntityType's own type list above.
const EntityModelMesh* MeshForEntityType(const std::string& typeName)
{
    if (typeName == "skeleton") return &g_skeletonMesh;
    return &g_humanoidMesh;
}

// Block-shaped entities (falling_block, tnt) reuse the humanoid textured
// pipeline: one shared unit-cube mesh, plus a small per-block-state texture
// with the six face tiles side by side (Up, Down, North, South, East, West —
// BlockRegistry's face order). No tinting yet, so grass tops stay gray-green.
constexpr size_t MAX_BLOCK_TEXTURES = 48;
constexpr int BLOCK_TILE_SIZE = 16;

EntityModelMesh g_blockMesh;
std::unordered_map<int32_t, SkinTexture> g_blockTextures; // An entry with a null view means "no usable texture".

void UploadBlockMesh()
{
    struct Face { glm::vec3 normal, right, up; };
    constexpr std::array<Face, 6> faces = {{
        { glm::vec3( 0, 1, 0), glm::vec3( 1, 0, 0), glm::vec3( 0, 0,-1) }, // Up
        { glm::vec3( 0,-1, 0), glm::vec3( 1, 0, 0), glm::vec3( 0, 0, 1) }, // Down
        { glm::vec3( 0, 0,-1), glm::vec3(-1, 0, 0), glm::vec3( 0, 1, 0) }, // North
        { glm::vec3( 0, 0, 1), glm::vec3( 1, 0, 0), glm::vec3( 0, 1, 0) }, // South
        { glm::vec3( 1, 0, 0), glm::vec3( 0, 0,-1), glm::vec3( 0, 1, 0) }, // East
        { glm::vec3(-1, 0, 0), glm::vec3( 0, 0, 1), glm::vec3( 0, 1, 0) }, // West
    }};

    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    const glm::vec3 center(0.0f, 0.5f, 0.0f);
    for (size_t f = 0; f < faces.size(); f++)
    {
        const Face& face = faces[f];
        float u0 = static_cast<float>(f) / 6.0f;
        float u1 = static_cast<float>(f + 1) / 6.0f;
        glm::vec3 faceCenter = center + face.normal * 0.5f;

        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back({ faceCenter - 0.5f * face.right - 0.5f * face.up, face.normal, { u0, 1.0f } });
        vertices.push_back({ faceCenter + 0.5f * face.right - 0.5f * face.up, face.normal, { u1, 1.0f } });
        vertices.push_back({ faceCenter + 0.5f * face.right + 0.5f * face.up, face.normal, { u1, 0.0f } });
        vertices.push_back({ faceCenter - 0.5f * face.right + 0.5f * face.up, face.normal, { u0, 0.0f } });
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    UploadTexturedMesh(vertices, indices, g_blockMesh);
}

// Texture name for one face of a block, falling back to non-cube visuals
// (slabs, glass, plants) so they at least show something cube-shaped.
std::string BlockFaceTextureName(const Block& block, int face)
{
    if (block.visualId != 0) return BlockRegistry::GetFaceTextureName(block.visualId, face);

    const BlockRegistry::NonCubeVisual& visual = BlockRegistry::GetNonCubeVisual(block.nonCubeVisualId);
    if (!visual.crossTexture.empty()) return visual.crossTexture;
    for (const auto& element : visual.elements)
    {
        if (!element.faceTextures[static_cast<size_t>(face)].empty()) return element.faceTextures[static_cast<size_t>(face)];
    }
    for (const auto& element : visual.elements)
    {
        for (const std::string& name : element.faceTextures)
        {
            if (!name.empty()) return name;
        }
    }
    return {};
}

// Builds (once per block state) and returns the face-strip texture, or
// nullptr if the state has nothing drawable or the cache is full.
const SkinTexture* BlockTextureFor(int32_t stateId)
{
    if (auto it = g_blockTextures.find(stateId); it != g_blockTextures.end())
    {
        return it->second.view != VK_NULL_HANDLE ? &it->second : nullptr;
    }
    if (g_blockTextures.size() >= MAX_BLOCK_TEXTURES) return nullptr;

    Block block = BlockRegistry::ResolveStateId(stateId);
    constexpr int stripWidth = BLOCK_TILE_SIZE * 6;
    std::vector<uint8_t> strip(static_cast<size_t>(stripWidth) * BLOCK_TILE_SIZE * 4, 0);

    bool anyFace = false;
    for (int face = 0; face < 6; face++)
    {
        std::string name = BlockFaceTextureName(block, face);
        if (name.empty()) continue;

        int width, height, channels;
        std::string path = "resources/minecraft/assets/minecraft/textures/block/" + name + ".png";
        uint8_t* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels) continue;

        // Animated textures are tall strips; the first square frame is enough.
        for (int y = 0; y < std::min(height, BLOCK_TILE_SIZE); y++)
        {
            for (int x = 0; x < std::min(width, BLOCK_TILE_SIZE); x++)
            {
                memcpy(&strip[(static_cast<size_t>(y) * stripWidth + face * BLOCK_TILE_SIZE + x) * 4],
                    &pixels[(static_cast<size_t>(y) * width + x) * 4], 4);
            }
        }
        stbi_image_free(pixels);
        anyFace = true;
    }

    SkinTexture texture{};
    if (anyFace)
    {
        texture = CreateTextureFromPixels(strip.data(), stripWidth, BLOCK_TILE_SIZE,
            "block state " + std::to_string(stateId));
    }
    auto inserted = g_blockTextures.emplace(stateId, texture).first;
    return inserted->second.view != VK_NULL_HANDLE ? &inserted->second : nullptr;
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

    // One descriptor per distinct skin, plus one per cached block-entity
    // texture (see MAX_BLOCK_TEXTURES) — cheap to over-provision.
    constexpr uint32_t MAX_SKIN_TEXTURES = 8 + static_cast<uint32_t>(MAX_BLOCK_TEXTURES);
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

        // One command per instanced shape (see EntityShapeSlot), indexed by
        // slot rather than one buffer per shape.
        g_indirectBuffers[i] = CreateOwnedBuffer(SHAPE_COUNT * sizeof(VkDrawIndexedIndirectCommand),
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        vmaMapMemory(vmaAllocator, g_indirectBuffers[i].allocation, &g_indirectBuffersMapped[i]);

        VkDrawIndexedIndirectCommand initialCommands[SHAPE_COUNT]{};
        initialCommands[SHAPE_CUBE].indexCount = g_cubeIndexCount;
        initialCommands[SHAPE_QUADRUPED].indexCount = g_quadrupedIndexCount;
        initialCommands[SHAPE_GOLEM].indexCount = g_golemIndexCount;
        initialCommands[SHAPE_PIG].indexCount = g_pigIndexCount;
        initialCommands[SHAPE_COW].indexCount = g_cowIndexCount;
        memcpy(g_indirectBuffersMapped[i], initialCommands, sizeof(initialCommands));

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

// Loads a vertex/fragment pair through the ShaderLibrary (shader pack
// override, else built-in), releasing the first module if the second fails.
void CreateShaderPair(const Engine::ShaderLibrary& shaders, const std::string& baseName,
    VkShaderModule& vertModule, VkShaderModule& fragModule)
{
    vertModule = CreateShaderModule(shaders, baseName + ".vert");
    try
    {
        fragModule = CreateShaderModule(shaders, baseName + ".frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
        vertModule = VK_NULL_HANDLE;
        throw;
    }
}

void DestroyShaderPair(VkShaderModule vertModule, VkShaderModule fragModule)
{
    vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
    vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
}

void CreatePipeline(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    CreateShaderPair(shaders, "entity", vertModule, fragModule);

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

    // AppendBox's winding (u x v == normal, corners (-u,-v)->(+u,-v)->
    // (+u,+v)->(-u,+v)) comes out CCW seen from outside, so front face is
    // counter-clockwise here — see EntityBoxMesh.hpp's own comment.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

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
        DestroyShaderPair(vertModule, fragModule);
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
        g_pipeline = VK_NULL_HANDLE;
        DestroyShaderPair(vertModule, fragModule);
        throw std::runtime_error("[ERROR] Failed to create entity graphics pipeline.");
    }

    DestroyShaderPair(vertModule, fragModule);
}

// Same fixed-function state as CreatePipeline() above (same render pass/
// subpass, same depth test since humanoids share the world's depth
// attachment too, same winding/cull convention — EntityJsonModel::BuildMesh
// follows BuildCubeMesh's own winding rule deliberately), differing only in
// vertex layout (+UV, no per-instance SSBO), shaders, and descriptor set 1
// (a skin texture instead of the instance buffer) plus a push constant
// range (this path draws one entity per call, not instanced).
void CreateHumanoidPipeline(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = VK_NULL_HANDLE;
    VkShaderModule fragModule = VK_NULL_HANDLE;
    CreateShaderPair(shaders, "entity_textured", vertModule, fragModule);

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
    binding.stride = sizeof(EntityJsonModel::Vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0].binding = 0;
    attributes[0].location = 0;
    attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[0].offset = offsetof(EntityJsonModel::Vertex, position);
    attributes[1].binding = 0;
    attributes[1].location = 1;
    attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributes[1].offset = offsetof(EntityJsonModel::Vertex, normal);
    attributes[2].binding = 0;
    attributes[2].location = 2;
    attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[2].offset = offsetof(EntityJsonModel::Vertex, uv);

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

    // Same winding/cull convention as CreatePipeline() above — EntityJsonModel::
    // BuildMesh follows AppendBox's rule deliberately (see its own comment).
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

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
        DestroyShaderPair(vertModule, fragModule);
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
        g_humanoidPipeline = VK_NULL_HANDLE;
        DestroyShaderPair(vertModule, fragModule);
        throw std::runtime_error("[ERROR] Failed to create humanoid entity graphics pipeline.");
    }

    DestroyShaderPair(vertModule, fragModule);
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

// Both pipelines and their layouts — everything shader-dependent, and
// nothing else (meshes, skins, per-frame buffers stay put).
void DestroyPipelines()
{
    VkDevice dev = GetDevice();
    if (g_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, g_pipeline, nullptr);
    if (g_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, g_pipelineLayout, nullptr);
    if (g_humanoidPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, g_humanoidPipeline, nullptr);
    if (g_humanoidPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, g_humanoidPipelineLayout, nullptr);
    g_pipeline = VK_NULL_HANDLE;
    g_pipelineLayout = VK_NULL_HANDLE;
    g_humanoidPipeline = VK_NULL_HANDLE;
    g_humanoidPipelineLayout = VK_NULL_HANDLE;
}

} // namespace

void EntityRenderer::Init(const Engine::ShaderLibrary& shaders)
{
    // All must run before CreatePerFrameBuffers() — it seeds each shape's
    // initial indirect-command indexCount from these.
    UploadCubeMesh();
    UploadQuadrupedMesh();
    UploadGolemMesh();
    UploadPigMesh();
    UploadCowMesh();
    CreateDescriptorLayoutAndPool();
    CreatePerFrameBuffers();
    CreatePipeline(shaders);

    // Humanoid path — CreateDescriptorLayoutAndPool() above must run first
    // (LoadSkins() allocates each skin's descriptor set from
    // g_textureDescriptorPool against g_textureSetLayout, both created
    // there).
    UploadJsonMesh("resources/volcano/models/entity/humanoid.json", g_humanoidMesh);
    UploadJsonMesh("resources/volcano/models/entity/skeleton.json", g_skeletonMesh);
    UploadBlockMesh();
    LoadSkins();
    CreateHumanoidPipeline(shaders);

    Log::Info("Entity renderer initialized.");
}

void EntityRenderer::ReloadPipelines(const Engine::ShaderLibrary& shaders)
{
    DestroyPipelines();
    CreatePipeline(shaders);
    CreateHumanoidPipeline(shaders);
}

VkDescriptorSet EntityRenderer::GetDefaultSkinSet()
{
    return g_steveSkin.descriptorSet;
}

VkDescriptorSetLayout EntityRenderer::GetSkinSetLayout()
{
    return g_textureSetLayout;
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

    if (g_quadrupedVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_quadrupedVertexBuffer.buffer, g_quadrupedVertexBuffer.allocation);
        g_quadrupedVertexBuffer = AllocatedBuffer{};
    }
    if (g_quadrupedIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_quadrupedIndexBuffer.buffer, g_quadrupedIndexBuffer.allocation);
        g_quadrupedIndexBuffer = AllocatedBuffer{};
    }
    if (g_golemVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_golemVertexBuffer.buffer, g_golemVertexBuffer.allocation);
        g_golemVertexBuffer = AllocatedBuffer{};
    }
    if (g_golemIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_golemIndexBuffer.buffer, g_golemIndexBuffer.allocation);
        g_golemIndexBuffer = AllocatedBuffer{};
    }
    if (g_pigVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_pigVertexBuffer.buffer, g_pigVertexBuffer.allocation);
        g_pigVertexBuffer = AllocatedBuffer{};
    }
    if (g_pigIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_pigIndexBuffer.buffer, g_pigIndexBuffer.allocation);
        g_pigIndexBuffer = AllocatedBuffer{};
    }
    if (g_cowVertexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_cowVertexBuffer.buffer, g_cowVertexBuffer.allocation);
        g_cowVertexBuffer = AllocatedBuffer{};
    }
    if (g_cowIndexBuffer.buffer != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(vmaAllocator, g_cowIndexBuffer.buffer, g_cowIndexBuffer.allocation);
        g_cowIndexBuffer = AllocatedBuffer{};
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
    DestroySkin(g_skeletonSkin);
    for (auto& [stateId, texture] : g_blockTextures) DestroySkin(texture);
    g_blockTextures.clear();

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

    for (EntityModelMesh* mesh : { &g_humanoidMesh, &g_skeletonMesh, &g_blockMesh })
    {
        if (mesh->vertexBuffer.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, mesh->vertexBuffer.buffer, mesh->vertexBuffer.allocation);
            mesh->vertexBuffer = AllocatedBuffer{};
        }
        if (mesh->indexBuffer.buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(vmaAllocator, mesh->indexBuffer.buffer, mesh->indexBuffer.allocation);
            mesh->indexBuffer = AllocatedBuffer{};
        }
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

    // Read per frame so a settings reload applies immediately. All four
    // channels are 0-255; alpha is how strongly the color replaces the model.
    // TODO: Graphics.EntityHurt.IncludeArmor — armor isn't rendered yet.
    auto hurtChannel = [state](const char* channel, uint32_t fallback) {
        std::string key = std::string("Graphics.EntityHurt.Color.") + channel;
        return static_cast<float>(std::get<uint32_t>(state->config->Get(key, uint32_t{fallback}))) / 255.0f;
    };
    glm::vec4 hurtColor(hurtChannel("Red", 255), hurtChannel("Green", 0), hurtChannel("Blue", 0), hurtChannel("Alpha", 64));
    auto HurtColorFor = [&](const Entity& entity) {
        return entity.IsHurt(now) ? hurtColor : glm::vec4(hurtColor.r, hurtColor.g, hurtColor.b, 0.0f);
    };

    std::vector<std::pair<float, const Entity*>> visible;
    // Humanoid-shaped entities (player/zombie/mannequin/skeleton) are pulled
    // out of the instanced path entirely and drawn separately below via
    // EntityJsonModel — they need a real UV-mapped mesh and their own bound
    // skin texture, neither of which the flat-tinted instance SSBO (shared
    // mesh per shape, no per-instance texture) has any way to carry.
    // Distance-sorting/MAX_VISIBLE_ENTITIES still applies to them via the
    // same `visible` pass below; they're just routed to a different draw
    // call afterward instead of into one of the shape instance lists. Holds
    // resolved values rather than Entity pointers — see HumanoidDraw's own
    // comment for why that distinction is load-bearing here.
    std::vector<HumanoidDraw> humanoidVisible;
    std::vector<BlockDraw> blockVisible;
    uint32_t cubeInstanceCount = 0;
    uint32_t quadrupedInstanceCount = 0;
    uint32_t golemInstanceCount = 0;
    uint32_t pigInstanceCount = 0;
    uint32_t cowInstanceCount = 0;
    uint32_t quadrupedFirstInstance = 0;
    uint32_t golemFirstInstance = 0;
    uint32_t pigFirstInstance = 0;
    uint32_t cowFirstInstance = 0;
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        visible.reserve(state->entities.size() + state->playerEntities.size());

        // Other players (playerEntities) render exactly like every other
        // humanoid — the invisibility status effect's flag aside, which
        // hides them the way it does in vanilla.
        state->ForEachEntity([&](uint32_t id, const Entity& entity)
        {
            if (!entity.visible) return;
            const PlayerEntity* player = state->FindPlayerEntity(id);
            if (player && player->invisible) return;

            // Dropped items with a 3D model are drawn by ItemPass instead.
            if (state->itemRenderer != nullptr && entity.itemId != 0)
            {
                const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(entity.itemId);
                if (item != nullptr && state->itemRenderer->HasModel(item->name)) return;
            }

            float halfWidth = entity.boundingBox.x * 0.5f;
            float halfHeight = entity.boundingBox.y * 0.5f;
            glm::vec3 center = entity.InterpolatedPosition(now) + glm::vec3(0.0f, halfHeight, 0.0f);
            float radius = glm::length(glm::vec3(halfWidth, halfHeight, halfWidth));

            if (!SphereInFrustum(frustumPlanes, center, radius)) return;

            // Hi-Z: skip non-players hidden behind terrain. Players stay
            // drawn so they can be seen through walls.
            if (player == nullptr
                && occlusionSnapshot.IsEntityOccluded(entity.InterpolatedPosition(now), entity.boundingBox.x, entity.boundingBox.y))
            {
                return;
            }

            float distanceSq = glm::dot(center - cameraPosition, center - cameraPosition);
            visible.emplace_back(distanceSq, &entity);
        });

        if (visible.size() > MAX_VISIBLE_ENTITIES)
        {
            std::partial_sort(visible.begin(), visible.begin() + MAX_VISIBLE_ENTITIES, visible.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            visible.resize(MAX_VISIBLE_ENTITIES);
        }

        // Build the GPU instance buffer while still holding the lock, since
        // `visible` (and humanoidVisible, built alongside it here) holds raw
        // pointers into state->entities/playerEntities. Split by
        // EntityRegistry::ShapeKind rather than one flat list — each shape
        // draws off its own mesh, so its instances need to land in their own
        // contiguous slice of the shared instance SSBO (see the
        // concatenation below).
        std::vector<GPUEntityInstance> cubeInstances;
        std::vector<GPUEntityInstance> quadrupedInstances;
        std::vector<GPUEntityInstance> golemInstances;
        std::vector<GPUEntityInstance> pigInstances;
        std::vector<GPUEntityInstance> cowInstances;
        cubeInstances.reserve(visible.size());
        for (const auto& [distanceSq, entity] : visible)
        {
            const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity->type);
            EntityRegistry::ShapeKind shape = info ? info->shape : EntityRegistry::ShapeKind::Cube;

            if (shape == EntityRegistry::ShapeKind::Humanoid)
            {
                const SkinTexture* skin = SkinForEntityType(info->name);
                if (skin != nullptr)
                {
                    humanoidVisible.push_back(HumanoidDraw{
                        entity->InterpolatedPosition(now), entity->InterpolatedYaw(now), HurtColorFor(*entity),
                        skin, MeshForEntityType(info->name) });
                    continue;
                }
                // Classified Humanoid but no bundled skin loaded for it yet
                // (see SkinForEntityType) — fall back to the flat cube
                // rather than silently dropping the entity.
                shape = EntityRegistry::ShapeKind::Cube;
            }

            if (shape == EntityRegistry::ShapeKind::Block && entity->blockStateId >= 0)
            {
                blockVisible.push_back(BlockDraw{
                    entity->InterpolatedPosition(now), HurtColorFor(*entity), entity->blockStateId });
                continue;
            }

            GPUEntityInstance gpu{};
            gpu.positionYaw = glm::vec4(entity->InterpolatedPosition(now), entity->InterpolatedYaw(now));
            gpu.halfExtents = glm::vec4(
                entity->boundingBox.x * 0.5f, entity->boundingBox.y * 0.5f, entity->boundingBox.x * 0.5f, 0.0f);
            gpu.color = glm::vec4(entity->color, 1.0f);
            gpu.hurtColor = HurtColorFor(*entity);

            switch (shape)
            {
                case EntityRegistry::ShapeKind::Quadruped: quadrupedInstances.push_back(gpu); break;
                case EntityRegistry::ShapeKind::Golem: golemInstances.push_back(gpu); break;
                case EntityRegistry::ShapeKind::Pig: pigInstances.push_back(gpu); break;
                case EntityRegistry::ShapeKind::Cow: cowInstances.push_back(gpu); break;
                default: cubeInstances.push_back(gpu); break;
            }
        }

        // One flat buffer, partitioned cube/quadruped/golem/pig/cow back to
        // back. Each shape's indirect command below points at its own slice
        // via firstInstance, which Vulkan folds into gl_InstanceIndex for us
        // — entity.vert doesn't need to know shapes were split up at all.
        quadrupedFirstInstance = static_cast<uint32_t>(cubeInstances.size());
        golemFirstInstance = quadrupedFirstInstance + static_cast<uint32_t>(quadrupedInstances.size());
        pigFirstInstance = golemFirstInstance + static_cast<uint32_t>(golemInstances.size());
        cowFirstInstance = pigFirstInstance + static_cast<uint32_t>(pigInstances.size());
        uint32_t totalInstances = cowFirstInstance + static_cast<uint32_t>(cowInstances.size());

        if (totalInstances > 0)
        {
            auto* dest = static_cast<GPUEntityInstance*>(g_instanceBuffersMapped[frameIndex]);
            memcpy(dest, cubeInstances.data(), cubeInstances.size() * sizeof(GPUEntityInstance));
            memcpy(dest + quadrupedFirstInstance, quadrupedInstances.data(),
                quadrupedInstances.size() * sizeof(GPUEntityInstance));
            memcpy(dest + golemFirstInstance, golemInstances.data(), golemInstances.size() * sizeof(GPUEntityInstance));
            memcpy(dest + pigFirstInstance, pigInstances.data(), pigInstances.size() * sizeof(GPUEntityInstance));
            memcpy(dest + cowFirstInstance, cowInstances.data(), cowInstances.size() * sizeof(GPUEntityInstance));
        }

        cubeInstanceCount = static_cast<uint32_t>(cubeInstances.size());
        quadrupedInstanceCount = static_cast<uint32_t>(quadrupedInstances.size());
        golemInstanceCount = static_cast<uint32_t>(golemInstances.size());
        pigInstanceCount = static_cast<uint32_t>(pigInstances.size());
        cowInstanceCount = static_cast<uint32_t>(cowInstances.size());
    }

    VkDrawIndexedIndirectCommand commands[SHAPE_COUNT]{};
    commands[SHAPE_CUBE] = { g_cubeIndexCount, cubeInstanceCount, 0, 0, 0 };
    commands[SHAPE_QUADRUPED] = { g_quadrupedIndexCount, quadrupedInstanceCount, 0, 0, quadrupedFirstInstance };
    commands[SHAPE_GOLEM] = { g_golemIndexCount, golemInstanceCount, 0, 0, golemFirstInstance };
    commands[SHAPE_PIG] = { g_pigIndexCount, pigInstanceCount, 0, 0, pigFirstInstance };
    commands[SHAPE_COW] = { g_cowIndexCount, cowInstanceCount, 0, 0, cowFirstInstance };
    memcpy(g_indirectBuffersMapped[frameIndex], commands, sizeof(commands));

    if (cubeInstanceCount + quadrupedInstanceCount + golemInstanceCount + pigInstanceCount + cowInstanceCount > 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);

        VkDescriptorSet sets[] = { cameraSets[frameIndex], g_instanceSets[frameIndex] };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 2, sets, 0, nullptr);

        // Each shape is its own draw (different vertex/index buffer), but
        // still one indirect draw per shape rather than one per entity —
        // the batching this pipeline exists for.
        VkDeviceSize vertexOffset = 0;
        if (cubeInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_cubeVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_CUBE * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            state->trianglesDrawn += (g_cubeIndexCount / 3) * cubeInstanceCount;
        }
        if (quadrupedInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_quadrupedVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_quadrupedIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_QUADRUPED * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            state->trianglesDrawn += (g_quadrupedIndexCount / 3) * quadrupedInstanceCount;
        }
        if (golemInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_golemVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_golemIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_GOLEM * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            state->trianglesDrawn += (g_golemIndexCount / 3) * golemInstanceCount;
        }
        if (pigInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_pigVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_pigIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_PIG * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            state->trianglesDrawn += (g_pigIndexCount / 3) * pigInstanceCount;
        }
        if (cowInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_cowVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_cowIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_COW * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            state->trianglesDrawn += (g_cowIndexCount / 3) * cowInstanceCount;
        }
    }

    // Humanoid path: one draw per entity (not instanced/indirect — the
    // batched approach above exists for potentially hundreds of flat-tinted
    // cubes; a handful of textured players/zombies/mannequins doesn't need
    // that complexity, and each needs its own bound skin texture anyway).
    // Static bind pose only — position/yaw come straight from the same
    // InterpolatedPosition/InterpolatedYaw every other entity uses, sampled
    // fresh every frame right here on the render thread. See
    // EntityJsonModel.hpp's own comment on where a future animated pose
    // should be sampled from (here, not TickLoop).
    if (!humanoidVisible.empty() || !blockVisible.empty())
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_humanoidPipeline);

        // Block entities: the shared cube mesh with each block state's own
        // face-strip texture.
        for (const BlockDraw& draw : blockVisible)
        {
            const SkinTexture* texture = BlockTextureFor(draw.stateId);
            if (texture == nullptr) continue;

            VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_blockMesh.vertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_blockMesh.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

            VkDescriptorSet sets[] = { cameraSets[frameIndex], texture->descriptorSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_humanoidPipelineLayout, 0, 2, sets, 0, nullptr);

            PushConstants pc{};
            pc.positionYaw = glm::vec4(draw.position, 0.0f);
            pc.hurtColor = draw.hurtColor;
            vkCmdPushConstants(cmd, g_humanoidPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd, g_blockMesh.indexCount, 1, 0, 0, 0);
            state->trianglesDrawn += g_blockMesh.indexCount / 3;
        }

        for (const HumanoidDraw& draw : humanoidVisible)
        {
            // Vertex/index buffer bound per draw, not once before the loop —
            // unlike the old single shared HumanoidModel mesh, different
            // species can now have genuinely different meshes (skeleton.json
            // vs humanoid.json) mixed within the same visible list.
            VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &draw.mesh->vertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, draw.mesh->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

            VkDescriptorSet sets[] = { cameraSets[frameIndex], draw.skin->descriptorSet };
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_humanoidPipelineLayout, 0, 2, sets, 0, nullptr);

            PushConstants pc{};
            pc.positionYaw = glm::vec4(draw.position, draw.yaw);
            pc.hurtColor = draw.hurtColor;
            vkCmdPushConstants(cmd, g_humanoidPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);

            vkCmdDrawIndexed(cmd, draw.mesh->indexCount, 1, 0, 0, 0);
            state->trianglesDrawn += draw.mesh->indexCount / 3;
        }
    }
}

} // namespace Volcano
