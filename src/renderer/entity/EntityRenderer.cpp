#include <array>
#include <vector>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <fstream>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include "../../resources/ImageLoad.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include "EntityRenderer.hpp"
#include "models/Entity.hpp"
#include "models/EntityRegistry.hpp"
#include "models/EntitySeat.hpp"
#include "models/BodyRotation.hpp"
#include "models/EntityAnimation.hpp"
#include "../misc/ItemRenderer.hpp"
#include "../engine/vulkan/VulkanRenderEngine.hpp"
#include "../../inventory/ItemRegistry.hpp"
#include "models/EntityBoxMesh.hpp"
#include "models/EntityJsonModel.hpp"
#include "models/QuadrupedModel.hpp"
#include "models/GolemModel.hpp"
#include "models/PigModel.hpp"
#include "models/CowModel.hpp"
#include "../terrain/models/BlockRegistry.hpp"
#include "../terrain/models/World.hpp"
#include "../terrain/OcclusionSnapshot.hpp"
#include "../VulkanInit.hpp"
#include "../../Helpers.hpp"
#include "../../TickLoop.hpp"
#include "../../Logger.hpp"

namespace Volcano {

namespace {

// Matches VulkanInit's fixed-size per-frame arrays (cameraSets, commandBuffers,
// etc.) - those are sized 3 even though maxFramesInFlight is 2, so we mirror
// that instead of introducing a second, possibly-inconsistent constant.
constexpr uint32_t FRAMES_IN_FLIGHT = 3;

// Local vertex format for every untextured procedural entity mesh - entities
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
// The flat-shaded pipeline built from entity_shadow.* (shadow receiving) and
// its layout, which adds the shadow map as set 2. Null when unavailable.
VkPipeline g_shadowedPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_shadowedPipelineLayout = VK_NULL_HANDLE;
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

// Quadruped/golem meshes - drawn through the very same instanced g_pipeline/
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
// the generic Quadruped box - same instanced path, just two more shapes.
AllocatedBuffer g_pigVertexBuffer;
AllocatedBuffer g_pigIndexBuffer;
uint32_t g_pigIndexCount = 0;

AllocatedBuffer g_cowVertexBuffer;
AllocatedBuffer g_cowIndexBuffer;
uint32_t g_cowIndexCount = 0;

// Slots into the per-frame indirect-command buffer (see CreatePerFrameBuffers/
// RecordDraw) - one VkDrawIndexedIndirectCommand per instanced shape, all
// reading from the same instance SSBO via a firstInstance offset into it
// rather than needing their own descriptor set.
enum EntityShapeSlot : uint32_t {
    SHAPE_CUBE = 0, SHAPE_QUADRUPED = 1, SHAPE_GOLEM = 2, SHAPE_PIG = 3, SHAPE_COW = 4, SHAPE_COUNT = 5
};

// Textured path (players, mobs from mobs.json, block-state entities, modeled
// block entities) - real UV-mapped meshes (EntityJsonModel), each draw needing
// its own bound skin texture. Everything else keeps using one of the flat-
// tinted instanced shapes above (cube/quadruped/golem). See
// EntityRenderer::RecordDraw's own comment for why these entities are split
// out of those instance lists rather than merged in.
//
// One record per drawn part: entity_textured.vert reads it as instance-rate
// vertex attributes (binding 1), so a part, an entity or a hundred of them
// are all the same kind of draw. See TexturedBatch.
struct TexturedInstance {
    glm::vec4 positionYaw;
    glm::vec4 hurtColor; // Same convention as GPUEntityInstance::hurtColor.
    glm::vec4 partPivot;    // xyz: point the part turns about, model-local; w: tint as 0xRRGGBB (0 for none).
    glm::vec4 partOffset;   // xyz: translation added after the turn, model-local; w: light, sky * 16 + block.
    glm::vec4 partRotation; // xyz: radians, applied Z * Y * X.
};

// One loaded skin - its own image/view/sampler/descriptor set, registered
// with g_textureSetLayout's single combined-image-sampler binding. Not
// registered in VulkanInit's global allocatedImages list, for the same
// double-free reason CreateOwnedBuffer's own comment gives for buffers.
struct SkinTexture {
    AllocatedImage image{};
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
};

// One loaded EntityJsonModel mesh (its own vertex/index buffer pair) - a
// separate GPU mesh per model JSON, not one shared buffer, since different
// species now have genuinely different box dimensions (see skeleton.json's
// thinner arms/legs vs humanoid.json) and each model also declares its own
// textureWidth/textureHeight, so UV is baked correctly per mesh at load
// time instead of needing a runtime per-draw correction.
struct EntityModelMesh {
    AllocatedBuffer vertexBuffer;
    AllocatedBuffer indexBuffer;
    uint32_t indexCount = 0;
    float scale = 1.0f;
    float feetHeight = 1.5f; // Ground level in the model frame, in blocks.
    std::vector<EntityJsonModel::PartInfo> parts; // Empty for meshes that don't animate.
    EntityAnimation::Roles roles;
};

// One humanoid entity's fully-resolved draw parameters, snapshotted under
// entitiesMutex. Deliberately NOT a `const Entity*`: unlike the cube path
// (which finishes writing its instance buffer before the lock drops), the
// humanoid draw loop runs after RecordDraw releases entitiesMutex, and the
// network thread can erase an entity from state->entities in that window -
// so a pointer into the map would be a use-after-free by the time it's
// dereferenced. The skin pointer is fine to keep: it aims at a file-scope
// global that outlives every frame.
struct MobLayer {
    const EntityModelMesh* mesh;
    const SkinTexture* skin;
};

// One textured mob from mobs.json: base layer first, then overlays.
struct MobModel {
    std::vector<MobLayer> layers;
    // Armor meshes fitted to this body (same parts, same pivots), or null for a mob that wears none.
    const EntityModelMesh* armorOuter = nullptr; // Helmet, chestplate, boots.
    const EntityModelMesh* armorInner = nullptr; // Leggings.
};

struct HumanoidDraw {
    glm::vec3 position;
    float yaw;
    glm::vec4 hurtColor;
    const MobModel* model;
    EntityAnimation::Inputs anim;
    const std::string* mainhandItem = nullptr; // Registry names, which outlive the frame.
    const std::string* offhandItem = nullptr;
    std::array<const std::string*, 4> armor{}; // Head, chest, legs, feet.
    bool hideHead = false; // First-person body view: the camera sits inside the head.
};

// Walk cycles per entity, keyed by network id; the local player uses LOCAL_WALK_KEY.
constexpr uint32_t LOCAL_WALK_KEY = 0xFFFFFFFFu;
std::unordered_map<uint32_t, EntityAnimation::WalkState> g_walkStates;
uint64_t g_walkFrame = 0;

// Vanilla's (rotation, offset) in model space -> this renderer's world-local
// space: the model's Y-down, Z-flipped frame is a half turn about X.
glm::vec3 ToWorldRotation(glm::vec3 vanilla) { return glm::vec3(vanilla.x, -vanilla.y, -vanilla.z); }
glm::vec3 ToWorldOffset(glm::vec3 vanilla, float scale) { return glm::vec3(vanilla.x, -vanilla.y, -vanilla.z) * (scale / 16.0f); }

// Item registry name of a stack, or null for an empty hand.
const std::string* HeldItemName(const ItemStack& stack)
{
    if (stack.IsEmpty()) return nullptr;
    const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(stack.itemId);
    return item != nullptr ? &item->name : nullptr;
}

// What one worn armor piece draws: its sheet, and for leather its dye and undyed overlay.
struct ArmorLook {
    std::string texture;
    std::string overlayTexture;
    uint32_t tint = 0;
};

// Whether an item name is armor for a slot (0 head, 1 chest, 2 legs, 3 feet), and how it looks.
bool ArmorLookOf(const std::string& itemName, int slot, ArmorLook& look)
{
    static const char* const SUFFIX[4] = { "_helmet", "_chestplate", "_leggings", "_boots" };
    size_t colon = itemName.find(':');
    std::string name = itemName.substr(colon == std::string::npos ? 0 : colon + 1);
    const std::string suffix = SUFFIX[slot];
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) return false;

