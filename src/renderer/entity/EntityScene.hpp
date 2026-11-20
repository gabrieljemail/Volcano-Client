#pragma once
#ifndef VOLCANO_ENTITY_SCENE_H
#define VOLCANO_ENTITY_SCENE_H

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "models/EntityAnimation.hpp"
#include "models/EntityJsonModel.hpp"

namespace Volcano {

struct Application;
struct ItemStack;
struct Entity;

// The backend-independent half of entity rendering: which entities are drawn
// this frame, where, in what pose and with which model, skin and armor -
// everything up to (but not including) talking to the GPU. The Vulkan
// EntityRenderer and the Direct3D 11 entity pass both consume a Frame built
// here, so culling, animation and armor layout exist once.
//
// What stays per backend: loading and owning meshes/skins (the *Base structs
// below are what the backend's own types derive from), instance buffers,
// pipelines, shadows and the draw calls themselves.
namespace EntityScene {

// Opaque to this layer: the backend derives its GPU mesh/skin from these and
// casts back when it draws. Deliberately not virtual, so the derived types
// stay plain aggregates.
struct SkinBase {};

struct MeshBase {
    uint32_t indexCount = 0;
    float scale = 1.0f;
    float feetHeight = 1.5f; // Ground level in the model frame, in blocks.
    std::vector<EntityJsonModel::PartInfo> parts; // Empty for meshes that don't animate.
    EntityAnimation::Roles roles;
};

// Flat-tinted procedural shapes (entity.vert), in the order backends slot them.
enum FlatShape : uint32_t { FLAT_CUBE, FLAT_QUADRUPED, FLAT_GOLEM, FLAT_PIG, FLAT_COW, FLAT_COUNT };

// Maximum entities drawn in a single frame (closest first, after frustum culling).
constexpr uint32_t MAX_VISIBLE_ENTITIES = 256;

// Key under which the local player's own skin is stored.
constexpr const char* LOCAL_SKIN_KEY = "local";

// std430 layout of entity.vert's EntityInstance: 4 vec4s, 16-byte aligned throughout.
struct GPUEntityInstance {
    glm::vec4 positionYaw; // xyz: world position (feet), w: yaw radians.
    glm::vec4 halfExtents; // xyz: half bounding-box extents, w: unused.
    glm::vec4 color;       // rgb: base tint, a: unused.
    glm::vec4 hurtColor;   // rgb: hit-flash color, a: blend amount (0 when not hurt).
};

// One record per drawn part: entity_textured.vert reads it as instance-rate
// vertex attributes, so a part, an entity or a hundred of them are all the
// same kind of draw.
struct TexturedInstance {
    glm::vec4 positionYaw;
    glm::vec4 hurtColor;    // Same convention as GPUEntityInstance::hurtColor.
    glm::vec4 partPivot;    // xyz: point the part turns about, model-local; w: tint as 0xRRGGBB (0 for none).
    glm::vec4 partOffset;   // xyz: translation added after the turn, model-local; w: light, sky * 16 + block.
    glm::vec4 partRotation; // xyz: radians, applied Z * Y * X; w: death roll of the whole body.
    glm::vec4 bodyPose;     // x: pitch of the whole body, radians, nose down positive.
};

struct MobLayer {
    const MeshBase* mesh;
    const SkinBase* skin;
};

// One textured mob from mobs.json: base layer first, then overlays.
struct MobModel {
    std::vector<MobLayer> layers;
    // Armor meshes fitted to this body (same parts, same pivots), or null for a mob that wears none.
    const MeshBase* armorOuter = nullptr; // Helmet, chestplate, boots.
    const MeshBase* armorInner = nullptr; // Leggings.
    int pitchMode = 0; // 1: the entity's look pitch tilts the body (nose down positive); -1: a projectile, pitch up positive.
};

// One humanoid entity's fully-resolved draw parameters, snapshotted under
// entitiesMutex. Deliberately NOT a `const Entity*`: the draw loop runs after
// the lock drops, and the network thread can erase an entity in that window.
// The model/skin pointers aim at backend caches that outlive every frame.
struct HumanoidDraw {
    glm::vec3 position;
    float yaw;
    glm::vec4 hurtColor;
    const MobModel* model;
    EntityAnimation::Inputs anim;
    const std::string* mainhandItem = nullptr; // Registry names, which outlive the frame.
    const std::string* offhandItem = nullptr;
    std::array<const std::string*, 4> armor{}; // Head, chest, legs, feet.
    uint8_t armorGlint = 0; // Bit per armor slot that is enchanted.
    bool mainhandGlint = false;
    bool offhandGlint = false;
    bool hideHead = false; // First-person body view: the camera sits inside the head.
    float deathRoll = 0.0f; // Radians the body has tipped over; 0 while alive.
    float pitch = 0.0f;     // Radians the whole body tilts nose down.
    const SkinBase* skin = nullptr; // A player's own skin, for every layer; null uses the model's.
};

// A block-shaped entity (falling_block, tnt), snapshotted like HumanoidDraw.
struct BlockDraw {
    glm::vec3 position;
    glm::vec4 hurtColor;
    int32_t stateId;
};

// What one frame draws.
struct Frame {
    std::array<std::vector<GPUEntityInstance>, FLAT_COUNT> flat; // By FlatShape.
    std::vector<HumanoidDraw> humanoids; // Includes the local player in third person.
    std::vector<BlockDraw> blocks;
};

// The lookups that depend on how a backend loads and stores its assets.
class Assets {
public:
    virtual ~Assets() = default;

