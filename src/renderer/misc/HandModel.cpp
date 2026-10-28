#include "HandModel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "ItemRenderer.hpp"
#include "../VulkanInit.hpp"
#include "../entity/EntityRenderer.hpp"
#include "../entity/models/EntityJsonModel.hpp"
#include "../../GlobalState.hpp"
#include "../../Logger.hpp"
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
constexpr float SWING_SECONDS = 0.3f; // Vanilla's 6-tick arm swing.
constexpr float UNITS_PER_BLOCK = 16.0f;
// An attack's swing plays in full at the raised hand height until the attack indicator is this full (0 to 1);
// then it is cut and the hand shows the indicator instead, low and rising as it charges.
constexpr float SWING_CUT_STRENGTH = 0.5f;
// Swings that aren't attacks (placing, mining) play out; the hand drops after this much of one.
constexpr float SWING_HANDOFF = 0.85f;

// Anything nearer than this in a hand-sized draw is always in front of the
// world, so the hands never clip into walls (vanilla clears depth instead).
constexpr float HAND_MAX_DEPTH = 0.01f;
constexpr float HAND_FOV_DEGREES = 80.0f; // Vanilla's default FOV.
constexpr float HELD_ITEM_PITCH_DEGREES = 0.0f; // Pitch of held items about their center; negative tips them forward.

struct ArmPush {
    glm::mat4 mvp;
    glm::vec4 shade; // x: brightness.
};

glm::mat4 RotateX(const glm::mat4& m, float degrees) { return glm::rotate(m, glm::radians(degrees), glm::vec3(1, 0, 0)); }
glm::mat4 RotateY(const glm::mat4& m, float degrees) { return glm::rotate(m, glm::radians(degrees), glm::vec3(0, 1, 0)); }
glm::mat4 RotateZ(const glm::mat4& m, float degrees) { return glm::rotate(m, glm::radians(degrees), glm::vec3(0, 0, 1)); }

// Vanilla's hand sway: the hands trail look changes (lag halves every tick)
// and tilt by a tenth of the gap. Angles are in Minecraft's convention.
struct HandSway {
    float yaw = 0.0f, pitch = 0.0f;
    float lagYaw = 0.0f, lagPitch = 0.0f;
    bool valid = false;
};
HandSway sway;

// 0..1 through the current swing, or 0 when idle. While a block is being
// mined the swing repeats until the break finishes or is cancelled.
float SwingProgress(GlobalState* state)
{
    return state->GetLocalSwingProgress();
}

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

void HandModel::Create(const Engine::ShaderLibrary& shaders)
{
    // Wide (Steve) arms: vanilla's box + part pivot, in model units. The left
    // arm has its own spot on the 64x64 skin sheet.
    BuildArm(rightArmMesh, glm::vec3(-8.0f, 0.0f, -2.0f), glm::vec2(40.0f, 16.0f));
    BuildArm(leftArmMesh, glm::vec3(4.0f, 0.0f, -2.0f), glm::vec2(32.0f, 48.0f));
    CreatePipeline(shaders);
    ready = true;
}

void HandModel::Destroy()
{
    if (!ready && pipeline == VK_NULL_HANDLE && rightArmMesh.vertices.buffer == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(GetDevice());
    DestroyPipeline();
    if (pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(GetDevice(), pipelineLayout, nullptr);
    pipelineLayout = VK_NULL_HANDLE;
    for (ArmMesh* mesh : { &rightArmMesh, &leftArmMesh })
    {
        DestroyBuffer(mesh->vertices);
        DestroyBuffer(mesh->indices);
        mesh->indexCount = 0;
    }
    ready = false;
}

void HandModel::DestroyPipeline()
{
    if (pipeline != VK_NULL_HANDLE) vkDestroyPipeline(GetDevice(), pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
}

void HandModel::DestroyBuffer(GpuBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE) vmaDestroyBuffer(vmaAllocator, buffer.buffer, buffer.allocation);
    buffer = GpuBuffer{};
}

HandModel::GpuBuffer HandModel::Upload(const void* data, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    GpuBuffer result;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vmaAllocator, &bufferInfo, &allocInfo, &result.buffer, &result.allocation, &info) != VK_SUCCESS)
    {
        throw std::runtime_error("[ERROR] HandModel: failed to allocate an arm buffer.");
    }
    std::memcpy(info.pMappedData, data, static_cast<size_t>(size));
    vmaFlushAllocation(vmaAllocator, result.allocation, 0, VK_WHOLE_SIZE);
    return result;
}