    std::string material = name.substr(0, name.size() - suffix.size());
    if (material == "golden") material = "gold";
    else if (material == "turtle") material = "turtle_scute";
    const std::string sheet = slot == 2 ? "equipment/humanoid_leggings/" : "equipment/humanoid/";
    look.texture = sheet + material + ".png";
    if (material == "leather")
    {
        look.overlayTexture = sheet + "leather_overlay.png";
        look.tint = 0xA06540; // Undyed: the dye color is a data component this client doesn't read.
    }
    return true;
}

// An armor sheet, or null when the packs have none for that material.
const SkinTexture* ArmorSkin(const std::string& path);

// Parts of the armor mesh each slot covers, by index (the models share humanoid.json's order).
constexpr std::array<std::array<int, 3>, 4> ARMOR_PARTS = {{ { 0, -1, -1 }, { 1, 2, 3 }, { 1, 4, 5 }, { 4, 5, -1 } }};

// The local player's body turn, advanced while they're drawn in third person.
BodyRotation g_localBody;
std::chrono::steady_clock::time_point g_localBodyTime = std::chrono::steady_clock::now();

// The local player's pose as last posed by RecordDraw, for the shadow pass
// (which records first in the frame). Valid only while they exist in the world.
HumanoidDraw g_localShadow{};
bool g_localShadowValid = false;

// Whether an entity sits on a vehicle, and whether that vehicle is a boat.
struct RideInfo {
    bool riding = false;
    bool boat = false;
};

// Whether an entity sits on a tracked vehicle, and the seat it renders at.
// Call under entitiesMutex.
RideInfo RideInfoFor(GlobalState* state, const Entity& entity)
{
    glm::vec3 seat;
    float vehicleYaw = 0.0f;
    EntitySeat::Kind kind = EntitySeat::Resolve(state, entity, std::chrono::steady_clock::now(), seat, vehicleYaw);
    return RideInfo{ kind != EntitySeat::Kind::None, kind == EntitySeat::Kind::Boat };
}

// Where a remote entity renders: its seat when riding, else its interpolated position.
glm::vec3 RenderPositionOf(GlobalState* state, const Entity& entity, std::chrono::steady_clock::time_point now)
{
    glm::vec3 seat;
    float vehicleYaw = 0.0f;
    if (EntitySeat::Resolve(state, entity, now, seat, vehicleYaw) != EntitySeat::Kind::None) return seat;
    return entity.InterpolatedPosition(now);
}

// The chunk light at a point on an entity, packed sky * 16 + block for TexturedInstance.
float EntityLight(GlobalState* state, const glm::vec3& feet)
{
    if (state->world == nullptr) return 15.0f * 16.0f;
    LightSample sample = state->world->GetLightSample(static_cast<int>(std::floor(feet.x)),
        static_cast<int>(std::floor(feet.y + 1.0f)), static_cast<int>(std::floor(feet.z)));
    return static_cast<float>(sample.sky) * 16.0f + static_cast<float>(sample.block);
}

// The local player's vehicle kind, from the vehicle they ride.
RideInfo LocalRideInfo(GlobalState* state)
{
    int32_t vehicleId = state->ridingEntityId.load();
    if (vehicleId < 0) return RideInfo{};
    std::lock_guard<std::mutex> lock(state->entitiesMutex);
    const Entity* vehicle = state->FindEntity(static_cast<uint32_t>(vehicleId));
    if (vehicle == nullptr) return RideInfo{ true, false };
    return RideInfo{ true, EntitySeat::KindOf(*vehicle) == EntitySeat::Kind::Boat };
}

// Same snapshot-under-the-lock reasoning as HumanoidDraw.
struct BlockDraw {
    glm::vec3 position;
    glm::vec4 hurtColor;
    int32_t stateId;
};

VkPipeline g_humanoidPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_humanoidPipelineLayout = VK_NULL_HANDLE;
// The same pipeline built from entity_textured_shadow.* (shadow receiving) and
// its layout, which adds the shadow map as set 2. Null when unavailable.
VkPipeline g_humanoidShadowedPipeline = VK_NULL_HANDLE;
VkPipelineLayout g_humanoidShadowedLayout = VK_NULL_HANDLE;
VkDescriptorSetLayout g_textureSetLayout = VK_NULL_HANDLE;
VkDescriptorPool g_textureDescriptorPool = VK_NULL_HANDLE;

// Textured mobs by entity type name, built from mobs.json. Skins and meshes
// are cached by path so mobs sharing a file share the GPU object; map nodes
// are stable, so the pointers held by MobModel stay valid.
std::unordered_map<std::string, MobModel> g_mobModels;
std::unordered_map<std::string, SkinTexture> g_skinCache;
std::unordered_map<std::string, EntityModelMesh> g_meshCache;

const SkinTexture* g_steveSkin = nullptr; // Default skin for the first-person arm.

// Chests: meshes by type (single/left/right), skins by [kind-1][type].
EntityModelMesh g_chestMeshes[3];
SkinTexture g_chestSkins[3][3];

// A textured draw waiting to be batched: one part of one entity.
struct TexturedItem {
    const EntityModelMesh* mesh;
    VkDescriptorSet skin;
    uint32_t indexCount;
    uint32_t firstIndex;
    TexturedInstance instance;
};

// Everything a frame wants drawn with the textured pipeline. Items are
// sorted by (mesh, skin, index range), and runs of identical ones become one
// command with an instance count, so ten zombies' right legs are one draw. A
// run of commands sharing a mesh and skin is one vkCmdDrawIndexedIndirect.
// Each owner has its own per-frame buffers because its pass flushes at a
// different point of the same frame.
enum BatchOwner : uint32_t { BATCH_MOBS, BATCH_BLOCK_ENTITIES, BATCH_OWNER_COUNT };

constexpr uint32_t MAX_TEXTURED_INSTANCES = 8192; // Also the command capacity: a command holds at least one.

struct TexturedBatch {
    struct Run {
        const EntityModelMesh* mesh;
        VkDescriptorSet skin;
        uint32_t firstCommand;
        uint32_t commandCount;
    };

