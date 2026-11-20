#include "HandScene.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "../../ApplicationDeps.hpp"
import Volcano;
#include "../../Math.hpp"
#include "../../Player.hpp"
#include "../terrain/models/World.hpp"
#include "../../TickLoop.hpp"
#include "../../interaction/InteractionManager.hpp"
#include "../../inventory/ItemBehavior.hpp"
#include "../../inventory/ItemRegistry.hpp"

namespace Volcano {

namespace {

constexpr float PI = glm::pi<float>();
constexpr float UNITS_PER_BLOCK = 16.0f;
// An attack's swing plays in full at the raised hand height until the attack indicator is this full (0 to 1);
// then it is cut and the hand shows the indicator instead, low and rising as it charges.
constexpr float SWING_CUT_STRENGTH = 0.5f;
// Swings that aren't attacks (placing, mining) play out; the hand drops after this much of one.
constexpr float SWING_HANDOFF = 0.85f;
constexpr float HAND_FOV_DEGREES = 80.0f; // Vanilla's default FOV.
constexpr float HELD_ITEM_PITCH_DEGREES = 0.0f; // Pitch of held items about their center; negative tips them forward.

glm::mat4 RotateX(const glm::mat4& m, float degrees) { return glm::rotate(m, glm::radians(degrees), glm::vec3(1, 0, 0)); }
glm::mat4 RotateY(const glm::mat4& m, float degrees) { return glm::rotate(m, glm::radians(degrees), glm::vec3(0, 1, 0)); }
glm::mat4 RotateZ(const glm::mat4& m, float degrees) { return glm::rotate(m, glm::radians(degrees), glm::vec3(0, 0, 1)); }

// ItemInHandRenderer.applyEatTransform; `usage` runs 0 (just started) to 1.
glm::mat4 EatTransform(glm::mat4 m, float invert, float usage, float ticksLeft)
{
    float scaled = 1.0f - usage;
    if (scaled < 0.8f) m = glm::translate(m, glm::vec3(0.0f, std::abs(std::cos((ticksLeft + 1.0f) / 4.0f * PI) * 0.1f), 0.0f));
    float jiggle = 1.0f - std::pow(scaled, 27.0f);
    m = glm::translate(m, glm::vec3(jiggle * 0.6f * invert, jiggle * -0.5f, 0.0f));
    m = RotateY(m, invert * jiggle * 90.0f);
    m = RotateX(m, jiggle * 10.0f);
    m = RotateZ(m, invert * jiggle * 30.0f);
    return m;
}

// ItemInHandRenderer.swingArm: the item's swing, before the attack rotations.
glm::mat4 SwingItem(glm::mat4 m, float invert, float attack)
{
    float sq = std::sqrt(attack);
    m = glm::translate(m, glm::vec3(invert * -0.4f * std::sin(sq * PI), 0.2f * std::sin(sq * 2.0f * PI), -0.2f * std::sin(attack * PI)));
    m = RotateY(m, invert * (45.0f + std::sin(attack * attack * PI) * -20.0f));
    m = RotateZ(m, invert * std::sin(sq * PI) * -20.0f);
    m = RotateX(m, std::sin(sq * PI) * -80.0f);
    m = RotateY(m, invert * -45.0f);
    return m;
}

// ItemInHandRenderer.renderPlayerArm, up to where the arm model draws.
glm::mat4 ArmTransform(float invert, float attack, float height)
{
    float sq = std::sqrt(attack);
    glm::mat4 m(1.0f);
    m = glm::translate(m, glm::vec3(invert * (-0.3f * std::sin(sq * PI) + 0.64000005f),
        0.4f * std::sin(sq * 2.0f * PI) - 0.6f + (height - 1.0f) * 0.6f, -0.4f * std::sin(attack * PI) - 0.71999997f));
    m = RotateY(m, invert * 45.0f);
    m = RotateY(m, invert * std::sin(sq * PI) * 70.0f);
    m = RotateZ(m, invert * std::sin(attack * attack * PI) * -20.0f);
    m = glm::translate(m, glm::vec3(invert * -1.0f, 3.6f, 3.5f));
    m = RotateZ(m, invert * 120.0f);
    m = RotateX(m, 200.0f);
    m = RotateY(m, invert * -135.0f);
    m = glm::translate(m, glm::vec3(invert * 5.6f, 0.0f, 0.0f));
    return m;
}

} // namespace

void HandScene::BuildArmGeometry(const glm::vec3& boxMin, const glm::vec2& uv,
    std::vector<EntityJsonModel::Vertex>& vertices, std::vector<uint32_t>& indices)
{
    constexpr float TEXTURE_SIZE = 64.0f;
    const glm::vec3 size(4.0f, 12.0f, 4.0f);
    const glm::vec3 center = boxMin + size * 0.5f;

    for (const EntityJsonModel::detail::FaceDef& face : EntityJsonModel::detail::FACES)
    {
        glm::vec3 faceCenter = center + face.normal * (glm::dot(size, glm::abs(face.normal)) * 0.5f);
        float halfU = glm::dot(size, glm::abs(face.u)) * 0.5f;
        float halfV = glm::dot(size, glm::abs(face.v)) * 0.5f;

        glm::vec4 rect = EntityJsonModel::detail::FaceUVRect(face.name, uv, size);
        std::array<glm::vec2, 4> rawUV = EntityJsonModel::detail::FaceUVCorners(face.name, rect.x, rect.y, rect.z, rect.w);
        std::array<glm::vec3, 4> corners = {
            faceCenter - halfU * face.u - halfV * face.v,
            faceCenter + halfU * face.u - halfV * face.v,
            faceCenter + halfU * face.u + halfV * face.v,
            faceCenter - halfU * face.u + halfV * face.v,
        };

        uint32_t base = static_cast<uint32_t>(vertices.size());
        for (size_t i = 0; i < 4; i++)
        {
            vertices.push_back({ corners[i] / UNITS_PER_BLOCK, face.normal, rawUV[i] / TEXTURE_SIZE });
        }
        indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

bool HandScene::Update(const Engine::FrameContext& frame, bool flipY, Frame& out)
{
    Application* state = frame.state;
    out.arms.clear();
    out.items.clear();
    if (state == nullptr) return false;
    if (state->connectionPhase.load() != ConnectionPhase::Playing) return false;
    if (state->localGameMode.load() == 3) return false; // Spectators have no hands.
    if (!state->player->camera.IsFirstPerson()) return false;
    if (std::get<bool>(state->config->Get("Graphics.BodyView", false))) return false; // The body is drawn instead.

    float scale = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.HandScale", uint32_t{100}))) / 100.0f;
    bool leftHanded = std::get<bool>(state->config->Get("Graphics.LeftHanded", false));

    ItemStack mainhand;
    ItemStack offhand;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        mainhand = state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot);
        offhand = state->inventory.offhand.Get(0);
    }

    float attack = state->GetLocalSwingProgress(); // 0..1 through the swing, or 0 when idle (mining repeats it).

    // Vanilla's mainHandHeight: the main hand drops after a swing and rises
    // as the attack indicator fills, moving at most 0.4 per tick.
    int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    float elapsedTicks = lastRecordNs == 0 ? 0.0f
        : std::min(static_cast<float>(nowNs - lastRecordNs) * 1e-9f * 20.0f, 1.0f);
    lastRecordNs = nowNs;

    float viewYaw = -state->player->camera.GetYaw(), viewPitch = -state->player->camera.GetPitch();
    if (sway.valid)
    {
        sway.lagYaw += std::remainder(viewYaw - sway.yaw, 360.0f);
        sway.lagPitch += viewPitch - sway.pitch;
    }
    sway.yaw = viewYaw;
    sway.pitch = viewPitch;
    sway.valid = true;
    float decay = std::pow(0.5f, elapsedTicks);
    sway.lagYaw *= decay;
    sway.lagPitch *= decay;
    glm::mat4 swayMatrix = RotateY(RotateX(glm::mat4(1.0f), sway.lagPitch * 0.1f), sway.lagYaw * 0.1f);

    // Swimming: the hands dip and stroke in alternation, scaled by the swim ease.
    float swimAmount = state->tickLoop != nullptr ? state->tickLoop->GetMovementSnapshot().swimAmount : 0.0f;
    float strokePhase = static_cast<float>(nowNs % 2600000000LL) * 1e-9f * 6.2831853f / 1.3f;
    auto swimStroke = [&](float invert)
    {
        float stroke = std::sin(strokePhase) * invert;
        glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(invert * 0.04f * swimAmount, (-0.06f + 0.05f * stroke) * swimAmount, 0.0f));
        m = RotateZ(m, stroke * 8.0f * swimAmount);
        return RotateX(m, (-6.0f + 6.0f * stroke) * swimAmount);
    };