// One 4x12x4 box in vanilla's Y-down model space, 1 block = 16 units. The
// first-person transform chain works in that space, so no axis conversion.
void HandModel::BuildArm(ArmMesh& mesh, const glm::vec3& boxMin, const glm::vec2& uv)
{
    constexpr float TEXTURE_SIZE = 64.0f;
    const glm::vec3 size(4.0f, 12.0f, 4.0f);
    const glm::vec3 center = boxMin + size * 0.5f;

    std::vector<EntityJsonModel::Vertex> vertices;
    std::vector<uint32_t> indices;
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

    mesh.vertices = Upload(vertices.data(), vertices.size() * sizeof(EntityJsonModel::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    mesh.indices = Upload(indices.data(), indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    mesh.indexCount = static_cast<uint32_t>(indices.size());
}

void HandModel::CreatePipeline(const Engine::ShaderLibrary& shaders)
{
    VkShaderModule vertModule = CreateShaderModule(shaders, "hand.vert");
    VkShaderModule fragModule = VK_NULL_HANDLE;
    try
    {
        fragModule = CreateShaderModule(shaders, "hand.frag");
    }
    catch (...)
    {
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
        throw;
    }
    auto destroyModules = [&]
    {
        vkDestroyShaderModule(GetDevice(), fragModule, nullptr);
        vkDestroyShaderModule(GetDevice(), vertModule, nullptr);
    };

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragModule;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{ 0, sizeof(EntityJsonModel::Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EntityJsonModel::Vertex, position)) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(EntityJsonModel::Vertex, normal)) };
    attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<uint32_t>(offsetof(EntityJsonModel::Vertex, uv)) };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

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

    // No culling: the box is convex and depth-tested, and the transform chain
    // mixes handedness enough that winding is not worth getting wrong.
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &blendAttachment;

    if (pipelineLayout == VK_NULL_HANDLE)
    {
        VkDescriptorSetLayout skinLayout = EntityRenderer::GetSkinSetLayout();
        VkPushConstantRange pushRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ArmPush) };
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &skinLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(GetDevice(), &layoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS)
        {
            destroyModules();
            throw std::runtime_error("[ERROR] HandModel: failed to create the pipeline layout.");
        }
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
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = GetRenderPass();
    pipelineInfo.subpass = 0;
    VkResult result = vkCreateGraphicsPipelines(GetDevice(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    destroyModules();
    if (result != VK_SUCCESS)
    {
        pipeline = VK_NULL_HANDLE;
        throw std::runtime_error("[ERROR] HandModel: failed to create the graphics pipeline.");
    }
}

void HandModel::DrawArm(VkCommandBuffer cmd, const ArmMesh& mesh, const glm::mat4& mvp, float brightness)
{
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet skinSet = EntityRenderer::GetDefaultSkinSet();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &skinSet, 0, nullptr);
    VkDeviceSize vertexOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh.vertices.buffer, &vertexOffset);
    vkCmdBindIndexBuffer(cmd, mesh.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    ArmPush push{ mvp, glm::vec4(brightness, 0.0f, 0.0f, 0.0f) };
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
    vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
}