    AllocatedBuffer instances[FRAMES_IN_FLIGHT]; // TexturedInstance records.
    AllocatedBuffer commands[FRAMES_IN_FLIGHT];  // VkDrawIndexedIndirectCommand.
    void* instancesMapped[FRAMES_IN_FLIGHT] = {};
    void* commandsMapped[FRAMES_IN_FLIGHT] = {};
    std::vector<TexturedItem> items; // Cleared by every flush.
    std::vector<Run> runs;
};
TexturedBatch g_texturedBatches[BATCH_OWNER_COUNT];

// Shadow casters: one model matrix per part, per cascade, per frame in flight.
// Each cascade records into its own pair of buffers (the open frame may still
// be reading the others).
constexpr uint32_t SHADOW_CASCADE_SLOTS = 3;
constexpr uint32_t MAX_SHADOW_INSTANCES = 2048;
constexpr float MAX_ENTITY_SHADOW_DISTANCE = 96.0f; // Entities further than this cast no shadow.

struct ShadowCasterBuffers {
    AllocatedBuffer instances[FRAMES_IN_FLIGHT][SHADOW_CASCADE_SLOTS];
    AllocatedBuffer commands[FRAMES_IN_FLIGHT][SHADOW_CASCADE_SLOTS];
    void* instancesMapped[FRAMES_IN_FLIGHT][SHADOW_CASCADE_SLOTS] = {};
    void* commandsMapped[FRAMES_IN_FLIGHT][SHADOW_CASCADE_SLOTS] = {};
};
ShadowCasterBuffers g_shadowCasters;

// A caster part waiting to be batched; a null mesh is the hitbox cube.
struct ShadowItem {
    const EntityModelMesh* mesh;
    uint32_t indexCount;
    uint32_t firstIndex;
    glm::mat4 model;
};
std::vector<ShadowItem> g_shadowTextured;
std::vector<ShadowItem> g_shadowBoxes;

// What a block entity draws with. Add a case per new kind (signs, beds,
// banners, shulker boxes, skulls...) with its mesh and skin; everything else
// (scanning, culling, batching, shadows) already handles it.
struct BlockEntityModel {
    const EntityModelMesh* mesh = nullptr;
    const SkinTexture* skin = nullptr;
};

BlockEntityModel LookupBlockEntityModel(uint8_t kind, uint8_t type)
{
    switch (kind)
    {
    case 1: // Chest, trapped chest, ender chest.
    case 2:
    case 3:
    {
        int variant = kind == 3 ? 0 : type; // Ender chests are always single.
        if (variant > 2) return {};
        const EntityModelMesh& mesh = g_chestMeshes[variant];
        const SkinTexture& skin = g_chestSkins[kind - 1][variant];
        if (skin.descriptorSet == VK_NULL_HANDLE || mesh.indexCount == 0) return {};
        return { &mesh, &skin };
    }
    default:
        return {};
    }
}

// Allocates a buffer via VMA without registering it in VulkanInit's global
// `allocatedBuffers` list. EntityRenderer::Shutdown() destroys every buffer
// it creates itself (unmapping first where needed) - going through the
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

// The placeholder box - now just a single unit box through AppendBox (see
// EntityBoxMesh.hpp for the shared winding convention every procedural
// entity mesh, this one included, follows).
void BuildCubeMesh(std::vector<EntityVertex>& vertices, std::vector<uint32_t>& indices)
{
    AppendBox(vertices, indices, glm::vec3(0.0f), glm::vec3(1.0f));
}

// Uploads a host-built mesh (vertices/indices already populated by
// BuildCubeMesh/QuadrupedModel::BuildMesh/GolemModel::BuildMesh) into a
// fresh GPU-only vertex/index buffer pair via a staging buffer - shared by
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

void UploadJsonMesh(const std::string& modelPath, EntityModelMesh& out, float scale = 1.0f, bool animated = false)
{
    EntityJsonModel::ModelDef model = EntityJsonModel::Load(modelPath);
    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
    EntityJsonModel::MeshInfo info = EntityJsonModel::BuildMesh(model, vertices, indices, scale, animated);
    UploadTexturedMesh(vertices, indices, out);
    out.scale = scale;
    out.feetHeight = model.feetY / 16.0f * scale;
    if (animated)
    {
        out.parts = std::move(info.parts);
        out.roles = EntityAnimation::FindRoles(out.parts);
    }
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

// Loads one skin PNG as its own plain 2D texture (no array, no mipmaps -
// unlike TextureManager's block/item array, a skin is viewed close-up on a
// single humanoid-sized entity, so there's no minification pressure worth
// the extra blit-chain complexity) and registers a descriptor set for it
// against g_textureSetLayout. Any size works (unlike TextureManager, which
// only accepts exactly 16x16) - player/zombie skins are both 64x64 in this
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
    // it exposed a filter choice - linear filtering would blur the seams
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
    int width = 0, height = 0;
    uint8_t* pixels = LoadImageRGBA(path, width, height);
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

// This client has no real skin fetching (no Mojang session-server lookup -
// see NetworkClient's own comment on why it doesn't attempt signed chat/
// online-mode auth either), so every player and mannequin uses the same
// bundled default skin. Zombie and skeleton get their own vanilla textures -
// the whole point of this pass is "one shared humanoid shape, texture is
// what tells them apart," per the task this was scoped to.
void LoadSkins()
{
    const char* chestDir = "assets/minecraft/textures/entity/chest/";
    const char* chestNames[3] = { "normal", "trapped", "ender" };
    const char* typeSuffix[3] = { "", "_left", "_right" };
    for (int kind = 0; kind < 3; kind++)
    {
        for (int type = 0; type < (kind == 2 ? 1 : 3); type++)
        {
            g_chestSkins[kind][type] = LoadSkinTexture(std::string(chestDir) + chestNames[kind] + typeSuffix[type] + ".png");
        }
    }
}

void DestroySkin(SkinTexture& skin)
{
    if (skin.sampler != VK_NULL_HANDLE) vkDestroySampler(GetDevice(), skin.sampler, nullptr);
    if (skin.view != VK_NULL_HANDLE) vkDestroyImageView(GetDevice(), skin.view, nullptr);
    if (skin.image.image != VK_NULL_HANDLE) vmaDestroyImage(vmaAllocator, skin.image.image, skin.image.allocation);
    skin = SkinTexture{};
}

constexpr const char* MOB_MODEL_DIR = "resources/volcano/models/entity/";
constexpr const char* MOB_SKIN_DIR = "assets/minecraft/textures/entity/"; // Logical pack path.

const SkinTexture* CachedSkin(const std::string& skinPath)
{
    auto it = g_skinCache.find(skinPath);
    if (it != g_skinCache.end()) return &it->second;
    return &g_skinCache.emplace(skinPath, LoadSkinTexture(MOB_SKIN_DIR + skinPath)).first->second;
}

const SkinTexture* ArmorSkin(const std::string& path)
{
    static std::unordered_map<std::string, const SkinTexture*> cache;
    auto found = cache.find(path);
    if (found != cache.end()) return found->second;
    const SkinTexture* skin = nullptr;
    try
    {
        skin = CachedSkin(path);
    }
    catch (const std::exception&)
    {
    }
    cache[path] = skin;
    return skin;
}

const EntityModelMesh* CachedMesh(const std::string& modelFile, float scale)
{
    std::string key = modelFile + "@" + std::to_string(scale);
    auto it = g_meshCache.find(key);
    if (it != g_meshCache.end()) return &it->second;
    EntityModelMesh mesh;
    UploadJsonMesh(MOB_MODEL_DIR + modelFile, mesh, scale, true);
    return &g_meshCache.emplace(key, mesh).first->second;
}

// Reads mobs.json: { "mobs": { "<entity name>": { "model", "skin", "scale"?,
// "layers"?: [ { "model", "skin" } ] } } }. A bad entry is logged and skipped.
void LoadMobManifest()
{
    std::string path = std::string(MOB_MODEL_DIR) + "mobs.json";
    nlohmann::json manifest;
    try
    {
        std::ifstream file(path);
        if (!file.is_open()) throw std::runtime_error("failed to open " + path);
        file >> manifest;
    }
    catch (const std::exception& e)
    {
        Log::Error(std::string("EntityRenderer: mob manifest unreadable: ") + e.what());
        return;
    }

    for (const auto& [name, entry] : manifest.at("mobs").items())
    {
        try
        {
            float scale = entry.value("scale", 1.0f);
            MobModel mob;
            mob.layers.push_back({ CachedMesh(entry.at("model").get<std::string>(), scale),
                CachedSkin(entry.at("skin").get<std::string>()) });
            if (entry.value("armor", false))
            {
                mob.armorOuter = CachedMesh("armor_outer.json", scale);
                mob.armorInner = CachedMesh("armor_inner.json", scale);
            }
            if (entry.contains("layers"))
            {
                for (const auto& layer : entry.at("layers"))
                {
                    mob.layers.push_back({ CachedMesh(layer.at("model").get<std::string>(), scale),
                        CachedSkin(layer.at("skin").get<std::string>()) });
                }
            }
            g_mobModels[name] = std::move(mob);
        }
        catch (const std::exception& e)
        {
            Log::Error("EntityRenderer: skipping mob '" + name + "': " + e.what());
        }
    }

    g_steveSkin = CachedSkin("player/wide/steve.png");
}

// Block-shaped entities (falling_block, tnt) reuse the humanoid textured
// pipeline: one shared unit-cube mesh, plus a small per-block-state texture
// with the six face tiles side by side (Up, Down, North, South, East, West -
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

        int width = 0, height = 0;
        std::string path = "assets/minecraft/textures/block/" + name + ".png";
        uint8_t* pixels = LoadImageRGBA(path, width, height);
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

    // Humanoid skin textures - a separate layout/pool from the instance SSBO
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
    // texture (see MAX_BLOCK_TEXTURES) - cheap to over-provision.
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

void CreateTexturedBatches()
{
    for (TexturedBatch& batch : g_texturedBatches)
    {
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            batch.instances[i] = CreateOwnedBuffer(MAX_TEXTURED_INSTANCES * sizeof(TexturedInstance),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
            vmaMapMemory(vmaAllocator, batch.instances[i].allocation, &batch.instancesMapped[i]);

            batch.commands[i] = CreateOwnedBuffer(MAX_TEXTURED_INSTANCES * sizeof(VkDrawIndexedIndirectCommand),
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
            vmaMapMemory(vmaAllocator, batch.commands[i].allocation, &batch.commandsMapped[i]);
        }
    }
}

void CreateShadowCasterBuffers()
{
    for (uint32_t f = 0; f < FRAMES_IN_FLIGHT; f++)
    {
        for (uint32_t c = 0; c < SHADOW_CASCADE_SLOTS; c++)
        {
            g_shadowCasters.instances[f][c] = CreateOwnedBuffer(MAX_SHADOW_INSTANCES * sizeof(glm::mat4),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
            vmaMapMemory(vmaAllocator, g_shadowCasters.instances[f][c].allocation, &g_shadowCasters.instancesMapped[f][c]);
            g_shadowCasters.commands[f][c] = CreateOwnedBuffer(MAX_SHADOW_INSTANCES * sizeof(VkDrawIndexedIndirectCommand),
                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
            vmaMapMemory(vmaAllocator, g_shadowCasters.commands[f][c].allocation, &g_shadowCasters.commandsMapped[f][c]);
        }
    }
}

void DestroyShadowCasterBuffers()
{
    for (uint32_t f = 0; f < FRAMES_IN_FLIGHT; f++)
    {
        for (uint32_t c = 0; c < SHADOW_CASCADE_SLOTS; c++)
        {
            AllocatedBuffer& instances = g_shadowCasters.instances[f][c];
            AllocatedBuffer& commands = g_shadowCasters.commands[f][c];
            if (g_shadowCasters.instancesMapped[f][c] != nullptr) vmaUnmapMemory(vmaAllocator, instances.allocation);
            if (instances.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, instances.buffer, instances.allocation);
            if (g_shadowCasters.commandsMapped[f][c] != nullptr) vmaUnmapMemory(vmaAllocator, commands.allocation);
            if (commands.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, commands.buffer, commands.allocation);
            instances = AllocatedBuffer{};
            commands = AllocatedBuffer{};
            g_shadowCasters.instancesMapped[f][c] = nullptr;
            g_shadowCasters.commandsMapped[f][c] = nullptr;
        }
    }
    g_shadowTextured = {};
    g_shadowBoxes = {};
}

void DestroyTexturedBatches()
{
    for (TexturedBatch& batch : g_texturedBatches)
    {
        for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++)
        {
            if (batch.instancesMapped[i] != nullptr) vmaUnmapMemory(vmaAllocator, batch.instances[i].allocation);
            if (batch.instances[i].buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(vmaAllocator, batch.instances[i].buffer, batch.instances[i].allocation);
            }
            if (batch.commandsMapped[i] != nullptr) vmaUnmapMemory(vmaAllocator, batch.commands[i].allocation);
            if (batch.commands[i].buffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(vmaAllocator, batch.commands[i].buffer, batch.commands[i].allocation);
            }
            batch.instances[i] = AllocatedBuffer{};
            batch.commands[i] = AllocatedBuffer{};
            batch.instancesMapped[i] = nullptr;
            batch.commandsMapped[i] = nullptr;
        }
        batch.items = {};
        batch.runs = {};
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
    // counter-clockwise here - see EntityBoxMesh.hpp's own comment.
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
    // draw order - no extra wiring needed for that culling method.
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

    // Shadow-receiving twin. Skipped when a shader pack replaces the shader; a
    // failure just leaves these entities unshadowed.
    if (!shaders.PackProvides("entity.vert") && !shaders.PackProvides("entity.frag"))
    {
        VkShaderModule shadowVert = VK_NULL_HANDLE, shadowFrag = VK_NULL_HANDLE;
        try
        {
            CreateShaderPair(shaders, "entity_shadow", shadowVert, shadowFrag);

            VkDescriptorSetLayout shadowedSetLayouts[] = { cameraSetLayout, g_instanceSetLayout, shadowSetLayout };
            VkPipelineLayoutCreateInfo shadowedLayoutInfo = pipelineLayoutInfo;
            shadowedLayoutInfo.setLayoutCount = 3;
            shadowedLayoutInfo.pSetLayouts = shadowedSetLayouts;
            if (vkCreatePipelineLayout(GetDevice(), &shadowedLayoutInfo, nullptr, &g_shadowedPipelineLayout) != VK_SUCCESS)
            {
                throw std::runtime_error("shadowed layout");
            }

            VkPipelineShaderStageCreateInfo shadowedStages[2] = { stages[0], stages[1] };
            shadowedStages[0].module = shadowVert;
            shadowedStages[1].module = shadowFrag;
            VkGraphicsPipelineCreateInfo shadowedInfo = pipelineInfo;
            shadowedInfo.pStages = shadowedStages;
            shadowedInfo.layout = g_shadowedPipelineLayout;
            if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &shadowedInfo, nullptr, &g_shadowedPipeline) != VK_SUCCESS)
            {
                g_shadowedPipeline = VK_NULL_HANDLE;
                throw std::runtime_error("shadowed pipeline");
            }
        }
        catch (const std::exception& e)
        {
            Log::Error(std::string("[ERROR] Shadow-receiving flat entities unavailable: ") + e.what());
            if (g_shadowedPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(GetDevice(), g_shadowedPipelineLayout, nullptr);
                g_shadowedPipelineLayout = VK_NULL_HANDLE;
            }
        }
        if (shadowFrag != VK_NULL_HANDLE) vkDestroyShaderModule(GetDevice(), shadowFrag, nullptr);
        if (shadowVert != VK_NULL_HANDLE) vkDestroyShaderModule(GetDevice(), shadowVert, nullptr);
    }

    DestroyShaderPair(vertModule, fragModule);
}

// Same fixed-function state as CreatePipeline() above (same render pass/
// subpass, same depth test since humanoids share the world's depth
// attachment too, same winding/cull convention - EntityJsonModel::BuildMesh
// follows BuildCubeMesh's own winding rule deliberately), differing only in
// vertex layout (+UV, plus the instance-rate TexturedInstance record on
// binding 1 instead of an SSBO), shaders, and descriptor set 1 (a skin
// texture instead of the instance buffer).
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

    VkVertexInputBindingDescription bindings[2] = { binding, {} };
    bindings[1].binding = 1;
    bindings[1].stride = sizeof(TexturedInstance);
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

    std::array<VkVertexInputAttributeDescription, 8> attributes{};
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
    const uint32_t instanceOffsets[5] = {
        offsetof(TexturedInstance, positionYaw), offsetof(TexturedInstance, hurtColor),
        offsetof(TexturedInstance, partPivot), offsetof(TexturedInstance, partOffset),
        offsetof(TexturedInstance, partRotation) };
    for (uint32_t i = 0; i < 5; i++)
    {
        attributes[3 + i].binding = 1;
        attributes[3 + i].location = 3 + i;
        attributes[3 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributes[3 + i].offset = instanceOffsets[i];
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
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

    // Same winding/cull convention as CreatePipeline() above - EntityJsonModel::
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

    VkDescriptorSetLayout setLayouts[] = { cameraSetLayout, g_textureSetLayout };
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 2;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
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

    // Shadow-receiving twin. Skipped when a shader pack replaces the shader; a
    // failure just leaves entities unshadowed.
    if (!shaders.PackProvides("entity_textured.vert") && !shaders.PackProvides("entity_textured.frag"))
    {
        VkShaderModule shadowVert = VK_NULL_HANDLE, shadowFrag = VK_NULL_HANDLE;
        try
        {
            CreateShaderPair(shaders, "entity_textured_shadow", shadowVert, shadowFrag);

            VkDescriptorSetLayout shadowedSetLayouts[] = { cameraSetLayout, g_textureSetLayout, shadowSetLayout };
            VkPipelineLayoutCreateInfo shadowedLayoutInfo = pipelineLayoutInfo;
            shadowedLayoutInfo.setLayoutCount = 3;
            shadowedLayoutInfo.pSetLayouts = shadowedSetLayouts;
            if (vkCreatePipelineLayout(GetDevice(), &shadowedLayoutInfo, nullptr, &g_humanoidShadowedLayout) != VK_SUCCESS)
            {
                throw std::runtime_error("shadowed layout");
            }

            VkPipelineShaderStageCreateInfo shadowedStages[2] = { stages[0], stages[1] };
            shadowedStages[0].module = shadowVert;
            shadowedStages[1].module = shadowFrag;
            VkGraphicsPipelineCreateInfo shadowedInfo = pipelineInfo;
            shadowedInfo.pStages = shadowedStages;
            shadowedInfo.layout = g_humanoidShadowedLayout;
            if (vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &shadowedInfo, nullptr, &g_humanoidShadowedPipeline) != VK_SUCCESS)
            {
                g_humanoidShadowedPipeline = VK_NULL_HANDLE;
                throw std::runtime_error("shadowed pipeline");
            }
        }
        catch (const std::exception& e)
        {
            Log::Error(std::string("[ERROR] Shadow-receiving entities unavailable: ") + e.what());
            if (g_humanoidShadowedLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(GetDevice(), g_humanoidShadowedLayout, nullptr);
                g_humanoidShadowedLayout = VK_NULL_HANDLE;
            }
        }
        if (shadowFrag != VK_NULL_HANDLE) vkDestroyShaderModule(GetDevice(), shadowFrag, nullptr);
        if (shadowVert != VK_NULL_HANDLE) vkDestroyShaderModule(GetDevice(), shadowVert, nullptr);
    }

    DestroyShaderPair(vertModule, fragModule);
}

// A mob the Hi-Z test hides while it is close to the camera is the case worth a line in the log: a mob that near is
// rarely really behind terrain, so this tells client culling from a mob the server hid or put somewhere wrong.
// At most one line per entity every few seconds.
void LogNearHiZCull(uint32_t id, const Entity& entity, const glm::vec3& position, const glm::vec3& camera,
    std::chrono::steady_clock::time_point now)
{
    constexpr float NEAR_BLOCKS = 5.0f;
    constexpr auto REPEAT = std::chrono::seconds(3);
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> lastLogged;

    float distance = glm::length(position - camera);
    if (distance > NEAR_BLOCKS) return;
    auto found = lastLogged.find(id);
    if (found != lastLogged.end() && now - found->second < REPEAT) return;
    lastLogged[id] = now;
    if (lastLogged.size() > 512) lastLogged.clear();

    const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
    Log::Info("[HIZ] Hides " + std::string(info != nullptr ? info->name : "entity") + " #" + std::to_string(id)
        + " " + std::to_string(distance) + " blocks away at (" + std::to_string(position.x) + ", "
        + std::to_string(position.y) + ", " + std::to_string(position.z) + "), camera at ("
        + std::to_string(camera.x) + ", " + std::to_string(camera.y) + ", " + std::to_string(camera.z)
        + "), box " + std::to_string(entity.boundingBox.x) + " x " + std::to_string(entity.boundingBox.y));
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

// Both pipelines and their layouts - everything shader-dependent, and
// nothing else (meshes, skins, per-frame buffers stay put).
void DestroyPipelines()
{
    VkDevice dev = GetDevice();
    if (g_pipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, g_pipeline, nullptr);
    if (g_pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, g_pipelineLayout, nullptr);
    if (g_shadowedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, g_shadowedPipeline, nullptr);
    if (g_shadowedPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, g_shadowedPipelineLayout, nullptr);
    g_shadowedPipeline = VK_NULL_HANDLE;
    g_shadowedPipelineLayout = VK_NULL_HANDLE;
    if (g_humanoidPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, g_humanoidPipeline, nullptr);
    if (g_humanoidPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, g_humanoidPipelineLayout, nullptr);
    if (g_humanoidShadowedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(dev, g_humanoidShadowedPipeline, nullptr);
    if (g_humanoidShadowedLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, g_humanoidShadowedLayout, nullptr);
    g_humanoidShadowedPipeline = VK_NULL_HANDLE;
    g_humanoidShadowedLayout = VK_NULL_HANDLE;
    g_pipeline = VK_NULL_HANDLE;
    g_pipelineLayout = VK_NULL_HANDLE;
    g_humanoidPipeline = VK_NULL_HANDLE;
    g_humanoidPipelineLayout = VK_NULL_HANDLE;
}

void AddTextured(BatchOwner owner, const EntityModelMesh& mesh, VkDescriptorSet skin, uint32_t indexCount,
    uint32_t firstIndex, const TexturedInstance& instance)
{
    g_texturedBatches[owner].items.push_back({ &mesh, skin, indexCount, firstIndex, instance });
}

// Sorts the owner's items, writes their records and commands into frameIndex's
// buffers and draws them: one bind pair and one indirect draw per run of
// commands sharing a mesh and skin (or plain instanced draws when `indirect`
// is off or unsupported). Returns the triangles drawn. Needs the open render
// pass and the viewport/scissor set.
uint64_t FlushTextured(BatchOwner owner, VkCommandBuffer cmd, uint32_t frameIndex,
    Engine::Vulkan::VulkanRenderEngine* vulkan, bool indirect)
{
    TexturedBatch& batch = g_texturedBatches[owner];
    std::vector<TexturedItem>& items = batch.items;
    const uint32_t slot = frameIndex % FRAMES_IN_FLIGHT;
    if (items.empty()) return 0;
    if (g_humanoidPipeline == VK_NULL_HANDLE || batch.instancesMapped[slot] == nullptr)
    {
        items.clear();
        return 0;
    }
    if (items.size() > MAX_TEXTURED_INSTANCES) items.resize(MAX_TEXTURED_INSTANCES);

    std::sort(items.begin(), items.end(), [](const TexturedItem& a, const TexturedItem& b)
    {
        if (a.mesh != b.mesh) return std::less<const EntityModelMesh*>()(a.mesh, b.mesh);
        if (a.skin != b.skin) return a.skin < b.skin;
        if (a.firstIndex != b.firstIndex) return a.firstIndex < b.firstIndex;
        return a.indexCount < b.indexCount;
    });

    auto* records = static_cast<TexturedInstance*>(batch.instancesMapped[slot]);
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(batch.commandsMapped[slot]);
    batch.runs.clear();
    uint32_t commandCount = 0;
    uint64_t triangles = 0;
    for (size_t i = 0; i < items.size();)
    {
        const TexturedItem& first = items[i];
        size_t j = i;
        while (j < items.size() && items[j].mesh == first.mesh && items[j].skin == first.skin
            && items[j].firstIndex == first.firstIndex && items[j].indexCount == first.indexCount)
        {
            records[j] = items[j].instance;
            j++;
        }
        const uint32_t instanceCount = static_cast<uint32_t>(j - i);
        commands[commandCount] = { first.indexCount, instanceCount, first.firstIndex, 0, static_cast<uint32_t>(i) };
        triangles += static_cast<uint64_t>(first.indexCount / 3) * instanceCount;

        if (!batch.runs.empty() && batch.runs.back().mesh == first.mesh && batch.runs.back().skin == first.skin)
        {
            batch.runs.back().commandCount++;
        }
        else
        {
            batch.runs.push_back({ first.mesh, first.skin, commandCount, 1 });
        }
        commandCount++;
        i = j;
    }
    vmaFlushAllocation(vmaAllocator, batch.instances[slot].allocation, 0, sizeof(TexturedInstance) * items.size());
    vmaFlushAllocation(vmaAllocator, batch.commands[slot].allocation, 0, sizeof(VkDrawIndexedIndirectCommand) * commandCount);
    items.clear();

    // With a shadow map this frame, the shadow-receiving twin draws instead:
    // same records and skin sets, plus the map as set 2.
    const bool shadowed = vulkan != nullptr && vulkan->AreShadowsActive() && g_humanoidShadowedPipeline != VK_NULL_HANDLE;
    const VkPipelineLayout layout = shadowed ? g_humanoidShadowedLayout : g_humanoidPipelineLayout;
    VkDescriptorSet shadowSetHandle = shadowed ? vulkan->GetShadowSet() : VK_NULL_HANDLE;
    const bool useIndirect = indirect && indirectDrawSupported;
    constexpr uint32_t STRIDE = sizeof(VkDrawIndexedIndirectCommand);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowed ? g_humanoidShadowedPipeline : g_humanoidPipeline);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 1, 1, &batch.instances[slot].buffer, &zero);
    for (const TexturedBatch::Run& run : batch.runs)
    {
        vkCmdBindVertexBuffers(cmd, 0, 1, &run.mesh->vertexBuffer.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, run.mesh->indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        VkDescriptorSet sets[] = { cameraSets[frameIndex], run.skin, shadowSetHandle };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, shadowed ? 3 : 2, sets, 0, nullptr);

        if (useIndirect)
        {
            vkCmdDrawIndexedIndirect(cmd, batch.commands[slot].buffer, run.firstCommand * STRIDE, run.commandCount, STRIDE);
        }
        else
        {
            for (uint32_t k = run.firstCommand; k < run.firstCommand + run.commandCount; k++)
            {
                const VkDrawIndexedIndirectCommand& c = commands[k];
                vkCmdDrawIndexed(cmd, c.indexCount, c.instanceCount, c.firstIndex, 0, c.firstInstance);
            }
        }
    }
    return triangles;
}

} // namespace

void EntityRenderer::Init(const Engine::ShaderLibrary& shaders)
{
    // All must run before CreatePerFrameBuffers() - it seeds each shape's
    // initial indirect-command indexCount from these.
    UploadCubeMesh();
    UploadQuadrupedMesh();
    UploadGolemMesh();
    UploadPigMesh();
    UploadCowMesh();
    CreateDescriptorLayoutAndPool();
    CreatePerFrameBuffers();
    CreateTexturedBatches();
    CreateShadowCasterBuffers();
    CreatePipeline(shaders);

    // Humanoid path - CreateDescriptorLayoutAndPool() above must run first
    // (LoadSkins() allocates each skin's descriptor set from
    // g_textureDescriptorPool against g_textureSetLayout, both created
    // there).
    UploadBlockMesh();
    UploadJsonMesh("resources/volcano/models/entity/chest.json", g_chestMeshes[0]);
    UploadJsonMesh("resources/volcano/models/entity/chest_left.json", g_chestMeshes[1]);
    UploadJsonMesh("resources/volcano/models/entity/chest_right.json", g_chestMeshes[2]);
    LoadSkins();
    LoadMobManifest();
    CreateHumanoidPipeline(shaders);

    Log::Info("Entity renderer initialized.");
}

void EntityRenderer::ReloadPipelines(const Engine::ShaderLibrary& shaders)
{
    DestroyPipelines();
    CreatePipeline(shaders);
    CreateHumanoidPipeline(shaders);
}

void EntityRenderer::ReloadTextures()
{
    for (auto& [path, skin] : g_skinCache) DestroySkin(skin);
    g_skinCache.clear();
    g_mobModels.clear();
    g_steveSkin = nullptr;
    for (auto& row : g_chestSkins)
    {
        for (SkinTexture& skin : row) DestroySkin(skin);
    }
    for (auto& [stateId, texture] : g_blockTextures) DestroySkin(texture);
    g_blockTextures.clear();

    // Every skin set came from this pool and all were just dropped.
    if (g_textureDescriptorPool != VK_NULL_HANDLE) vkResetDescriptorPool(GetDevice(), g_textureDescriptorPool, 0);

    // Meshes stay cached (they come from client JSON, not the pack).
    LoadSkins();
    LoadMobManifest();
}

VkDescriptorSet EntityRenderer::GetDefaultSkinSet()
{
    return g_steveSkin != nullptr ? g_steveSkin->descriptorSet : VK_NULL_HANDLE;
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
    if (g_shadowedPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_shadowedPipeline, nullptr);
        g_shadowedPipeline = VK_NULL_HANDLE;
    }
    if (g_shadowedPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetDevice(), g_shadowedPipelineLayout, nullptr);
        g_shadowedPipelineLayout = VK_NULL_HANDLE;
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

    // Textured path.
    DestroyTexturedBatches();
    DestroyShadowCasterBuffers();
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
    if (g_humanoidShadowedPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(GetDevice(), g_humanoidShadowedPipeline, nullptr);
        g_humanoidShadowedPipeline = VK_NULL_HANDLE;
    }
    if (g_humanoidShadowedLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(GetDevice(), g_humanoidShadowedLayout, nullptr);
        g_humanoidShadowedLayout = VK_NULL_HANDLE;
    }

    for (auto& [path, skin] : g_skinCache) DestroySkin(skin);
    g_skinCache.clear();
    g_mobModels.clear();
    g_steveSkin = nullptr;
    for (auto& row : g_chestSkins)
    {
        for (SkinTexture& skin : row) DestroySkin(skin);
    }
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

    std::vector<EntityModelMesh*> meshes = { &g_blockMesh, &g_chestMeshes[0], &g_chestMeshes[1], &g_chestMeshes[2] };
    for (auto& [key, cached] : g_meshCache) meshes.push_back(&cached);
    for (EntityModelMesh* mesh : meshes)
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
    g_meshCache.clear();
}

void EntityRenderer::RecordDraw(VkCommandBuffer cmd, uint32_t frameIndex, GlobalState* state,
    const glm::mat4& view, const glm::mat4& proj, const glm::vec3& cameraPosition, bool indirect)
{
    std::array<glm::vec4, 4> frustumPlanes = ExtractFrustumPlanes(proj * view);

    // Frustum-cull first, then keep only the closest MAX_VISIBLE_ENTITIES -
    // the "simple entity limit" culling method from the entity renderer
    // plan, applied after frustum culling so it prioritizes what's actually
    // in view instead of arbitrarily dropping visible entities in favor of
    // off-screen ones.
    auto now = std::chrono::steady_clock::now();

    // Read per frame so a settings reload applies immediately. All four
    // channels are 0-255; alpha is how strongly the color replaces the model.
    // TODO: Graphics.EntityHurt.IncludeArmor - armor isn't rendered yet.
    auto hurtChannel = [state](const char* channel, uint32_t fallback) {
        std::string key = std::string("Graphics.EntityHurt.Color.") + channel;
        return static_cast<float>(std::get<uint32_t>(state->config->Get(key, uint32_t{fallback}))) / 255.0f;
    };
    glm::vec4 hurtColor(hurtChannel("Red", 255), hurtChannel("Green", 0), hurtChannel("Blue", 0), hurtChannel("Alpha", 64));
    auto HurtColorFor = [&](const Entity& entity) {
        return entity.IsHurt(now) ? hurtColor : glm::vec4(hurtColor.r, hurtColor.g, hurtColor.b, 0.0f);
    };

    // Off draws every in-frustum entity, for telling client culling from server hiding.
    bool occlusionCulling = std::get<bool>(state->config->Get("Graphics.EntityOcclusionCulling", false));

    std::vector<std::pair<float, const Entity*>> visible;
    // Humanoid-shaped entities (player/zombie/mannequin/skeleton) are pulled
    // out of the instanced path entirely and drawn separately below via
    // EntityJsonModel - they need a real UV-mapped mesh and their own bound
    // skin texture, neither of which the flat-tinted instance SSBO (shared
    // mesh per shape, no per-instance texture) has any way to carry.
    // Distance-sorting/MAX_VISIBLE_ENTITIES still applies to them via the
    // same `visible` pass below; they're just routed to a different draw
    // call afterward instead of into one of the shape instance lists. Holds
    // resolved values rather than Entity pointers - see HumanoidDraw's own
    // comment for why that distinction is load-bearing here.
    std::vector<HumanoidDraw> humanoidVisible;
    std::vector<BlockDraw> blockVisible;

    // Wall-clock "age" in ticks for the idle arm sway.
    static const auto animationStart = std::chrono::steady_clock::now();
    float ageTicks = std::fmod(std::chrono::duration<float>(now - animationStart).count(), 3000.0f) * 20.0f;
    g_walkFrame++;
    if (g_walkFrame % 600 == 0)
    {
        std::erase_if(g_walkStates, [](const auto& entry) { return g_walkFrame - entry.second.lastSeenFrame > 300; });
    }
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
        // humanoid - the invisibility status effect's flag aside, which
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
            // Riders get no position packets, so cull and draw them at their seat.
            glm::vec3 renderPosition = RenderPositionOf(state, entity, now);
            glm::vec3 center = renderPosition + glm::vec3(0.0f, halfHeight, 0.0f);
            float radius = glm::length(glm::vec3(halfWidth, halfHeight, halfWidth));

            if (!SphereInFrustum(frustumPlanes, center, radius)) return;

            // Hi-Z: skip non-players hidden behind terrain. Players stay
            // drawn so they can be seen through walls.
            if (player == nullptr && occlusionCulling
                && occlusionSnapshot.IsEntityOccluded(renderPosition, entity.boundingBox.x, entity.boundingBox.y))
            {
                LogNearHiZCull(id, entity, renderPosition, cameraPosition, now);
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
        // EntityRegistry::ShapeKind rather than one flat list - each shape
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

            // Any type listed in mobs.json takes the textured path, whatever its shape.
            if (info != nullptr)
            {
                auto mob = g_mobModels.find(info->name);
                if (mob != g_mobModels.end())
                {
                    HumanoidDraw draw{
                        RenderPositionOf(state, *entity, now), entity->InterpolatedYaw(now), HurtColorFor(*entity),
                        &mob->second };

                    if (mob->second.armorOuter != nullptr)
                    {
                        const EquipmentSlot slots[4] = { EquipmentSlot::Head, EquipmentSlot::Chest, EquipmentSlot::Legs, EquipmentSlot::Feet };
                        for (size_t i = 0; i < 4; i++) draw.armor[i] = HeldItemName(entity->GetEquipment(slots[i]));
                    }

                    EntityAnimation::WalkState& walk = g_walkStates[entity->id];
                    walk.Update(draw.position, now);
                    walk.lastSeenFrame = g_walkFrame;
                    draw.anim.walkPosition = walk.position;
                    draw.anim.walkSpeed = walk.speed;
                    draw.anim.ageTicks = ageTicks;

                    float sinceSwing = std::chrono::duration<float>(now - entity->lastSwingTime).count();
                    if (entity->lastSwingTime != std::chrono::steady_clock::time_point{}
                        && sinceSwing < EntityAnimation::SWING_SECONDS)
                    {
                        draw.anim.attackTime = sinceSwing / EntityAnimation::SWING_SECONDS;
                    }

                    bool swimming = false;
                    if (const PlayerEntity* holder = state->FindPlayerEntity(entity->id))
                    {
                        draw.mainhandItem = HeldItemName(holder->GetEquipment(EquipmentSlot::MainHand));
                        draw.offhandItem = HeldItemName(holder->GetEquipment(EquipmentSlot::OffHand));
                        swimming = holder->IsSwimming();
                        if (holder->IsCrouching()) draw.anim.posture = EntityAnimation::Posture::Crouching;
                    }
                    draw.anim.holdingMainhand = draw.mainhandItem != nullptr;
                    draw.anim.holdingOffhand = draw.offhandItem != nullptr;

                    RideInfo ride = RideInfoFor(state, *entity);
                    draw.anim.riding = ride.riding;
                    draw.anim.ridingBoat = ride.boat;
                    draw.anim.swimAmount = walk.EaseSwim(swimming && !ride.riding, now);
                    if (ride.riding) draw.anim.posture = EntityAnimation::Posture::Riding;
                    else if (draw.anim.swimAmount > 0.0f) draw.anim.posture = EntityAnimation::Posture::Swimming;
                    walk.posture = draw.anim.posture;
                    walk.riding = draw.anim.riding;
                    walk.ridingBoat = draw.anim.ridingBoat;
                    humanoidVisible.push_back(draw);
                    continue;
                }
            }
            if (shape == EntityRegistry::ShapeKind::Humanoid) shape = EntityRegistry::ShapeKind::Cube;

            if (shape == EntityRegistry::ShapeKind::Block && entity->blockStateId >= 0)
            {
                blockVisible.push_back(BlockDraw{
                    entity->InterpolatedPosition(now), HurtColorFor(*entity), entity->blockStateId });
                continue;
            }

            GPUEntityInstance gpu{};
            gpu.positionYaw = glm::vec4(RenderPositionOf(state, *entity, now), entity->InterpolatedYaw(now));
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
        // - entity.vert doesn't need to know shapes were split up at all.
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

    // The local player has no entity of their own; draw them as a plain
    // "player" humanoid when the camera isn't in their head. The pose is
    // advanced in first person too (body turn, walk cycle, swing), so the
    // shadow they cast - see RecordShadowCasters - moves like everyone else's.
    g_localShadowValid = false;
    if (state->worldReady.load() && state->localGameMode.load() != 3)
    {
        auto mob = g_mobModels.find("player");
        if (mob != g_mobModels.end())
        {
            float headYaw = state->player->camera.GetYaw();
            float deltaSeconds = std::min(std::chrono::duration<float>(now - g_localBodyTime).count(), 0.1f);
            g_localBodyTime = now;

            TickLoop::MovementSnapshot movement = state->tickLoop->GetMovementSnapshot();
            glm::vec2 intended = movement.intendedDirection;
            bool moving = glm::dot(intended, intended) > 1e-4f;
            float moveYaw = glm::degrees(std::atan2(intended.x, intended.y));

            float attackTime = state->GetLocalSwingProgress();
            bool swinging = attackTime > 0.0f || state->miningTicksRequired.load(std::memory_order_relaxed) > 0;

            float bodyYaw = g_localBody.Update(deltaSeconds, headYaw, moving, moveYaw, swinging);

            HumanoidDraw draw{
                state->tickLoop->GetRenderPosition(), glm::radians(bodyYaw),
                glm::vec4(hurtColor.r, hurtColor.g, hurtColor.b, 0.0f), &mob->second };

            EntityAnimation::WalkState& walk = g_walkStates[LOCAL_WALK_KEY];
            walk.Update(draw.position, now);
            walk.lastSeenFrame = g_walkFrame;
            draw.anim.walkPosition = walk.position;
            draw.anim.walkSpeed = walk.speed;
            draw.anim.attackTime = attackTime;
            draw.anim.ageTicks = ageTicks;
            draw.anim.headXRot = -glm::radians(state->player->camera.GetPitch());
            draw.anim.headYRot = -glm::radians(BodyRotation::Wrap(headYaw - bodyYaw));

            {
                std::lock_guard<std::mutex> lock(state->inventory.mutex);
                draw.mainhandItem = HeldItemName(state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot));
                draw.offhandItem = HeldItemName(state->inventory.offhand.Get(0));
                if (mob->second.armorOuter != nullptr)
                {
                    for (size_t i = 0; i < 4; i++) draw.armor[i] = HeldItemName(state->inventory.armor.Get(i));
                }
            }
            draw.anim.holdingMainhand = draw.mainhandItem != nullptr;
            draw.anim.holdingOffhand = draw.offhandItem != nullptr;

            // Posture comes from the local movement mode, not from the server.
            RideInfo ride = LocalRideInfo(state);
            draw.anim.riding = movement.mode == MoveMode::Ride || ride.riding;
            draw.anim.ridingBoat = draw.anim.riding && ride.boat;
            if (draw.anim.riding) draw.anim.posture = EntityAnimation::Posture::Riding;
            else if (movement.mode == MoveMode::SwimSprint)
            {
                draw.anim.posture = EntityAnimation::Posture::Swimming;
                draw.anim.swimAmount = movement.swimAmount;
            }
            else if (movement.mode == MoveMode::Swim) draw.anim.posture = EntityAnimation::Posture::Treading;
            else if (state->tickLoop->IsSneaking()) draw.anim.posture = EntityAnimation::Posture::Crouching;
            walk.posture = draw.anim.posture;
            walk.swimAmount = draw.anim.swimAmount;
            walk.headXRot = draw.anim.headXRot;
            walk.riding = draw.anim.riding;
            walk.ridingBoat = draw.anim.ridingBoat;
            g_localShadow = draw;
            g_localShadowValid = true;
            bool firstPerson = state->player->camera.IsFirstPerson();
            if (!firstPerson || std::get<bool>(state->config->Get("Graphics.BodyView", false)))
            {
                draw.hideHead = firstPerson;
                humanoidVisible.push_back(draw);
            }
        }
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
        Engine::Vulkan::VulkanRenderEngine* shadowVulkan = state->renderEngine != nullptr ? state->renderEngine->GetVulkan() : nullptr;
        const bool flatShadowed = shadowVulkan != nullptr && shadowVulkan->AreShadowsActive() && g_shadowedPipeline != VK_NULL_HANDLE;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flatShadowed ? g_shadowedPipeline : g_pipeline);

        VkDescriptorSet sets[] = { cameraSets[frameIndex], g_instanceSets[frameIndex],
            flatShadowed ? shadowVulkan->GetShadowSet() : VK_NULL_HANDLE };
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flatShadowed ? g_shadowedPipelineLayout : g_pipelineLayout,
            0, flatShadowed ? 3 : 2, sets, 0, nullptr);

