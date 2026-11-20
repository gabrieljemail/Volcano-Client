#include "EntityScene.hpp"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <glm/gtc/matrix_transform.hpp>
#include "../../ApplicationDeps.hpp"
import Volcano;
#include "../../Logger.hpp"
#include "../../TickLoop.hpp"
#include "../../inventory/ItemRegistry.hpp"
#include "../../network/Uuid.hpp"
#include "../misc/ItemRenderer.hpp"
#include "../engine/d3d11/D3D11ItemRenderer.hpp"
#include "../terrain/OcclusionSnapshot.hpp"
#include "../terrain/models/World.hpp"
#include "models/BodyRotation.hpp"
#include "models/Entity.hpp"
#include "models/EntityRegistry.hpp"
#include "models/EntitySeat.hpp"

namespace Volcano::EntityScene {

namespace {

std::unordered_map<uint32_t, EntityAnimation::WalkState> g_walkStates;
uint64_t g_walkFrame = 0;

// The local player's body turn, advanced while they're drawn in third person.
BodyRotation g_localBody;
std::chrono::steady_clock::time_point g_localBodyTime = std::chrono::steady_clock::now();

HumanoidDraw g_localShadow{};
bool g_localShadowValid = false;

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

// Parts of the armor mesh each slot covers, by index (the models share humanoid.json's order).
constexpr std::array<std::array<int, 3>, 4> ARMOR_PARTS = {{ { 0, -1, -1 }, { 1, 2, 3 }, { 1, 4, 5 }, { 4, 5, -1 } }};

// Whether an entity sits on a vehicle, and whether that vehicle is a boat.
struct RideInfo {
    bool riding = false;
    bool boat = false;
};

// Whether an entity sits on a tracked vehicle, and the seat it renders at.
// Call under entitiesMutex.
RideInfo RideInfoFor(Application* state, const Entity& entity)
{
    glm::vec3 seat;
    float vehicleYaw = 0.0f;
    EntitySeat::Kind kind = EntitySeat::Resolve(state, entity, std::chrono::steady_clock::now(), seat, vehicleYaw);
    return RideInfo{ kind != EntitySeat::Kind::None, kind == EntitySeat::Kind::Boat };
}

// How far a dying body has tipped over, in radians, `ticks` after its death: vanilla's sqrt ramp up to 90 degrees.
float DeathRoll(float ticks)
{
    float f = std::sqrt(std::clamp((ticks - 1.0f) / 20.0f * 1.6f, 0.0f, 1.0f));
    return glm::radians(90.0f * f);
}

// The local player's vehicle kind, from the vehicle they ride.
RideInfo LocalRideInfo(Application* state)
{
    int32_t vehicleId = state->ridingEntityId.load();
    if (vehicleId < 0) return RideInfo{};
    std::lock_guard<std::mutex> lock(state->entitiesMutex);
    const Entity* vehicle = state->FindEntity(static_cast<uint32_t>(vehicleId));
    if (vehicle == nullptr) return RideInfo{ true, false };
    return RideInfo{ true, EntitySeat::KindOf(*vehicle) == EntitySeat::Kind::Boat };
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

} // namespace

// ---- Shared helpers ---------------------------------------------------------

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

glm::vec3 RenderPositionOf(Application* state, const Entity& entity, std::chrono::steady_clock::time_point now)
{
    glm::vec3 seat;
    float vehicleYaw = 0.0f;
    if (EntitySeat::Resolve(state, entity, now, seat, vehicleYaw) != EntitySeat::Kind::None) return seat;
    return entity.InterpolatedPosition(now);
}

float EntityLight(Application* state, const glm::vec3& feet)
{
    if (state->world == nullptr) return 15.0f * 16.0f;
    LightSample sample = state->world->GetLightSample(static_cast<int>(std::floor(feet.x)),
        static_cast<int>(std::floor(feet.y + 1.0f)), static_cast<int>(std::floor(feet.z)));
    return static_cast<float>(sample.sky) * 16.0f + static_cast<float>(sample.block);
}

glm::vec3 ToWorldRotation(glm::vec3 vanilla) { return glm::vec3(vanilla.x, -vanilla.y, -vanilla.z); }
glm::vec3 ToWorldOffset(glm::vec3 vanilla, float scale) { return glm::vec3(vanilla.x, -vanilla.y, -vanilla.z) * (scale / 16.0f); }

const std::string* HeldItemName(const ItemStack& stack)
{
    if (stack.IsEmpty()) return nullptr;
    const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(stack.itemId);
    return item != nullptr ? &item->name : nullptr;
}

std::unordered_map<uint32_t, EntityAnimation::WalkState>& WalkStates() { return g_walkStates; }
HumanoidDraw& LocalShadow() { return g_localShadow; }
bool& LocalShadowValid() { return g_localShadowValid; }

// ---- Collect ----------------------------------------------------------------

void Collect(Application* state, const glm::mat4& view, const glm::mat4& proj,
    const glm::vec3& cameraPosition, Assets& assets, Frame& out)
{
    for (auto& list : out.flat) list.clear();
    out.humanoids.clear();
    out.blocks.clear();

    std::array<glm::vec4, 4> frustumPlanes = ExtractFrustumPlanes(proj * view);

    // Frustum-cull first, then keep only the closest MAX_VISIBLE_ENTITIES -
    // the "simple entity limit" culling method from the entity renderer
    // plan, applied after frustum culling so it prioritizes what's actually
    // in view instead of arbitrarily dropping visible entities in favor of
    // off-screen ones.
    auto now = std::chrono::steady_clock::now();

    // Read per frame so a settings reload applies immediately. All four
    // channels are 0-255; alpha is how strongly the color replaces the model.
    // TODO: Graphics.EntityHurt.IncludeArmor
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

    // Wall-clock "age" in ticks for the idle arm sway.
    static const auto animationStart = std::chrono::steady_clock::now();
    float ageTicks = std::fmod(std::chrono::duration<float>(now - animationStart).count(), 3000.0f) * 20.0f;
    g_walkFrame++;
    if (g_walkFrame % 600 == 0)
    {
        std::erase_if(g_walkStates, [](const auto& entry) { return g_walkFrame - entry.second.lastSeenFrame > 300; });
    }

    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        visible.reserve(state->entities.size() + state->playerEntities.size());

        // Other players (playerEntities) render exactly like every other
        // humanoid
        state->ForEachEntity([&](uint32_t id, const Entity& entity)
        {
            if (!entity.visible) return;
            const PlayerEntity* player = state->FindPlayerEntity(id);
            if (player && player->invisible) return;

            // Dropped items with a 3D model are drawn by ItemPass instead.
            if (entity.itemId != 0 && (state->itemRenderer != nullptr || state->d3dItemRenderer != nullptr))
            {
                const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(entity.itemId);
                if (item != nullptr && (state->itemRenderer != nullptr ? state->itemRenderer->HasModel(item->name)
                                                                        : state->d3dItemRenderer->HasModel(item->name))) return;
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

        // Build the instance lists while still holding the lock, since
        // `visible` (and humanoids, built alongside it here) holds raw
        // pointers into state->entities/playerEntities. Split by
        // EntityRegistry::ShapeKind rather than one flat list
        out.flat[FLAT_CUBE].reserve(visible.size());
        for (const auto& [distanceSq, entity] : visible)
        {
            const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity->type);
            EntityRegistry::ShapeKind shape = info ? info->shape : EntityRegistry::ShapeKind::Cube;

            // Thrown items are drawn as their item by ItemPass.
            if (info != nullptr && EntityRegistry::ThrownItemName(info->name) != nullptr) continue;

            // Any type listed in mobs.json takes the textured path, whatever its shape.
            if (info != nullptr)
            {
                const MobModel* mob = assets.FindMob(info->name);
                if (mob != nullptr)
                {
                    HumanoidDraw draw{
                        RenderPositionOf(state, *entity, now), entity->InterpolatedYaw(now), HurtColorFor(*entity),
                        mob, {} };
                    if (const PlayerEntity* skinned = state->FindPlayerEntity(entity->id))
                    {
                        assets.ResolvePlayerLook(state, FormatUuid(skinned->uuid), draw.model, draw.skin);
                    }

                    if (mob->armorOuter != nullptr)
                    {
                        const EquipmentSlot slots[4] = { EquipmentSlot::Head, EquipmentSlot::Chest, EquipmentSlot::Legs, EquipmentSlot::Feet };
                        for (size_t i = 0; i < 4; i++)
                        {
                            const ItemStack& piece = entity->GetEquipment(slots[i]);
                            draw.armor[i] = HeldItemName(piece);
                            if (piece.glint) draw.armorGlint |= static_cast<uint8_t>(1u << i);
                        }
                    }

                    draw.pitch = static_cast<float>(mob->pitchMode) * entity->InterpolatedPitch(now);
                    if (entity->dead && entity->deathTime != std::chrono::steady_clock::time_point{})
                    {
                        draw.deathRoll = DeathRoll(std::chrono::duration<float>(now - entity->deathTime).count() * 20.0f);
                    }

                    EntityAnimation::WalkState& walk = g_walkStates[entity->id];
                    const glm::vec3 previousDrawPosition = walk.lastPosition;
                    const bool hadPosition = walk.valid;
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
                        draw.mainhandGlint = holder->GetEquipment(EquipmentSlot::MainHand).glint;
                        draw.offhandGlint = holder->GetEquipment(EquipmentSlot::OffHand).glint;
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

                    // Head and body turn apart, as for the local player: the head shows the look direction and the
                    // body trails it, following the way the entity walks.
                    if (mob->pitchMode == 0 && !entity->dead)
                    {
                        float headYawDeg = glm::degrees(entity->InterpolatedHeadYaw(now));
                        glm::vec3 step = draw.position - previousDrawPosition;
                        if (hadPosition && step.x * step.x + step.z * step.z > 1e-6f)
                        {
                            walk.moveYaw = glm::degrees(std::atan2(step.x, step.z));
                        }
                        bool moving = walk.speed > 0.05f;
                        bool swinging = draw.anim.attackTime > 0.0f;
                        float deltaSeconds = std::min(std::chrono::duration<float>(now - walk.bodyTime).count(), 0.1f);
                        walk.bodyTime = now;
                        float bodyYawDeg = walk.body.Update(deltaSeconds, headYawDeg, moving, walk.moveYaw, swinging);
                        if (ride.riding) bodyYawDeg = glm::degrees(draw.yaw); // Seated bodies face as the vehicle does.
                        draw.yaw = glm::radians(bodyYawDeg);
                        draw.anim.headYRot = -glm::radians(BodyRotation::Wrap(headYawDeg - bodyYawDeg));
                        draw.anim.headXRot = entity->InterpolatedPitch(now);
                        walk.headXRot = draw.anim.headXRot;
                        walk.headYRot = draw.anim.headYRot;
                        walk.bodyYaw = draw.yaw;
                        walk.bodyDrawn = true;
                    }
                    out.humanoids.push_back(draw);
                    continue;
                }
            }
            if (shape == EntityRegistry::ShapeKind::Humanoid) shape = EntityRegistry::ShapeKind::Cube;

            if (shape == EntityRegistry::ShapeKind::Block && entity->blockStateId >= 0)
            {
                out.blocks.push_back(BlockDraw{
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
                case EntityRegistry::ShapeKind::Quadruped: out.flat[FLAT_QUADRUPED].push_back(gpu); break;
                case EntityRegistry::ShapeKind::Golem: out.flat[FLAT_GOLEM].push_back(gpu); break;
                case EntityRegistry::ShapeKind::Pig: out.flat[FLAT_PIG].push_back(gpu); break;
                case EntityRegistry::ShapeKind::Cow: out.flat[FLAT_COW].push_back(gpu); break;
                default: out.flat[FLAT_CUBE].push_back(gpu); break;
            }
        }
    }

    // The local player has no entity of their own; draw them as a plain
    // "player" humanoid when the camera isn't in their head. The pose is
    // advanced in first person too (body turn, walk cycle, swing), so the
    // shadow they cast
    g_localShadowValid = false;
    if (state->worldReady.load() && state->localGameMode.load() != 3)
    {
        const MobModel* mob = assets.FindMob("player");
        if (mob != nullptr)
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
                glm::vec4(hurtColor.r, hurtColor.g, hurtColor.b, 0.0f), mob, {} };
            assets.ResolvePlayerLook(state, LOCAL_SKIN_KEY, draw.model, draw.skin);

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
                draw.mainhandGlint = state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot).glint;
                draw.offhandGlint = state->inventory.offhand.Get(0).glint;
                if (mob->armorOuter != nullptr)
                {
                    for (size_t i = 0; i < 4; i++)
                    {
                        draw.armor[i] = HeldItemName(state->inventory.armor.Get(i));
                        if (state->inventory.armor.Get(i).glint) draw.armorGlint |= static_cast<uint8_t>(1u << i);
                    }
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
            if (int64_t deathNs = state->localDeathNs.load(); deathNs != 0)
            {
                int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                draw.deathRoll = DeathRoll(static_cast<float>(nowNs - deathNs) * 1e-9f * 20.0f);
            }
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
                out.humanoids.push_back(draw);
            }
        }
    }
}

// ---- ExpandTextured -----------------------------------------------------------

void ExpandTextured(Application* state, const Frame& frame, Assets& assets, const TexturedSink& sink)
{
    for (const HumanoidDraw& draw : frame.humanoids)
    {
        // Mesh and skin differ per layer: species differ in both.
        TexturedInstance instance{};
        instance.positionYaw = glm::vec4(draw.position, draw.yaw);
        instance.hurtColor = draw.hurtColor;
        instance.bodyPose = glm::vec4(draw.pitch, 0.0f, 0.0f, 0.0f);
        const float light = EntityLight(state, draw.position);
        for (const MobLayer& layer : draw.model->layers)
        {
            const MeshBase& mesh = *layer.mesh;
            const SkinBase& layerSkin = *(draw.skin != nullptr ? draw.skin : layer.skin);

            // One draw per part, each turned about its own pivot.
            EntityAnimation::Pose pose = EntityAnimation::ComputePose(mesh.roles, mesh.parts, draw.anim);
            for (size_t part = 0; part < mesh.parts.size() && part < EntityAnimation::MAX_PARTS; part++)
            {
                if (draw.hideHead && static_cast<int>(part) == mesh.roles.head) continue;
                const EntityJsonModel::PartInfo& info = mesh.parts[part];
                instance.partPivot = glm::vec4(info.pivot, 0.0f);
                instance.partOffset = glm::vec4(ToWorldOffset(pose[part].offset, mesh.scale), light);
                instance.partRotation = glm::vec4(ToWorldRotation(pose[part].rotation), draw.deathRoll);
                sink(Batch::Mobs, mesh, layerSkin, info.indexCount, info.firstIndex, instance);
            }
            if (mesh.parts.empty())
            {
                instance.partPivot = glm::vec4(0.0f);
                instance.partRotation = glm::vec4(0.0f, 0.0f, 0.0f, draw.deathRoll);
                instance.partOffset = glm::vec4(0.0f, 0.0f, 0.0f, light);
                sink(Batch::Mobs, mesh, layerSkin, mesh.indexCount, 0, instance);
            }
        }

        // Worn armor: bigger copies of the body parts, held in the body's own pose.
        if (draw.model->armorOuter != nullptr)
        {
            const MeshBase& body = *draw.model->layers.front().mesh;
            EntityAnimation::Pose bodyPose = EntityAnimation::ComputePose(body.roles, body.parts, draw.anim);
            const SkinBase* glintSheet = draw.armorGlint != 0 ? assets.ArmorGlintSkin() : nullptr;
            for (int slot = 0; slot < 4; slot++)
            {
                const std::string* worn = draw.armor[static_cast<size_t>(slot)];
                ArmorLook look;
                if (worn == nullptr || (draw.hideHead && slot == 0) || !ArmorLookOf(*worn, slot, look)) continue;
                const MeshBase& armorMesh = slot == 2 ? *draw.model->armorInner : *draw.model->armorOuter;
                if (armorMesh.parts.size() < 6 || body.parts.size() < 6) continue;

                for (int pass = 0; pass < (look.overlayTexture.empty() ? 1 : 2); pass++)
                {
                    const SkinBase* skin = assets.ArmorSkin(pass == 0 ? look.texture : look.overlayTexture);
                    if (skin == nullptr) continue;
                    for (int part : ARMOR_PARTS[static_cast<size_t>(slot)])
                    {
                        if (part < 0) continue;
                        if (draw.hideHead && slot == 1 && part >= 2) continue; // Shoulders fill the view.
                        size_t index = static_cast<size_t>(part);
                        const EntityJsonModel::PartInfo& info = armorMesh.parts[index];
                        instance.partPivot = glm::vec4(info.pivot, pass == 0 ? static_cast<float>(look.tint) : 0.0f);
                        instance.partOffset = glm::vec4(ToWorldOffset(bodyPose[index].offset, body.scale), light);
                        instance.partRotation = glm::vec4(ToWorldRotation(bodyPose[index].rotation), draw.deathRoll);
                        sink(Batch::Mobs, armorMesh, *skin, info.indexCount, info.firstIndex, instance);
                        if (pass == 0 && glintSheet != nullptr && ((draw.armorGlint >> slot) & 1) != 0)
                        {
                            sink(Batch::Glint, armorMesh, *skin, info.indexCount, info.firstIndex, instance);
                        }
                    }
                }
            }
        }
    }
}

void CollectHeldItems(const Frame& frame, std::vector<HeldItemDraw>& out)
{
    out.clear();
    for (const HumanoidDraw& draw : frame.humanoids)
    {
        if (draw.mainhandItem == nullptr && draw.offhandItem == nullptr) continue;
        if (draw.model == nullptr || draw.model->layers.empty()) continue;

        const MeshBase& mesh = *draw.model->layers.front().mesh;
        if (mesh.parts.empty()) continue;
        EntityAnimation::Pose pose = EntityAnimation::ComputePose(mesh.roles, mesh.parts, draw.anim);

        // Entity placement, then vanilla's Y-down model frame (a half turn about X) in blocks.
        glm::mat4 placement = glm::translate(glm::mat4(1.0f), draw.position);
        placement = glm::rotate(placement, draw.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        placement = glm::translate(placement, glm::vec3(0.0f, mesh.feetHeight, 0.0f));
        placement = glm::scale(placement, glm::vec3(mesh.scale, -mesh.scale, -mesh.scale));

        auto addHand = [&](int armPart, bool left, const std::string* item, bool glint)
        {
            if (item == nullptr || armPart < 0) return;
            size_t index = static_cast<size_t>(armPart);

            // ItemInHandLayer: stand the item up in the hand.
            glm::mat4 m = placement * EntityAnimation::PartMatrix(mesh.parts[index], pose[index]);
            m = glm::rotate(m, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            m = glm::rotate(m, glm::radians(180.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            m = glm::translate(m, glm::vec3(left ? -1.0f / 16.0f : 1.0f / 16.0f, 0.125f, -0.625f));
            out.push_back({ item, left ? "thirdperson_lefthand" : "thirdperson_righthand", m, glint });
        };
        addHand(mesh.roles.rightArm, false, draw.mainhandItem, draw.mainhandGlint);
        addHand(mesh.roles.leftArm, true, draw.offhandItem, draw.offhandGlint);
    }
}

void CollectWorldItems(Application* state, const glm::mat4& view, const glm::vec3& cameraPosition,
    float time, std::vector<WorldItemDraw>& out)
{
    constexpr float MAX_DISTANCE = 64.0f;
    out.clear();

    struct Drop
    {
        glm::vec3 position;
        int32_t itemId;
        uint32_t id;
    };
    std::vector<Drop> drops;
    struct Thrown
    {
        glm::vec3 position;
        const char* item;
        float height;
    };
    std::vector<Thrown> thrown;
    auto now = std::chrono::steady_clock::now();
    bool occlusionCulling = std::get<bool>(state->config->Get("Graphics.EntityOcclusionCulling", false));
    {
        std::lock_guard<std::mutex> lock(state->entitiesMutex);
        state->ForEachEntity([&](uint32_t id, const Entity& entity)
        {
            const EntityRegistry::EntityTypeInfo* info = EntityRegistry::Lookup(entity.type);
            const char* thrownItem = info != nullptr ? EntityRegistry::ThrownItemName(info->name) : nullptr;
            if ((entity.itemId == 0 && thrownItem == nullptr) || !entity.visible) return;
            glm::vec3 position = entity.InterpolatedPosition(now);
            if (glm::distance(position, cameraPosition) > MAX_DISTANCE) return;
            if (occlusionCulling && occlusionSnapshot.IsEntityOccluded(position, entity.boundingBox.x, entity.boundingBox.y)) return;
            if (thrownItem != nullptr)
            {
                thrown.push_back({ position, thrownItem, entity.boundingBox.y });
                return;
            }
            drops.push_back({ position, entity.itemId, id });
        });
    }

    // Thrown items face the camera: the inverse of the view's rotation turns the sprite to look back down the ray.
    glm::mat4 facing(glm::transpose(glm::mat3(view)));
    for (const Thrown& item : thrown)
    {
        glm::mat4 placement = glm::translate(glm::mat4(1.0f), item.position + glm::vec3(0.0f, item.height * 0.5f, 0.0f)) * facing;
        out.push_back({ item.item, placement });
    }

    for (const Drop& drop : drops)
    {
        const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(drop.itemId);
        if (item == nullptr) continue;

        // Vanilla's bob (0.1 amplitude, ~3s period) and 1 rad/s spin, offset per entity.
        float phase = static_cast<float>(drop.id % 64) * 0.7f;
        float bob = std::sin(time * 2.0f + phase) * 0.1f + 0.1f;
        glm::mat4 placement = glm::translate(glm::mat4(1.0f), drop.position + glm::vec3(0.0f, 0.15f + bob, 0.0f));
        placement = glm::rotate(placement, time + phase, glm::vec3(0.0f, 1.0f, 0.0f));
        out.push_back({ item->name, placement });
    }
}

} // namespace Volcano::EntityScene