void HandModel::Record(Engine::FrameContext& frame)
{
    GlobalState* state = frame.state;
    if (!ready || state == nullptr || state->itemRenderer == nullptr) return;
    if (state->connectionPhase.load() != ConnectionPhase::Playing) return;
    if (state->localGameMode.load() == 3) return; // Spectators have no hands.
    if (!state->player->camera.IsFirstPerson()) return;
    if (std::get<bool>(state->config->Get("Graphics.BodyView", false))) return; // The body is drawn instead.

    float scale = static_cast<float>(std::get<uint32_t>(state->config->Get("Graphics.HandScale", uint32_t{100}))) / 100.0f;
    bool leftHanded = std::get<bool>(state->config->Get("Graphics.LeftHanded", false));

    ItemStack mainhand;
    ItemStack offhand;
    {
        std::lock_guard<std::mutex> lock(state->inventory.mutex);
        mainhand = state->inventory.hotbar.Get(state->inventory.selectedHotbarSlot);
        offhand = state->inventory.offhand.Get(0);
    }

    float attack = SwingProgress(state);

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
            int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (behavior.useTicks > 0)
            {
                float elapsedTicks = static_cast<float>(nowNs - startNs) * 1e-9f * 20.0f;
                usage = std::clamp(elapsedTicks / static_cast<float>(behavior.useTicks), 0.0f, 1.0f);
                ticksLeft = std::max(static_cast<float>(behavior.useTicks) - elapsedTicks, 0.0f);
                consuming = true;
            }
        }
    }

    VkCommandBuffer cmd = frame.commandBuffer;
    float width = static_cast<float>(frame.extent.width);
    float height = static_cast<float>(frame.extent.height);
    VkViewport handViewport{ 0.0f, 0.0f, width, height, 0.0f, HAND_MAX_DEPTH };
    vkCmdSetViewport(cmd, 0, 1, &handViewport);

    // Fixed FOV so the user's Graphics.FOV and sprint FOV effects don't
    // stretch the hand.
    glm::mat4 proj = glm::perspective(glm::radians(HAND_FOV_DEGREES), width / std::max(height, 1.0f), 0.05f, 1000.0f);
    proj[1][1] *= -1.0f; // Vulkan clip space has Y pointing down.
    proj = proj * frame.scene->viewBob; // The hand bobs with the view, as in vanilla.
    glm::mat4 scaling = glm::scale(glm::mat4(1.0f), glm::vec3(scale));

    for (int handIndex = 0; handIndex < 2; handIndex++)
    {
        bool isMain = handIndex == 0;
        const ItemStack& stack = isMain ? mainhand : offhand;
        bool rightArm = isMain != leftHanded;
        float invert = rightArm ? 1.0f : -1.0f;
        float handAttack = isMain ? shownAttack : 0.0f;
        float height = isMain ? mainHandHeight : 1.0f;

        if (stack.IsEmpty())
        {
            if (!isMain) continue;
            glm::mat4 m = swayMatrix * swimStroke(invert) * ArmTransform(invert, handAttack, height) * scaling;
            DrawArm(cmd, rightArm ? rightArmMesh : leftArmMesh, proj * m, handLight);
            continue;
        }

        const ItemRegistry::ItemTypeInfo* item = ItemRegistry::Lookup(stack.itemId);
        if (item == nullptr) continue;

        glm::mat4 m = swayMatrix * swimStroke(invert);
        if (isMain && consuming) m = EatTransform(m, invert, usage, ticksLeft);
        m = glm::translate(m, glm::vec3(invert * 0.56f, -0.52f + (height - 1.0f) * 0.6f, -0.72f));
        if (!(isMain && consuming) && handAttack > 0.0f) m = SwingItem(m, invert, handAttack);
        m = m * scaling;

        state->itemRenderer->RecordItem(cmd, item->name,
            rightArm ? "firstperson_righthand" : "firstperson_lefthand", m, proj,
            RotateX(glm::mat4(1.0f), HELD_ITEM_PITCH_DEGREES), 1.0f - handLight);
    }

    VkViewport fullViewport{ 0.0f, 0.0f, width, height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &fullViewport);
}

} // namespace Volcano