        // Each shape is its own draw (different vertex/index buffer), but
        // still one indirect draw per shape rather than one per entity -
        // the batching this pipeline exists for.
        VkDeviceSize vertexOffset = 0;
        if (cubeInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_cubeVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_CUBE * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            if (state->debugHudVisible) state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += (g_cubeIndexCount / 3) * cubeInstanceCount;
        }
        if (quadrupedInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_quadrupedVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_quadrupedIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_QUADRUPED * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            if (state->debugHudVisible) state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += (g_quadrupedIndexCount / 3) * quadrupedInstanceCount;
        }
        if (golemInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_golemVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_golemIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_GOLEM * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            if (state->debugHudVisible) state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += (g_golemIndexCount / 3) * golemInstanceCount;
        }
        if (pigInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_pigVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_pigIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_PIG * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            if (state->debugHudVisible) state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += (g_pigIndexCount / 3) * pigInstanceCount;
        }
        if (cowInstanceCount > 0)
        {
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_cowVertexBuffer.buffer, &vertexOffset);
            vkCmdBindIndexBuffer(cmd, g_cowIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexedIndirect(cmd, g_indirectBuffers[frameIndex].buffer,
                SHAPE_COW * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
            if (state->debugHudVisible) state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += (g_cowIndexCount / 3) * cowInstanceCount;
        }
    }