    // The partial tick keeps the charge (and so the hand height) smooth
    // between the 20 Hz ticks; the faster the attack speed, the quicker it fills.
    float partialTick = state->tickLoop != nullptr ? state->tickLoop->GetPartialTick() : 0.0f;
    float strength = state->interaction != nullptr ? state->interaction->GetAttackStrengthScale(partialTick) : 1.0f;
    float targetHeight = strength * strength * strength;

    // An attack resets the indicator, so a swing that starts with it low is one: it plays at the raised hand
    // until the indicator reaches SWING_CUT_STRENGTH (or the swing ends), then cuts to the indicator's height.
    if (attack <= 0.0f)
    {
        if (swingMode == SwingMode::Playing) mainHandHeight = targetHeight;
        swingMode = SwingMode::Idle;
    }
    else
    {
        if (swingMode == SwingMode::Idle)
        {
            bool mining = state->miningTicksRequired.load(std::memory_order_relaxed) > 0;
            swingMode = (strength < SWING_CUT_STRENGTH && !mining) ? SwingMode::Playing : SwingMode::Plain;
        }
        if (swingMode == SwingMode::Playing && strength >= SWING_CUT_STRENGTH)
        {
            swingMode = SwingMode::Cut;
            mainHandHeight = targetHeight;
        }
    }