    // The textured model for an entity type name (mobs.json), or null.
    virtual const MobModel* FindMob(const std::string& entityName) const = 0;

    // Swaps in a player's own skin and slim model when downloaded; leaves both
    // untouched otherwise (and requests the download).
    virtual void ResolvePlayerLook(Application* state, const std::string& key,
        const MobModel*& model, const SkinBase*& skin) = 0;

    // An armor sheet by pack-relative path, or null when the packs have none.
    virtual const SkinBase* ArmorSkin(const std::string& path) = 0;

    // The armor enchantment glint sheet, or null when the packs have none.
    virtual const SkinBase* ArmorGlintSkin() = 0;
};

// Frustum-culls Application's entities, keeps the closest MAX_VISIBLE_ENTITIES,
// poses everything, and fills `out` (which it clears). Also advances the walk
// cycles and, when the local player exists in third person (or with the body
// view on), adds their body to `humanoids`. Call once per rendered frame.
void Collect(Application* state, const glm::mat4& view, const glm::mat4& proj,
    const glm::vec3& cameraPosition, Assets& assets, Frame& out);

enum class Batch { Mobs, Glint };

// Receives one drawn part: the mesh and skin to draw with, the index range
// and the per-part instance record.
using TexturedSink = std::function<void(Batch batch, const MeshBase& mesh, const SkinBase& skin,
    uint32_t indexCount, uint32_t firstIndex, const TexturedInstance& instance)>;

// Turns every humanoid in `frame` into per-part instances: each layer's parts
// posed about their pivots, then worn armor (and its glint) in the body's pose.
void ExpandTextured(Application* state, const Frame& frame, Assets& assets, const TexturedSink& sink);

// An item held by a posed humanoid: it rides on the arm part, so it follows the
// walk and swing. `placement` is the item's model matrix before its display
// transform (ItemDraw::MakeWorld applies `context`'s).
struct HeldItemDraw {
    const std::string* item; // Registry name, which outlives the frame.
    const char* context;     // "thirdperson_righthand" / "thirdperson_lefthand".
    glm::mat4 placement;
    bool glint;
};

// The held items of every humanoid in `frame`, posed like ExpandTextured's parts.
void CollectHeldItems(const Frame& frame, std::vector<HeldItemDraw>& out);

// A dropped item (spinning and bobbing) or a thrown one (facing the camera),
// drawn with the item renderer in the "ground" context.
struct WorldItemDraw {
    std::string item; // Registry name.
    glm::mat4 placement;
};

// The item entities within range (and, with Graphics.EntityOcclusionCulling,
// not hidden behind terrain), each placed in the world. `time` is the scene
// time driving the spin and bob. Call once per rendered frame.
void CollectWorldItems(Application* state, const glm::mat4& view, const glm::vec3& cameraPosition,
    float time, std::vector<WorldItemDraw>& out);

// ---- Shared helpers (also used by backend-specific code) -----------------

// Left/Right/Bottom/Top frustum planes (Gribb/Hartmann) from a view-projection matrix.
std::array<glm::vec4, 4> ExtractFrustumPlanes(const glm::mat4& m);
bool SphereInFrustum(const std::array<glm::vec4, 4>& planes, const glm::vec3& center, float radius);

// Where a remote entity renders: its seat when riding, else its interpolated position.
// Call under entitiesMutex.
glm::vec3 RenderPositionOf(Application* state, const Entity& entity, std::chrono::steady_clock::time_point now);

// The chunk light at a point on an entity, packed sky * 16 + block for TexturedInstance.
float EntityLight(Application* state, const glm::vec3& feet);

// Vanilla's (rotation, offset) in model space -> this renderer's world-local space.
glm::vec3 ToWorldRotation(glm::vec3 vanilla);
glm::vec3 ToWorldOffset(glm::vec3 vanilla, float scale);

// Item registry name of a stack, or null for an empty hand.
const std::string* HeldItemName(const ItemStack& stack);

// Walk cycles per entity, keyed by network id; the local player uses LOCAL_WALK_KEY.
constexpr uint32_t LOCAL_WALK_KEY = 0xFFFFFFFFu;
std::unordered_map<uint32_t, EntityAnimation::WalkState>& WalkStates();

// The local player's pose as last posed by Collect, for the shadow pass
// (which records first in the frame). Valid only while they exist in the world.
HumanoidDraw& LocalShadow();
bool& LocalShadowValid();

} // namespace EntityScene

} // namespace Volcano

#endif