    // Textured path: every part of every textured entity (and block-state
    // entity) goes into one batch, grouped by mesh and skin, so a crowd of
    // zombies is a handful of instanced draws. Position and yaw come straight
    // from the same InterpolatedPosition/InterpolatedYaw every other entity
    // uses, sampled fresh every frame right here on the render thread. See
    // EntityJsonModel.hpp's own comment on where a future animated pose
    // should be sampled from (here, not TickLoop).
    {
        // Block entities: the shared cube mesh with each block state's own
        // face-strip texture.
        for (const BlockDraw& draw : blockVisible)
        {
            const SkinTexture* texture = BlockTextureFor(draw.stateId);
            if (texture == nullptr) continue;

            TexturedInstance instance{};
            instance.positionYaw = glm::vec4(draw.position, 0.0f);
            instance.hurtColor = draw.hurtColor;
            instance.partOffset = glm::vec4(0.0f, 0.0f, 0.0f, EntityLight(state, draw.position));
            AddTextured(BATCH_MOBS, g_blockMesh, texture->descriptorSet, g_blockMesh.indexCount, 0, instance);
        }

        for (const HumanoidDraw& draw : humanoidVisible)
        {
            // Mesh and skin differ per layer: species differ in both.
            TexturedInstance instance{};
            instance.positionYaw = glm::vec4(draw.position, draw.yaw);
            instance.hurtColor = draw.hurtColor;
            const float light = EntityLight(state, draw.position);
            for (const MobLayer& layer : draw.model->layers)
            {
                const EntityModelMesh& mesh = *layer.mesh;

                // One draw per part, each turned about its own pivot.
                EntityAnimation::Pose pose = EntityAnimation::ComputePose(mesh.roles, mesh.parts, draw.anim);
                for (size_t part = 0; part < mesh.parts.size() && part < EntityAnimation::MAX_PARTS; part++)
                {
                    if (draw.hideHead && static_cast<int>(part) == mesh.roles.head) continue;
                    const EntityJsonModel::PartInfo& info = mesh.parts[part];
                    instance.partPivot = glm::vec4(info.pivot, 0.0f);
                    instance.partOffset = glm::vec4(ToWorldOffset(pose[part].offset, mesh.scale), light);
                    instance.partRotation = glm::vec4(ToWorldRotation(pose[part].rotation), 0.0f);
                    AddTextured(BATCH_MOBS, mesh, layer.skin->descriptorSet, info.indexCount, info.firstIndex, instance);
                }
                if (mesh.parts.empty())
                {
                    instance.partPivot = instance.partRotation = glm::vec4(0.0f);
                    instance.partOffset = glm::vec4(0.0f, 0.0f, 0.0f, light);
                    AddTextured(BATCH_MOBS, mesh, layer.skin->descriptorSet, mesh.indexCount, 0, instance);
                }
            }

            // Worn armor: bigger copies of the body parts, held in the body's own pose.
            if (draw.model->armorOuter != nullptr)
            {
                const EntityModelMesh& body = *draw.model->layers.front().mesh;
                EntityAnimation::Pose bodyPose = EntityAnimation::ComputePose(body.roles, body.parts, draw.anim);
                for (int slot = 0; slot < 4; slot++)
                {
                    const std::string* worn = draw.armor[static_cast<size_t>(slot)];
                    ArmorLook look;
                    if (worn == nullptr || (draw.hideHead && slot == 0) || !ArmorLookOf(*worn, slot, look)) continue;
                    const EntityModelMesh& armorMesh = slot == 2 ? *draw.model->armorInner : *draw.model->armorOuter;
                    if (armorMesh.parts.size() < 6 || body.parts.size() < 6) continue;

                    for (int pass = 0; pass < (look.overlayTexture.empty() ? 1 : 2); pass++)
                    {
                        const SkinTexture* skin = ArmorSkin(pass == 0 ? look.texture : look.overlayTexture);
                        if (skin == nullptr) continue;
                        for (int part : ARMOR_PARTS[static_cast<size_t>(slot)])
                        {
                            if (part < 0) continue;
                            if (draw.hideHead && slot == 1 && part >= 2) continue; // Shoulders fill the view.
                            size_t index = static_cast<size_t>(part);
                            const EntityJsonModel::PartInfo& info = armorMesh.parts[index];
                            instance.partPivot = glm::vec4(info.pivot, pass == 0 ? static_cast<float>(look.tint) : 0.0f);
                            instance.partOffset = glm::vec4(ToWorldOffset(bodyPose[index].offset, body.scale), light);
                            instance.partRotation = glm::vec4(ToWorldRotation(bodyPose[index].rotation), 0.0f);
                            AddTextured(BATCH_MOBS, armorMesh, skin->descriptorSet, info.indexCount, info.firstIndex, instance);
                        }
                    }
                }
            }
        }

        // The flush needs the shadow map's availability from the engine.
        Engine::Vulkan::VulkanRenderEngine* vulkan = state->renderEngine != nullptr ? state->renderEngine->GetVulkan() : nullptr;
        uint64_t triangles = FlushTextured(BATCH_MOBS, cmd, frameIndex, vulkan, indirect);
        if (state->debugHudVisible) state->trianglesDrawn[GlobalState::TRIANGLES_ENTITY] += triangles;
    }