    float shownAttack = attack;
    if (swingMode == SwingMode::Playing)
    {
        mainHandHeight = 1.0f;
    }
    else if (swingMode == SwingMode::Cut)
    {
        shownAttack = 0.0f;
        mainHandHeight = Math::MoveToward(mainHandHeight, targetHeight, 0.4f * elapsedTicks);
    }
    else if (attack <= 0.0f || attack >= SWING_HANDOFF)
    {
        // Held still during a plain swing; the drop and rise start once it's nearly done.
        mainHandHeight = Math::MoveToward(mainHandHeight, targetHeight, 0.4f * elapsedTicks);
    }

    // The server's light at the player's eye, through the terrain's curve (no
    // shadows or AO), eased so stepping through a doorway doesn't pop.
    float targetLight = 1.0f;
    if (state->world != nullptr)
    {
        const Engine::SceneView& scene = *frame.scene;
        glm::vec3 eye = scene.cameraPosition;
        LightSample sample = state->world->GetLightSample(static_cast<int>(std::floor(eye.x)),
            static_cast<int>(std::floor(eye.y)), static_cast<int>(std::floor(eye.z)));
        auto curve = [&](float level) { return level / (1.0f + scene.lightFalloff * (1.0f - level)); };
        float skyBrightness = glm::mix(0.05f, 1.0f, curve(static_cast<float>(sample.sky) / 15.0f) * scene.dayBrightness);
        float blockBrightness = glm::mix(0.05f, 1.0f, curve(static_cast<float>(sample.block) / 15.0f));
        float level = std::max(std::min(skyBrightness, 0.85f), blockBrightness);
        if (scene.lightingEnabled)
        {
            float inverted = 1.0f - level;
            float bright = 1.0f - inverted * inverted * inverted * inverted;
            targetLight = glm::mix(level, bright, scene.brightnessFactor);
        }
    }
    handLight += (targetLight - handLight) * std::min(elapsedTicks * 0.5f, 1.0f);

    // Eating and drinking progress, for the item bob.
    bool consuming = false;
    float usage = 0.0f;
    float ticksLeft = 0.0f;
    if (state->usingItem.load(std::memory_order_relaxed))
    {
        UseAction action = static_cast<UseAction>(state->usingItemAction.load(std::memory_order_relaxed));
        if (action == UseAction::Eat || action == UseAction::Drink)
        {
            ItemBehavior behavior = ItemBehaviorFor(ItemRegistry::Lookup(mainhand.itemId));
            int64_t startNs = state->useStartNs.load(std::memory_order_relaxed);
            if (behavior.useTicks > 0)
            {
                float usedTicks = static_cast<float>(nowNs - startNs) * 1e-9f * 20.0f;
                usage = std::clamp(usedTicks / static_cast<float>(behavior.useTicks), 0.0f, 1.0f);
                ticksLeft = std::max(static_cast<float>(behavior.useTicks) - usedTicks, 0.0f);
                consuming = true;
            }
        }
    }

    float width = static_cast<float>(frame.extent.width);
    float height = static_cast<float>(frame.extent.height);

    // Fixed FOV so the user's Graphics.FOV and sprint FOV effects don't
    // stretch the hand.
    glm::mat4 proj = glm::perspective(glm::radians(HAND_FOV_DEGREES), width / std::max(height, 1.0f), 0.05f, 1000.0f);
    if (flipY) proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.
    proj = proj * frame.scene->viewBob; // The hand bobs with the view, as in vanilla.
    glm::mat4 scaling = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

    out.proj = proj;
    out.handLight = handLight;

    for (int handIndex = 0; handIndex < 2; handIndex++)
    {
        bool isMain = handIndex == 0;
        const ItemStack& stack = isMain ? mainhand : offhand;
        bool rightArm = isMain != leftHanded;
        float invert = rightArm ? 1.0f : -1.0f;
        float handAttack = isMain ? shownAttack : 0.0f;
        float handHeight = isMain ? mainHandHeight : 1.0f;

        if (stack.IsEmpty())
        {
            if (!isMain) continue;
            glm::mat4 m = swayMatrix * swimStroke(invert) * ArmTransform(invert, handAttack, handHeight) * scaling;
            out.arms.push_back({ rightArm, proj * m });
            continue;
        }

        const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(stack.itemId);
        if (item == nullptr) continue;

        glm::mat4 m = swayMatrix * swimStroke(invert);
        if (isMain && consuming) m = EatTransform(m, invert, usage, ticksLeft);
        m = glm::translate(m, glm::vec3(invert * 0.56f, -0.52f + (handHeight - 1.0f) * 0.6f, -0.72f));
        if (!(isMain && consuming) && handAttack > 0.0f) m = SwingItem(m, invert, handAttack);
        m = m * scaling;

        Item drawn;
        drawn.name = item->name;
        drawn.context = rightArm ? "firstperson_righthand" : "firstperson_lefthand";
        drawn.placement = m;
        drawn.pivotRotation = RotateX(glm::mat4(1.0f), HELD_ITEM_PITCH_DEGREES);
        drawn.darkness = 1.0f - handLight;
        drawn.glint = stack.glint;
        out.items.push_back(std::move(drawn));
    }
    return true;
}

} // namespace Volcano