    // Held items ride on the arm parts, so they follow the walk and swing.
    if (state->itemRenderer != nullptr)
    {
        glm::mat4 viewProj = proj * view;
        for (const HumanoidDraw& draw : humanoidVisible)
        {
            if (draw.mainhandItem == nullptr && draw.offhandItem == nullptr) continue;

            const EntityModelMesh& mesh = *draw.model->layers.front().mesh;
            if (mesh.parts.empty()) continue;
            EntityAnimation::Pose pose = EntityAnimation::ComputePose(mesh.roles, mesh.parts, draw.anim);

            // Entity placement, then vanilla's Y-down model frame (a half turn about X) in blocks.
            glm::mat4 placement = glm::translate(glm::mat4(1.0f), draw.position);
            placement = glm::rotate(placement, draw.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
            placement = glm::translate(placement, glm::vec3(0.0f, mesh.feetHeight, 0.0f));
            placement = glm::scale(placement, glm::vec3(mesh.scale, -mesh.scale, -mesh.scale));

            auto drawHand = [&](int armPart, bool left, const std::string* item)
            {
                if (item == nullptr || armPart < 0 || !state->itemRenderer->HasModel(*item)) return;
                size_t index = static_cast<size_t>(armPart);

                // ItemInHandLayer: stand the item up in the hand.
                glm::mat4 m = placement * EntityAnimation::PartMatrix(mesh.parts[index], pose[index]);
                m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
                m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
                m = glm::translate(m, glm::vec3(left ? -1.0f / 16.0f : 1.0f / 16.0f, 0.125f, -0.625f));
                state->itemRenderer->RecordItem(cmd, *item,
                    left ? "thirdperson_lefthand" : "thirdperson_righthand", m, viewProj);
            };
            drawHand(mesh.roles.rightArm, false, draw.mainhandItem);
            drawHand(mesh.roles.leftArm, true, draw.offhandItem);
        }
    }
}


uint64_t EntityRenderer::RecordBlockEntities(VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<ModeledBEDraw>& draws,
    const glm::mat4& viewProj, bool indirect, Engine::Vulkan::VulkanRenderEngine* vulkan)
{
    if (draws.empty() || g_humanoidPipeline == VK_NULL_HANDLE) return 0;

    std::array<glm::vec4, 4> frustumPlanes = ExtractFrustumPlanes(viewProj);
    for (const ModeledBEDraw& draw : draws)
    {
        BlockEntityModel model = LookupBlockEntityModel(draw.kind, draw.type);
        if (model.mesh == nullptr) continue;

        // A block's model fits a sphere of about a block's radius.
        if (!SphereInFrustum(frustumPlanes, draw.position + glm::vec3(0.0f, 0.5f, 0.0f), 1.0f)) continue;

        TexturedInstance instance{};
        instance.positionYaw = glm::vec4(draw.position, draw.yaw);
        AddTextured(BATCH_BLOCK_ENTITIES, *model.mesh, model.skin->descriptorSet, model.mesh->indexCount, 0, instance);
    }
    return FlushTextured(BATCH_BLOCK_ENTITIES, cmd, frameIndex, vulkan, indirect);
}


// ---- Shadow casting -------------------------------------------------------

void EntityRenderer::CreateShadowPipelines(const Engine::ShaderLibrary& shaders, VkRenderPass shadowRenderPass,
    VkPipelineLayout layout, ShadowPipelines& out)
{
    VkDevice dev = GetDevice();
    VkShaderModule vertModule = CreateShaderModule(shaders, "shadow_entity.vert");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    stage.module = vertModule;
    stage.pName = "main";

    // Position is the first attribute of both vertex formats, so the two
    // pipelines differ only in stride. Binding 1 carries one model matrix per
    // instance, as four vec4 columns at locations 1-4.
    VkVertexInputAttributeDescription attributes[5] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 },
        { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32 },
        { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48 } };

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

    // Both faces cast, with the same depth bias as the terrain casters.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor = 1.75f;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;

    auto build = [&](uint32_t stride) -> VkPipeline
    {
        VkVertexInputBindingDescription bindings[2] = {
            { 0, stride, VK_VERTEX_INPUT_RATE_VERTEX },
            { 1, sizeof(glm::mat4), VK_VERTEX_INPUT_RATE_INSTANCE } };
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 2;
        vertexInput.pVertexBindingDescriptions = bindings;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.stageCount = 1;
        info.pStages = &stage;
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &inputAssembly;
        info.pViewportState = &viewportState;
        info.pRasterizationState = &rasterizer;
        info.pMultisampleState = &multisampling;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlending;
        info.pDynamicState = &dynamicState;
        info.layout = layout;
        info.renderPass = shadowRenderPass;
        info.subpass = 0;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) return VK_NULL_HANDLE;
        return pipeline;
    };

    out.textured = build(sizeof(EntityJsonModel::Vertex));
    out.box = build(sizeof(EntityVertex));
    vkDestroyShaderModule(dev, vertModule, nullptr);

    if (out.textured == VK_NULL_HANDLE || out.box == VK_NULL_HANDLE)
    {
        DestroyShadowPipelines(out);
        throw std::runtime_error("[ERROR] Failed to create entity shadow pipelines.");
    }
}

void EntityRenderer::DestroyShadowPipelines(ShadowPipelines& pipelines)
{
    VkDevice dev = GetDevice();
    if (pipelines.textured != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipelines.textured, nullptr);
    if (pipelines.box != VK_NULL_HANDLE) vkDestroyPipeline(dev, pipelines.box, nullptr);
    pipelines = ShadowPipelines{};
}

void EntityRenderer::RecordShadowCasters(VkCommandBuffer cmd, VkPipelineLayout layout, const ShadowPipelines& pipelines,
    GlobalState* state, const glm::mat4& lightViewProj, const glm::vec3& cameraPosition, float distance,
    uint32_t frameIndex, uint32_t cascade, bool indirect)
{
    if (pipelines.textured == VK_NULL_HANDLE || pipelines.box == VK_NULL_HANDLE) return;
    const uint32_t slot = frameIndex % FRAMES_IN_FLIGHT;
    if (cascade >= SHADOW_CASCADE_SLOTS || g_shadowCasters.instancesMapped[slot][cascade] == nullptr) return;

    // A mob's shadow only reads at close range; further out the cost isn't worth the pixels.
    distance = std::min(distance, MAX_ENTITY_SHADOW_DISTANCE);

    auto now = std::chrono::steady_clock::now();
    const float rangeSquared = (distance + 16.0f) * (distance + 16.0f);
    g_shadowTextured.clear();
    g_shadowBoxes.clear();

    // How much one block spans in the light's clip space, to pad the box test.
    const float clipPerBlock = glm::length(glm::vec3(lightViewProj[0][0], lightViewProj[1][0], lightViewProj[2][0]));
    auto inLightBox = [&](const glm::vec3& center, float radius)
    {
        glm::vec3 offset = center - cameraPosition;
        if (glm::dot(offset, offset) > rangeSquared) return false;
        glm::vec4 clip = lightViewProj * glm::vec4(center, 1.0f);
        float margin = 1.0f + radius * clipPerBlock + 0.05f;
        return std::abs(clip.x) <= margin && std::abs(clip.y) <= margin;
    };

    // One animated mob: every part turned about its pivot, as the real draw does.
    // Parts are collected, then drawn batched below.
    auto drawMob = [&](const EntityModelMesh& mesh, const glm::vec3& position, float yaw, const EntityAnimation::Inputs& anim)
    {
        glm::mat4 entityMatrix = glm::rotate(glm::translate(glm::mat4(1.0f), position), yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        if (mesh.parts.empty())
        {
            g_shadowTextured.push_back({ &mesh, mesh.indexCount, 0, entityMatrix });
            return;
        }

        EntityAnimation::Pose pose = EntityAnimation::ComputePose(mesh.roles, mesh.parts, anim);
        for (size_t part = 0; part < mesh.parts.size() && part < EntityAnimation::MAX_PARTS; part++)
        {
            const EntityJsonModel::PartInfo& info = mesh.parts[part];
            glm::vec3 rotation = ToWorldRotation(pose[part].rotation);

            // The shader's local = R * (p - pivot) + pivot + offset, as a matrix.
            glm::mat4 partMatrix = glm::translate(entityMatrix, info.pivot + ToWorldOffset(pose[part].offset, mesh.scale));
            partMatrix = glm::rotate(partMatrix, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
            partMatrix = glm::rotate(partMatrix, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
            partMatrix = glm::rotate(partMatrix, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
            partMatrix = glm::translate(partMatrix, -info.pivot);

            g_shadowTextured.push_back({ &mesh, info.indexCount, info.firstIndex, partMatrix });
        }
    };

    // Walk and swing as the real draw last saw them (read only: updating the
    // walk state here would double-step it).
    auto animationFor = [&](uint32_t key, const Entity* entity)
    {
        EntityAnimation::Inputs anim;
        if (auto it = g_walkStates.find(key); it != g_walkStates.end())
        {
            anim.walkPosition = it->second.position;
            anim.walkSpeed = it->second.speed;
            anim.posture = it->second.posture;
            anim.swimAmount = it->second.swimAmount;
            anim.headXRot = it->second.headXRot;
            anim.riding = it->second.riding;
            anim.ridingBoat = it->second.ridingBoat;
        }
        if (entity != nullptr && entity->lastSwingTime != std::chrono::steady_clock::time_point{})
        {
            float sinceSwing = std::chrono::duration<float>(now - entity->lastSwingTime).count();
            if (sinceSwing < EntityAnimation::SWING_SECONDS) anim.attackTime = sinceSwing / EntityAnimation::SWING_SECONDS;
        }
        return anim;
    };

    std::lock_guard<std::mutex> lock(state->entitiesMutex);

    // Pass 1: textured mobs and players.
    state->ForEachEntity([&](uint32_t id, const Entity& entity)
    {
        if (!entity.visible || id == static_cast<uint32_t>(state->localEntityId.load())) return;
        const PlayerEntity* player = state->FindPlayerEntity(id);
        if (player != nullptr && (player->invisible || player->IsSpectator())) return;

        const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
        if (info == nullptr) return;
        auto mob = g_mobModels.find(info->name);
        if (mob == g_mobModels.end()) return;

        glm::vec3 position = RenderPositionOf(state, entity, now);
        float height = std::max(entity.boundingBox.y, 1.0f);
        if (!inLightBox(position + glm::vec3(0.0f, height * 0.5f, 0.0f), height)) return;

        drawMob(*mob->second.layers.front().mesh, position, entity.InterpolatedYaw(now), animationFor(id, &entity));
    });

    // The local player is no entity of the world's: they cast in any perspective,
    // with the same pose the third-person draw uses (RecordDraw keeps it current).
    if (g_localShadowValid && inLightBox(g_localShadow.position + glm::vec3(0.0f, 0.9f, 0.0f), 1.8f))
    {
        drawMob(*g_localShadow.model->layers.front().mesh, g_localShadow.position, g_localShadow.yaw, g_localShadow.anim);
    }

    // Pass 2: a box the size of the hitbox for entities with no model.
    state->ForEachEntity([&](uint32_t id, const Entity& entity)
    {
        if (!entity.visible || entity.itemId != 0 || id == static_cast<uint32_t>(state->localEntityId.load())) return;
        if (entity.boundingBox.x < 0.25f && entity.boundingBox.y < 0.25f) return; // Arrows, drops and the like.
        if (state->FindPlayerEntity(id) != nullptr) return;

        const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
        if (info != nullptr && g_mobModels.find(info->name) != g_mobModels.end()) return;

        glm::vec3 position = entity.InterpolatedPosition(now);
        float width = entity.boundingBox.x, height = entity.boundingBox.y;
        glm::vec3 center = position + glm::vec3(0.0f, height * 0.5f, 0.0f);
        if (!inLightBox(center, std::max(width, height))) return;

        glm::mat4 model = glm::rotate(glm::translate(glm::mat4(1.0f), center), entity.InterpolatedYaw(now), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(width, height, width));
        g_shadowBoxes.push_back({ nullptr, g_cubeIndexCount, 0, model });
    });

    // Draw: parts sharing a mesh and index range are one instanced command,
    // and commands sharing a mesh are one indirect draw. The light matrix is a
    // push constant for both lists; each instance carries its own model matrix.
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lightViewProj), &lightViewProj);

    auto* matrices = static_cast<glm::mat4*>(g_shadowCasters.instancesMapped[slot][cascade]);
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(g_shadowCasters.commandsMapped[slot][cascade]);
    const VkBuffer instanceBuffer = g_shadowCasters.instances[slot][cascade].buffer;
    const VkBuffer commandBuffer = g_shadowCasters.commands[slot][cascade].buffer;
    const bool useIndirect = indirect && indirectDrawSupported;
    constexpr uint32_t STRIDE = sizeof(VkDrawIndexedIndirectCommand);
    uint32_t instanceCursor = 0;
    uint32_t commandCursor = 0;

    auto submit = [&](std::vector<ShadowItem>& items, VkPipeline pipeline)
    {
        if (items.empty() || instanceCursor >= MAX_SHADOW_INSTANCES) return;
        if (instanceCursor + items.size() > MAX_SHADOW_INSTANCES) items.resize(MAX_SHADOW_INSTANCES - instanceCursor);

        std::sort(items.begin(), items.end(), [](const ShadowItem& a, const ShadowItem& b)
        {
            if (a.mesh != b.mesh) return std::less<const EntityModelMesh*>()(a.mesh, b.mesh);
            if (a.firstIndex != b.firstIndex) return a.firstIndex < b.firstIndex;
            return a.indexCount < b.indexCount;
        });

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 1, 1, &instanceBuffer, &zero);

        // Same-mesh commands are contiguous, so a run is one buffer pair and one draw.
        size_t i = 0;
        while (i < items.size())
        {
            const EntityModelMesh* mesh = items[i].mesh;
            const uint32_t runFirst = commandCursor;
            while (i < items.size() && items[i].mesh == mesh)
            {
                const ShadowItem& first = items[i];
                size_t j = i;
                const uint32_t firstInstance = instanceCursor;
                while (j < items.size() && items[j].mesh == mesh && items[j].firstIndex == first.firstIndex
                    && items[j].indexCount == first.indexCount)
                {
                    matrices[instanceCursor++] = items[j].model;
                    j++;
                }
                commands[commandCursor++] = { first.indexCount, static_cast<uint32_t>(j - i), first.firstIndex, 0, firstInstance };
                i = j;
            }

            const VkBuffer vertexBuffer = mesh != nullptr ? mesh->vertexBuffer.buffer : g_cubeVertexBuffer.buffer;
            const VkBuffer indexBuffer = mesh != nullptr ? mesh->indexBuffer.buffer : g_cubeIndexBuffer.buffer;
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &zero);
            vkCmdBindIndexBuffer(cmd, indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            if (useIndirect)
            {
                vkCmdDrawIndexedIndirect(cmd, commandBuffer, runFirst * STRIDE, commandCursor - runFirst, STRIDE);
            }
            else
            {
                for (uint32_t k = runFirst; k < commandCursor; k++)
                {
                    vkCmdDrawIndexed(cmd, commands[k].indexCount, commands[k].instanceCount, commands[k].firstIndex, 0,
                        commands[k].firstInstance);
                }
            }
        }
    };
    submit(g_shadowTextured, pipelines.textured);
    submit(g_shadowBoxes, pipelines.box);

    if (instanceCursor > 0)
    {
        vmaFlushAllocation(vmaAllocator, g_shadowCasters.instances[slot][cascade].allocation, 0, sizeof(glm::mat4) * instanceCursor);
        vmaFlushAllocation(vmaAllocator, g_shadowCasters.commands[slot][cascade].allocation, 0, STRIDE * commandCursor);
    }
}

} // namespace Volcano
